# MeeGo/N9: LAN-Multiplayer per LD_PRELOAD

Die Plattformschicht fuer den **originalen** Loader `/opt/usr/bin/ea-mobile-nfsshift`
(Harmattan 1.2, armel).  Der plattformneutrale Kern liegt in `src/mp/`; hier steht nur,
was Gastspeicher, Gastcode und Loader-Interna beruehrt.

| Datei | Zweck |
|---|---|
| `preload.c` | `mprotect`-Interposition, Image-Discovery, Patchlauf, Veneers, Trampolin-Hooks, `struct mp_platform`, die 29 Stub-Thunks und die 3 bl-Hook-Thunks |
| `stub_table_dummy.c` | **Attrappe** von `mp_stubs`/`mp_hooks`/`mp_code_patches`; wird nur gebaut, wenn `src/mp/patches.c` fehlt |
| `build.sh` | baut `libnfsmp.so` mit MADDE auf dem Build-Rechner, holt sie nach `build/meego/` |
| `startup-mp.sh` | Startskript fuer das Geraet (setzt `LD_PRELOAD`, laesst die Originaldateien in Ruhe) |
| `test/` | qemu-Test: `fakeloader.c` baut die Loader-Speicherlage nach, `core_dummy.c` ersetzt den Protokollkern, `run-test.sh` faehrt beides |

## Bauen

    ./meego/build.sh            # -> build/meego/libnfsmp.so
    ./meego/test/run-test.sh    # qemu-arm, Exitcode 0 = alles gruen

Beide Skripte arbeiten ueber `ssh` auf dem Build-Rechner (`BUILD_HOST`, Standard
`sebastian@192.168.1.21`) und legen alles unter `/tmp` ab.  Toolchain ist der
MADDE-Wrapper `~/QtSDK/Madde/targets/harmattan_10.2011.34-1_rt1.2/bin/gcc`
(gcc 4.4.1, `arm-none-linux-gnueabi`), der Sysroot und `specs` selbst anhaengt.
Flags: `-shared -fPIC -O2 -Wall -Wextra -std=gnu99 -marm`, Bibliotheken
`-ldl -lpthread -lrt`.

`build.sh` waehlt die Kernquellen selbst: alle `src/mp/*.c`; fehlen die Tabellen,
springt `stub_table_dummy.c` ein, fehlt der Kern, `test/core_dummy.c` — jeweils mit
Warnung.

## Ablauf zur Laufzeit

1. **Konstruktor**: reserviert `[0x4a000000, +0x202000)` mit `PROT_NONE`, damit keine
   Heap-Adresse im Gastfenster landet (siehe Adressumrechnung unten).
2. **`mprotect`-Interposition**: der Loader ruft genau einmal
   `mprotect(code, 0x1c6000, PROT_READ|PROT_EXEC)` — nach Relokation und Trampolinbau,
   vor dem Entrypoint.  Wir machen daraus RWX und patchen sofort; der loadereigene
   Cacheflush folgt unmittelbar.
3. **Image**: `*(void**)0x0007c328`, geprueft auf Magic `XE3U`, Basis `0x4a000000`,
   `split == 0x1c1078` und Trampolinformat.
4. **Patchen**: `mp_code_patches` (Wort fuer Wort, `expect` wird geprueft),
   `mp_stubs` (>= 8 Byte inline `ldr pc,[pc,#-4]`, 4 Byte `b <Veneer>`),
   `mp_hooks` (`bl <Veneer>`).  Veneers liegen im freien RWX-Schwanz hinter dem
   Thunk-Pool: 152 von 3992 Byte belegt.
5. **Trampoline**: Export 336 `s3eDeviceYield` -> Frame-Pump (`mp_pump`),
   Export 490 `s3eTimerGetUTC` -> Uhrenversatz.  Beide Hooks laufen hinter dem
   Stack-Switch-Thunk, also auf dem grossen Loader-Stack.
6. **`mp_init`** mit dem Spielernamen aus `NFSMP_NAME`.

## Adressumrechnung

Der `.s3e` liegt an einer Laufzeitbasis, der Kern rechnet in Gastadressen:

    host = (off < split) ? code + off : data + (off - split),  off = guest - 0x4a000000

Alles ausserhalb des Images — `MultiplayerManager`, `GamePeer`, `midp::String`, `Flow`,
also Spiel-Heap — wird **1:1** durchgereicht.  Eindeutig ist das nur, weil der
Konstruktor das Gastfenster reserviert; schlaegt das fehl, warnt die Bibliothek.

## ABI: softfp gegen hardfp

Der `.s3e`-Code ist softfp, diese `.so` ist hardfp (die MADDE-`specs` erzwingen
`-mfloat-abi=hard`, ein reiner softfp-Bau scheitert am Linken).  Fuer reine
Integer-/Zeigersignaturen sind beide Konventionen bitgleich — und genau solche sind
alle Ein- und Ausgaenge hier: die 29 Stubs, die 3 Hooks, `s3eDeviceYield(uint32)`,
`s3eTimerGetUTC() -> int64` (r0/r1), `operator new`, `String::String(const char*)`,
`addRef`, `GamePeer_PushHistory` und `call_guest`.  Erst wenn eine vom Spielcode
gerufene Funktion `float`/`double` entgegennimmt oder zurueckgibt, wird der
Unterschied sichtbar; dann braucht dieser eine Prototyp
`__attribute__((pcs("aapcs")))` (von gcc 4.4.1 unterstuetzt).  Die betroffenen
Stellen sind in `preload.c` mit `SOFTFP` markiert.

## Auf dem Geraet

    # Developer Mode, als root
    install -d /opt/nfsshift-mp
    install -m644 libnfsmp.so   /opt/nfsshift-mp/
    install -m755 startup-mp.sh /opt/nfsshift-mp/
    /opt/nfsshift-mp/startup-mp.sh

Erwartet auf stderr: `[nfsmp] Image: code=... pad=...`, danach
`Wort-Patches: 15 von 15`, `Stub-Umleitungen: 29 von 29`, `bl-Hooks: 3 von 3`.
Meldet `ld.so` dagegen `object '...' from LD_PRELOAD cannot be preloaded`, greift
das Preload nicht (Ursache pruefen: Architektur, absoluter Pfad, `AT_SECURE`).
Die Originaldateien aus `digsigsums` duerfen nicht veraendert werden; das
Paketieren als eigenstaendiges `.deb` beschreibt `scratchpad/wf1/meego-runtime-patch.md` §8.2.
