# NFS Shift LAN multiplayer

Up to 4 players on one Wi-Fi network, cross-play between Sailfish OS, Android and MeeGo
(Nokia N9). All devices run the same game build (MeeGo `NFSShift.s3e`, 1.0.20), so every
game-side structure is binary compatible.

The game still contains the complete multiplayer logic of the iPhone original; only the
transport layer (28 empty `MultiplayerManager` methods) and the menu entry were compiled
out. We reimplement the transport outside the game and drive the game's own data model.

Analysis this is based on: `scratchpad/wf1/{mpm-fields,stubs-session,stubs-lobby-race,
verify-*,flow-statemachine,race-sync,menu-patch,meego-runtime-patch,android-env,
packaging-lan-design}.md`; review of the first draft in `scratchpad/wf2/design-review.md`.

## 1. Components

| Component | Where it runs | Language |
|---|---|---|
| `src/mp/nfsmp.c` — transport: sockets, discovery, snapshot exchange, clock sync | all platforms | C11 |
| `src/mp/glue.c` — the stub bodies, the MPM/GamePeer fields, the session logic | all platforms | C11 |
| `src/mp/patches.c` — the three patch tables | all platforms | C11 |
| `src/hle_mp.cpp` — binds the core into the loader (HLE stubs, guest strings, pump) | Sailfish, Android | C++ |
| `meego/preload.c` — `LD_PRELOAD` patcher for the original N9 loader | MeeGo | C11 |

The core never allocates guest memory and never calls guest code directly. Everything that
needs the guest goes through `struct mp_platform` (`src/mp/nfsmp.h`), which the loader or
the preload library fills in: address translation, the wall clock, the clock offset,
`GamePeer_PushHistory`, a four-argument guest call, and logging.

Guest addresses are `uint32_t`. On our loader the guest is identity mapped; on MeeGo the
image lives at a run-time base, so the platform translates with
`code + (a - 0x4a000000)` below the split `0x1c1078` and `data + (a - 0x4a000000 - split)`
above it.

## 2. Network protocol

UDP only, two ports (`NFS_MP_PORT` moves our own pair when two instances share a machine):

| Purpose | Port |
|---|---|
| Discovery — the host answers broadcast probes | UDP 45470 |
| Session traffic — the host binds it, clients use an ephemeral port | UDP 45471 |

Every datagram starts with a 16-byte header: `"NFSS"`, protocol version, message type,
sender slot, flags, session id, and one type-specific argument. Anything else is dropped.

### 2.1 State snapshots instead of a message zoo

Reliability is deliberately dumb. The host mirrors its **complete** session state to each
client as one numbered snapshot, and each client mirrors its own complete state back. Both
sides repeat their snapshot every 150 ms until the peer acknowledges that revision, and at
least every 400 ms as a keepalive. A lost datagram therefore costs one retransmit interval
and can never leave the two sides disagreeing — the next snapshot carries everything again.

| Type | Direction | Payload |
|---|---|---|
| `DISCOVER` | client → broadcast:45470 | probe token |
| `ADVERT` | host → prober | token, session port, player count, name, race settings |
| `JOIN_REQ` | client → host | protocol version, player name |
| `JOIN_ACCEPT` / `JOIN_REJECT` | host → client | assigned slot / game error code |
| `SHARED` | host → client | the whole session: race settings, start release, t0/r0, abort counter, results, and per slot connected/ready/loaded/at-gate/finished, name, 13 car words, results |
| `MINE` | client → host | that client's own half of the same picture, plus its abort request |
| `ACK` | both | the revision that arrived |
| `SAMPLE` | any → host → all | slot, sequence, race time, 0x98 sample bytes |
| `PING` / `PONG` | client ↔ host | client send time, host time |
| `KICK`, `LEAVE` | — | — |

Discrete events ride along inside the snapshots as counters (`race_seq`, `abort_seq`,
`kick_mask`), so they survive packet loss without a separate retransmit path.

Sample relay runs through the host (star). At 15 Hz, 4 players and 176 bytes of payload
that is about 10 kB/s per device — irrelevant for Wi-Fi, and it keeps the firewall story
simple.

### 2.2 Clock synchronisation

Every multiplayer time in the game comes from `s3eTimerGetUTC` (verified: exactly one call
site, inside `GetTimeMs64` at 0x4a0dbf74). The client therefore never converts timestamps;
it shifts its own clock:

