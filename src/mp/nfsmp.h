// LAN multiplayer for NFS Shift: protocol core and game glue.
//
// This code is shared by all three platforms (Sailfish OS and Android inside
// the loader, MeeGo inside an LD_PRELOAD library). It is plain C11 and never
// calls guest code directly; everything that needs the guest goes through
// mp_platform, which the loader or the preload library fills in.
#ifndef NFSMP_H
#define NFSMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MAX_PEERS 4
#define MP_NAME_MAX 16
#define MP_CAR_WORDS 13
#define MP_SAMPLE_SIZE 0xa4
#define MP_SAMPLE_WIRE 0x98  // bytes 0x98..0xa3 are uninitialised stack
#define MP_DISCOVERY_PORT 45470
#define MP_SESSION_PORT 45471
#define MP_PROTOCOL_VERSION 1

// ---------------------------------------------------------------------------
// Platform interface

struct mp_platform {
    // Translate a guest address to a host pointer (identity on our loader).
    void *(*guest_ptr)(uint32_t guest_addr);

    // Raw wall clock in milliseconds, the same source the game's
    // s3eTimerGetUTC reads, *without* the offset we install below.
    int64_t (*utc_ms)(void);

    // Monotonic milliseconds for the transport's own timers, never shifted.
    int64_t (*mono_ms)(void);

    // Install the offset the game's s3eTimerGetUTC has to add. Absolute, not
    // cumulative; the core remembers the value it last passed.
    void (*set_clock_offset)(int64_t offset_ms);

    // Call the guest's GamePeer_PushHistory(peer, sample) (0x4a0a8610) with a
    // MP_SAMPLE_SIZE byte sample passed by value.
    void (*push_sample)(uint32_t peer_addr, const void *sample);

