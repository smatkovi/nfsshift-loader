/*
 * ###########################################################################
 * #   A T T R A P P E  —  P L A T Z H A L T E R .                            #
 * #   Wird NUR gebaut, wenn src/mp/patches.c fehlt.                          #
 * ###########################################################################
 *
 * mp_stubs[] / mp_hooks[] / mp_code_patches[] gehoeren in den Kern
 * (src/mp/patches.c).  Diese Datei existiert nur, damit meego/preload.c und
 * der qemu-Test auch ohne den Kern gebaut und ausgefuehrt werden koennen.
 *
 * meego/build.sh und meego/test/run-test.sh binden sie ausschliesslich dann
 * ein, wenn in src/mp/ keine Datei mp_stubs[] definiert, und warnen dabei.
 * Sobald patches.c da ist, ist patches.c massgeblich — Abweichungen dieser
 * Attrappe sind dann bedeutungslos.
 *
 * Herkunft der Werte (Stand: identisch zu src/mp/patches.c vom 2026-09-12):
 *   Stubadressen/-groessen/-ruempfe : verify-stubs-session.md §0.1,
 *                                     verify-stubs-lobby-race.md "Rumpf-Verifikation"
 *   Menuewoerter                    : menu-patch.md §7/§8.1
 *   Verbindungstyp                  : menu-patch.md §11
 *   SetPeerIndex                    : race-sync.md §534
 *   Hooks                           : verify-stubs-session.md §1.1 (Screen 2),
 *                                     race-sync.md §405 (State4to5),
 *                                     mpm-fields.md §147 (UpdateCountdown)
 */
#include "nfsmp.h"

#define BX_LR    0xe12fff1eu /* bx lr                      */
#define MOV_R0_0 0xe3a00000u /* mov r0,#0                  */
#define MOV_R0_3 0xe3a00003u /* mov r0,#3                  */
#define MOV_R1_0 0xe3a01000u /* mov r1,#0                  */

const struct mp_stub mp_stubs[] = {
    { 0x4a0a7ec8u,  4, BX_LR,    "mp_HostLobbyReenter" },
    { 0x4a0a7c28u,  4, BX_LR,    "mp_EnterLobbyState" },
    { 0x4a0a7cb8u,  4, BX_LR,    "mp_HostCloseLobby" },
    { 0x4a0a8060u,  4, BX_LR,    "mp_HostRequestStart" },
    { 0x4a0a7ea4u,  4, BX_LR,    "mp_SetBrowsing" },
    { 0x4a0a7eacu,  8, MOV_R0_0, "mp_GetHostCount" },
    { 0x4a0a7e8cu, 12, MOV_R1_0, "mp_GetHostName" },
    { 0x4a0a7ef8u,  4, BX_LR,    "mp_ConnectToHost" },
    { 0x4a0a7e78u, 12, MOV_R1_0, "mp_GetRequestingPeerName" },
    { 0x4a0a7ffcu,  8, MOV_R0_0, "mp_AcceptPeer" },
    { 0x4a0a7ea8u,  4, BX_LR,    "mp_DeclinePeer" },
    { 0x4a0a8004u,  4, BX_LR,    "mp_CancelConnect" },
    { 0x4a0a7e54u,  4, BX_LR,    "mp_ResumeBrowsing" },
    { 0x4a0a7cb4u,  4, BX_LR,    "mp_HostStartCountdown" },
    { 0x4a0a81e0u,  4, BX_LR,    "mp_BeginRaceHandshake" },
    { 0x4a0a803cu,  4, BX_LR,    "mp_OnHostSelected" },
    { 0x4a0a8064u,  8, MOV_R0_0, "mp_GetPlayerCount" },
    { 0x4a0a7c64u, 12, MOV_R1_0, "mp_GetLobbyPeerName" },
    { 0x4a0a7e84u,  8, MOV_R0_3, "mp_GetPeerLobbyState" },
    { 0x4a0a81acu,  8, MOV_R0_0, "mp_HasConnectionProblem" },
    { 0x4a0a7cacu,  8, MOV_R0_0, "mp_HasPendingJoinRequest" },
    { 0x4a0a81e4u,  8, MOV_R0_0, "mp_HasRematchSyncFailed" },
    { 0x4a0a8ad0u,  8, MOV_R0_0, "mp_IsPeerReady" },
    { 0x4a0a8988u,  4, BX_LR,    "mp_KickPeer" },
    { 0x4a0a7f10u,  4, BX_LR,    "mp_SendLocalRaceEnd" },
    { 0x4a0a7ea0u,  4, BX_LR,    "mp_SendRaceLoaded" },
    { 0x4a0a806cu, 12, MOV_R1_0, "mp_GetHudPeerName" },
    { 0x4a0a89a0u,  4, BX_LR,    "mp_SendRaceAbort" },
    /* echte Funktion, komplett ersetzt — 12 Byte, aber KEIN sret */
    { 0x4a0a8528u, 12, MOV_R1_0, "mp_Clear51c" },
};
const size_t mp_stub_count = sizeof mp_stubs / sizeof mp_stubs[0];

const struct mp_patch mp_code_patches[] = {
    { 0x4a1489e8u, 0xe3a05003u, 0xe3a05004u, "menu: capacity 3 -> 4" },
    { 0x4a148a04u, 0x03a05002u, 0x03a05003u, "menu: capacity 2 -> 3" },
    { 0x4a148adcu, 0xe5945004u, 0xe3a010afu, "menu: mov r1,#0xaf" },
    { 0x4a148ae0u, 0xe1a00005u, 0xe3a030bau, "menu: mov r3,#0xba" },
    { 0x4a148ae4u, 0xebfefb53u, 0xe28420a4u, "menu: add r2,r4,#0xa4" },
    { 0x4a148ae8u, 0xe1a06000u, 0xe2811b02u, "menu: add r1,r1,#0x800" },
    { 0x4a148aecu, 0xe1a00005u, 0xe2840090u, "menu: add r0,r4,#0x90" },
    { 0x4a148af0u, 0xebfefa84u, 0xebfec7f8u, "menu: bl AddMenuItem" },
    { 0x4a148af4u, 0xe0900006u, 0xe3580002u, "menu: cmp r8,#2" },
    { 0x4a08dba0u, 0xe3a01011u, 0xe1a01004u, "screen 2: mov r1,r4" },
    { 0x4a08dbccu, 0x03a05002u, 0x03a05001u, "connection type = 1 (screen 4)" },
    { 0x4a08dc84u, 0x03a05002u, 0x03a05001u, "connection type = 1 (screen 5)" },
    { 0x4a08ddc8u, 0x03a01002u, 0x03a01001u, "connection type = 1 (screen 3)" },
    { 0x4a1101e4u, 0xebfd82c4u, 0xe580101cu, "TrackObject::SetPeerIndex" },
    { 0x4a0a8160u, 0xe5901010u, 0xe1a00000u, "lobby car column: keep the index" },
};
const size_t mp_code_patch_count = sizeof mp_code_patches / sizeof mp_code_patches[0];

const struct mp_hook mp_hooks[] = {
    { 0x4a08dba4u, 0xe5801524u, 0u,          "mp_Screen2Enter" },
    { 0x4a11fab8u, 0xebfe23b3u, 0x4a0a898cu, "mp_State4to5" },
    { 0x4a12c80cu, 0xebfdedd8u, 0x4a0a7f74u, "mp_CountdownGate" },
};
const size_t mp_hook_count = sizeof mp_hooks / sizeof mp_hooks[0];