1. In the lobby the client sends a `PING` every 250 ms, later every 2 s.
2. `offset = host_time + rtt/2 - own_utc` for the sample with the smallest round trip.
3. The loader adds that offset inside `s3eTimerGetUTC` and only there — `s3eTimerGetMs`
   stays monotonic for animations.

On the loopback test this settles at ±1 ms. After that, `GetSyncedTime()`, the countdown
comparison against `T0` and every sample timestamp are in the same time base everywhere.

## 3. Mapping the game's session model

The host is always slot 0, clients get 1..3 in join order, and every device uses the same
slot numbers, so `peer[i]` means the same player everywhere. `MPM+0x10` is the local slot,
`MPM+0x14` is 0 (the host) on clients.

Fields the transport owns (from `mpm-fields.md` §12):

| Field | Meaning | When we set it |
|---|---|---|
| `+0x0c` | multiplayer active | when the session opens |
| `+0x08` | session state | 2 lobby, 5 local player finished, 6 results, 7 error (3/4 the game sets itself) |
| `+0x10` / `+0x14` | local slot / watched slot | on join |
| `+0x500..+0x50c` | race settings and their valid flag | host from the lobby, client from the snapshot |
| `+0x510` | final position | from the host's ranking |
| `+0x514` | start released | derived from everyone's ready flag |
| `+0x516`, `+0x539` | race announced, sync pulse | on entering screen 15 |
| `+0x51d`, `+0x538` | countdown armed, start time valid | with the start time |
| `+0x52c`, `+0x530/+0x534` | race clock base, `T0` | with the start time and on every resync |
| `+0x524` | error code 1..17 | connection loss, kick, decline, version mismatch |
| `+0x528`, `+0x53c` | remote abort pulse and reason | on an abort from another device |
| `+0x53a` | host list ready | when a host is selected on screen 3 |
| peer `+0x04..+0x34`, `+0x60` | car setup | from the snapshot |
| peer `+0x54/+0x58/+0x5c` | results | from the snapshot |
| peer `+0x62` | loaded | from the snapshot; cleared before every race |
| peer `+0x65` | connected — **also for the local player** | from the snapshot |
| peer `+0x66` | has samples | on the first sample |
| peer `+0x68` | grid and HUD slot | = slot index |
| peer vector `+0x80..+0x88` | sample history | through `GamePeer_PushHistory` |

Samples are pushed in strictly increasing `sample[0]` order; out-of-order datagrams are
dropped, never inserted, and nothing is pushed outside MPM state 3..5 (otherwise the guest
vector would grow without anybody popping from it).

## 4. Triggers — where the state actually changes

The Flow calls no stub at three of the points that matter, which is what made the first
draft unbuildable. The implementation covers them like this:

| Event | How we notice |
|---|---|
| Host opens the session | polled: `MPM+0x18 == 1 && MPM+0x50c == 1` while we are idle (screen 5 sets both and calls nothing) |
| Player is ready | `MPM_Clear51c` (0x4a0a8528) is reimplemented; it is the only call on both the host and the client path, in the menu and in the in-game lobby |
| Car setup changed | polled: `peer[local]+0x60` plus the 13 words |
| Race settings changed | polled: `MPM+0x500..+0x50c` on the host |
| Session ended (`MPM_Reset`) | polled: state went back to 0 behind our back |
| Rematch (`Rematch_6to2`) | polled: state went 6 → 2 behind our back |
| Local sample produced | polled: `MPM+0x520` changed, sample read from `peer[local]+0x8c` |
| Device is waiting for the start | the `MPM_UpdateCountdown` call site inside race state 6 (0x4a12c80c) |
| Local result | `SendLocalRaceEnd` (position events) and the `State4to5` call site (score events) |

## 5. Race start

The start time cannot be agreed in the lobby: loading the scene takes several seconds and
differs per device, so a `T0` picked before that would pass while slower devices are still
loading, and each of them would start its purely local 3-2-1 animation whenever it arrived.

Instead:

1. Screen 15 sets `+0x516` and `+0x539` locally, and the device leaves for the race.
2. Race state 1 reports "loaded" (`peer+0x62`), race state 2 waits for everybody.
3. Race state 6 freezes the countdown until MPM state becomes 3. The hooked
   `UpdateCountdown` call site tells us the device is sitting there.