    // Run a guest function with up to four integer arguments and return r0.
    uint32_t (*call_guest)(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

    void (*log)(const char *fmt, ...);
};

// ---------------------------------------------------------------------------
// Lifecycle

// `player_name` is shown in the lobby and the HUD (ASCII, cut to MP_NAME_MAX-1).
void mp_init(const struct mp_platform *platform, const char *player_name);
void mp_shutdown(void);

// Called from the loader's yield (or from the s3eDeviceYield hook on MeeGo) as
// often as the game yields; does all socket I/O and all timeouts. `mpm` is the
// current MultiplayerManager instance or 0 while the game has none.
void mp_pump(uint32_t mpm);

// ---------------------------------------------------------------------------
// The stubs. Names follow stubs-session.md / stubs-lobby-race.md; the guest
// address of each one is in the comment. `mpm` is always the guest address of
// the MultiplayerManager (the sret stubs get it in r1, the rest in r0).

void mp_host_lobby_reenter(uint32_t mpm);              // 0x4a0a7ec8
void mp_enter_lobby_state(uint32_t mpm);               // 0x4a0a7c28
void mp_host_close_lobby(uint32_t mpm);                // 0x4a0a7cb8
void mp_host_request_start(uint32_t mpm);              // 0x4a0a8060
void mp_set_browsing(uint32_t mpm, uint32_t ui_list);  // 0x4a0a7ea4
int32_t mp_get_host_count(uint32_t mpm);               // 0x4a0a7eac
void mp_connect_to_host(uint32_t mpm, int32_t index);  // 0x4a0a7ef8
int32_t mp_accept_peer(uint32_t mpm);                  // 0x4a0a7ffc
void mp_decline_peer(uint32_t mpm);                    // 0x4a0a7ea8
void mp_cancel_connect(uint32_t mpm);                  // 0x4a0a8004
void mp_resume_browsing(uint32_t mpm);                 // 0x4a0a7e54
void mp_host_start_countdown(uint32_t mpm);            // 0x4a0a7cb4
void mp_begin_race_handshake(uint32_t mpm);            // 0x4a0a81e0
void mp_on_host_selected(uint32_t mpm, int32_t index); // 0x4a0a803c

int32_t mp_get_player_count(uint32_t mpm);                 // 0x4a0a8064
int32_t mp_get_peer_lobby_state(uint32_t mpm, int32_t i);  // 0x4a0a7e84
int32_t mp_has_connection_problem(uint32_t mpm);           // 0x4a0a81ac
int32_t mp_has_pending_join_request(uint32_t mpm);         // 0x4a0a7cac
int32_t mp_has_rematch_sync_failed(uint32_t mpm);          // 0x4a0a81e4
int32_t mp_is_peer_ready(uint32_t mpm, int32_t i);         // 0x4a0a8ad0
void mp_kick_peer(uint32_t mpm, int32_t i);                // 0x4a0a8988
void mp_send_race_loaded(uint32_t mpm);                    // 0x4a0a7ea0
void mp_send_race_abort(uint32_t mpm, int32_t reason);     // 0x4a0a89a0

// 0x4a0a7f10. `from_leave` distinguishes the two call sites: the finish line
// (0x4a124f14, position = car+0x398) from giving up (0x4a11b5e0, position = 0).
void mp_send_local_race_end(uint32_t mpm, int32_t position, int32_t from_leave);

// The four sret stubs return a midp::String, which only the platform layer can
// build. The core hands out plain Latin-1 C strings; NULL means "empty slot"
// and the platform then writes a null pointer into the sret cell.
const char *mp_host_name(uint32_t mpm, int32_t index);          // 0x4a0a7e8c
const char *mp_requesting_peer_name(uint32_t mpm);              // 0x4a0a7e78
const char *mp_lobby_peer_name(uint32_t mpm, int32_t i);        // 0x4a0a7c64
const char *mp_hud_peer_name(uint32_t mpm, int32_t i);          // 0x4a0a806c

// ---------------------------------------------------------------------------
// Hooks on real game functions.

// MPM_Clear51c 0x4a0a8528, the only "the local player pressed Ready" signal on
// the client. Does the original store itself.
void mp_clear_51c(uint32_t mpm);

// Flow::EnterScreen case screen 2 (0x4a08dba4). Replaces the unconditional
// `MPM->error = 17` kill switch and builds the Host/Join selector.
void mp_screen2_enter(uint32_t mpm, uint32_t flow);

// Call site of MPM_State4to5 (0x4a11fab8): the game drops the result values,
// we keep them. The platform runs the original function afterwards.
void mp_on_state4to5(uint32_t mpm, int32_t slot, int32_t score, int32_t a, int32_t b);

// Call site of MPM_UpdateCountdown inside SceneGame race state 6 (0x4a12c80c):
// proof that this device sits at the countdown gate. The platform runs the
// original function and returns its result.
void mp_on_countdown_gate(uint32_t mpm);

// ---------------------------------------------------------------------------
// Patch tables, shared by all platforms. `expect` is checked before writing.

struct mp_patch {
    uint32_t addr;
    uint32_t expect;
    uint32_t value;
    const char *what;
};

// Plain word patches (menu entry, peer index, connection type, lobby car column).
extern const struct mp_patch mp_code_patches[];
extern const size_t mp_code_patch_count;

// Function bodies that are replaced wholesale: the platform makes `addr` jump
// to its implementation of `name`. `size` is how many bytes are available.
struct mp_stub {
    uint32_t addr;
    uint8_t size;
    uint32_t expect;   // first word of the original body
    const char *name;
};
extern const struct mp_stub mp_stubs[];
extern const size_t mp_stub_count;

// Call sites that are redirected while keeping `bl` semantics: the platform
// writes `bl <impl of name>` and the implementation runs the original.
struct mp_hook {
    uint32_t addr;
    uint32_t expect;
    uint32_t orig;     // guest function the implementation has to run, 0 if none
    const char *name;
};
extern const struct mp_hook mp_hooks[];
extern const size_t mp_hook_count;

// Guest addresses the platform layer needs.
#define MP_GUEST_BASE 0x4a000000u
#define MP_GUEST_SPLIT 0x1c1078u
#define MP_ADDR_MPM_GLOBAL 0x4a1c2050u
#define MP_ADDR_PUSH_HISTORY 0x4a0a8610u
#define MP_ADDR_OPERATOR_NEW 0x4a16e8e4u
#define MP_ADDR_STRING_CTOR 0x4a0dbc30u
#define MP_ADDR_STRING_ADDREF 0x4a0cf99cu
#define MP_ADDR_SELECTOR_INIT 0x4a067d80u
#define MP_ADDR_SELECTOR_ADD 0x4a06809cu

// Text ids used by the Host/Join selector we build on screen 2.
#define MP_TEXT_HOST_JOIN 0xabau
#define MP_TEXT_HOST_GAME 0x9deu
#define MP_TEXT_JOIN_GAME 0x9ddu

#ifdef __cplusplus
}
#endif
#endif