4. Once every connected slot is at that gate, the host picks `T0 = now + 700 ms` and
   publishes it. Every device writes `+0x52c/+0x530/+0x534`, `+0x51d`, `+0x538`.
5. `UpdateCountdown` flips 2 → 3 at `T0` on all devices within a frame of each other.

The race clock does not start at `T0` — the game freezes it until then, plays the countdown
and only starts counting at "GO". How long that takes is not knowable in advance (measured:
4811 ms including the lead). The host therefore measures it once from its own first sample
and publishes the difference as the clock base `r0`; every device snaps onto the same line.
Afterwards each device compares its own sample timestamps against `(now - T0) + r0` and
re-arms `+0x538` if it has drifted more than 150 ms, at most once every 5 s.

## 6. Game code patches

All patch bytes are position independent (no relocations in the patched ranges, verified
against the `.s3e` relocation table), so they are identical on all three platforms.
`src/mp/patches.c` holds three tables and checks the original word before every write.

| Group | What |
|---|---|
| 9 words at 0x4a1489e8 … 0x4a148af4 | add "Multiplayer" (text 0x8af, icon 0xba) to the Race menu and raise the list capacity |
| 0x4a08dba0 | `mov r1,r4` — hands the Flow object to the screen-2 hook |
| 0x4a08dbd0, 0x4a08dc88, 0x4a08ddcc | connection type 2 (Wi-Fi): four host-list rows, kick column, and `Flow_Start` stops flipping the layout back |
| 0x4a1101e4 | `bl TrackObject::SetPeerIndex` → `str r1,[r0,#0x1c]`; without it every network car has peer index 0 |
| 0x4a0a8160 | drop the load that overwrites the index, so the lobby shows each player's own car |
| 29 stub bodies | branch to our implementation |
| 3 call sites | `bl` to our implementation, which then runs the original |

The screen-2 hook replaces the unconditional `MPM->error = 17` kill switch and builds the
Host/Join selector the shipped build never builds — without it screen 3 ("Join") is
unreachable and there can be no client at all.

## 7. Per-platform integration

### Sailfish OS / Android (our loader)

- `hle_mp.cpp` registers one HLE function per stub; `patch_arm_function` turns each stub
  body into a branch to the stub page at 0x4a400000 (3.6 MB away, inside branch range).
- The four `sret` name stubs get a cached `midp::String` per name, created through the
  guest's `operator new(0x14)`, `String::String(const char*)` and `addRef`. The transport
  keeps one reference forever and adds one per call, because the HUD releases its reference
  *before* it uses the raw pointer.
- The pump runs from `runtime::yield()`, several times per frame.
- Patches are applied after `s3e_load()` and before the entry point, so the JIT cache is
  still empty and needs no invalidation.

### MeeGo (original loader plus `LD_PRELOAD`)

- `meego/preload.c` interposes `mprotect`: the loader calls it exactly once with
  `PROT_READ|PROT_EXEC` after relocation and before the entry point. We turn that into RWX,
  patch, and let the loader's own cache flush cover us.
- The image structure is at the fixed address `*(void**)0x0007c328`; `+0xd8` is the code
  base, `+0xe0` the data base, `+0xf0` the import trampolines, `+0xf4` their size.
- 16 four-byte stubs branch into veneers placed in the 3992 unused RWX bytes at the end of
  the loader's code allocation (152 bytes are actually needed).
- Frame pump and clock: the trampoline target words of `s3eDeviceYield` (export 336) and
  `s3eTimerGetUTC` (export 490) are redirected.

## 8. Test status

1. **Two instances, one machine** — done. Discovery, join popup, accepted join,
   synchronised lobbies, ready, common countdown, identical `T0`, live sample exchange, the
   remote car visible and moving, HUD showing both player names and positions.
2. **Phone against PC** — open. Checks discovery over a real network and the clock sync
   across machines.
3. **Phone against phone / Android** — open, needs the user's devices.
4. **N9** — open; the `LD_PRELOAD` path is proven under qemu (45 checks) but never on
   hardware, and whether Aegis lets it through is unknown.

Acceptance for a first release: two devices find each other, agree on track and event,
start within a second of each other, and drive three laps with the remote cars moving
smoothly and a joint result screen at the end.
