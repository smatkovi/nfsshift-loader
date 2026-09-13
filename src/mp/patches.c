// Every change the LAN multiplayer makes to the game image, in one place.
//
// None of the patched ranges carries a relocation (checked against the .s3e
// fixup sections), so the bytes are identical on Sailfish OS, Android and
// MeeGo; only the way the platform writes them differs.
//
// Sources: scratchpad/wf1/menu-patch.md (menu entry), race-sync.md (peer
// index), stubs-lobby-race.md (kill switch, lobby car column),
// mpm-fields.md (connection type).

#include "nfsmp.h"

const struct mp_patch mp_code_patches[] = {
    // Race submenu (SceneMenu state 22): raise the list capacity by one and
    // rewrite the second add block into "Multiplayer" (text 0x8af, icon 0xba).
    {0x4a1489e8, 0xe3a05003, 0xe3a05004, "menu: capacity 3 -> 4"},
    {0x4a148a04, 0x03a05002, 0x03a05003, "menu: capacity 2 -> 3"},
    {0x4a148adc, 0xe5945004, 0xe3a010af, "menu: mov r1,#0xaf"},
    {0x4a148ae0, 0xe1a00005, 0xe3a030ba, "menu: mov r3,#0xba"},
    {0x4a148ae4, 0xebfefb53, 0xe28420a4, "menu: add r2,r4,#0xa4"},
    {0x4a148ae8, 0xe1a06000, 0xe2811b02, "menu: add r1,r1,#0x800"},
    {0x4a148aec, 0xe1a00005, 0xe2840090, "menu: add r0,r4,#0x90"},
    {0x4a148af0, 0xebfefa84, 0xebfec7f8, "menu: bl AddMenuItem"},
    {0x4a148af4, 0xe0900006, 0xe3580002, "menu: cmp r8,#2"},

    // Flow::EnterScreen screen 2: hand r4 (the flow) to our hook, which then
    // replaces the `MPM->error = 17` store at 0x4a08dba4.
    {0x4a08dba0, 0xe3a01011, 0xe1a01004, "screen 2: mov r1,r4"},

    // Connection type 2 is the Wi-Fi branch: four rows in the host list, a
    // kick column, and error 16 does not throw the player out. It also keeps
    // Flow_Start from flipping Flow+0x861 back to the Bluetooth layout, since
    // that is recomputed as (MPM+4 == 1).
    {0x4a08dbd0, 0x13a05001, 0x13a05002, "connection type = 2 (screen 4)"},
    {0x4a08dc88, 0x13a05001, 0x13a05002, "connection type = 2 (screen 5)"},
    {0x4a08ddcc, 0x13a01001, 0x13a01002, "connection type = 2 (screen 3)"},

    // TrackObject::SetPeerIndex was compiled down to `bx lr`, which leaves
    // every network car with peer index 0: wrong models, names, scores and
    // collision partners as soon as three players race.
    {0x4a1101e4, 0xebfd82c4, 0xe580101c, "TrackObject::SetPeerIndex"},

    // MPM_GetLocalPeerField04 overwrites its index argument, so the lobby
    // shows the local car in every row. Dropping the load fixes the column.
    {0x4a0a8160, 0xe5901010, 0xe1a00000, "lobby car column: keep the index"},

    // Screen element lookup (0x4a0a2fc4): `index < count` is a signed compare,
    // so index -1 -- which the function means to ignore, see the
    // `cmn r1,#1; bxeq lr` right behind it -- falls through to array[-1]. The
    // lobby build loop at 0x4a08d1f4 feeds it -1 for elements the current
    // layout does not have, and on a screen without any element list that is
    // a read of 0xfffffffc: the N9 died right there when "ready" was tapped.
    // Unsigned: -1 is out of range and takes the path that returns.
    {0x4a0a2fcc, 0xda000005, 0x9a000005, "element lookup: ble -> bls"},
};
const size_t mp_code_patch_count = sizeof mp_code_patches / sizeof mp_code_patches[0];

// The 28 empty MultiplayerManager methods plus MPM_Clear51c, which is a real
// function we reimplement because it is the only "the local player pressed
// Ready" signal that fires for host and client alike.
const struct mp_stub mp_stubs[] = {
    {0x4a0a7ec8, 4, 0xe12fff1e, "mp_HostLobbyReenter"},
    {0x4a0a7c28, 4, 0xe12fff1e, "mp_EnterLobbyState"},
    {0x4a0a7cb8, 4, 0xe12fff1e, "mp_HostCloseLobby"},
    {0x4a0a8060, 4, 0xe12fff1e, "mp_HostRequestStart"},
    {0x4a0a7ea4, 4, 0xe12fff1e, "mp_SetBrowsing"},
    {0x4a0a7eac, 8, 0xe3a00000, "mp_GetHostCount"},
    {0x4a0a7e8c, 12, 0xe3a01000, "mp_GetHostName"},
    {0x4a0a7ef8, 4, 0xe12fff1e, "mp_ConnectToHost"},
    {0x4a0a7e78, 12, 0xe3a01000, "mp_GetRequestingPeerName"},
    {0x4a0a7ffc, 8, 0xe3a00000, "mp_AcceptPeer"},
    {0x4a0a7ea8, 4, 0xe12fff1e, "mp_DeclinePeer"},
    {0x4a0a8004, 4, 0xe12fff1e, "mp_CancelConnect"},
    {0x4a0a7e54, 4, 0xe12fff1e, "mp_ResumeBrowsing"},
    {0x4a0a7cb4, 4, 0xe12fff1e, "mp_HostStartCountdown"},
    {0x4a0a81e0, 4, 0xe12fff1e, "mp_BeginRaceHandshake"},
    {0x4a0a803c, 4, 0xe12fff1e, "mp_OnHostSelected"},
    {0x4a0a8064, 8, 0xe3a00000, "mp_GetPlayerCount"},
    {0x4a0a7c64, 12, 0xe3a01000, "mp_GetLobbyPeerName"},
    {0x4a0a7e84, 8, 0xe3a00003, "mp_GetPeerLobbyState"},
    {0x4a0a81ac, 8, 0xe3a00000, "mp_HasConnectionProblem"},
    {0x4a0a7cac, 8, 0xe3a00000, "mp_HasPendingJoinRequest"},
    {0x4a0a81e4, 8, 0xe3a00000, "mp_HasRematchSyncFailed"},
    {0x4a0a8ad0, 8, 0xe3a00000, "mp_IsPeerReady"},
    {0x4a0a8988, 4, 0xe12fff1e, "mp_KickPeer"},
    {0x4a0a7f10, 4, 0xe12fff1e, "mp_SendLocalRaceEnd"},
    {0x4a0a7ea0, 4, 0xe12fff1e, "mp_SendRaceLoaded"},
    {0x4a0a806c, 12, 0xe3a01000, "mp_GetHudPeerName"},
    {0x4a0a89a0, 4, 0xe12fff1e, "mp_SendRaceAbort"},
    {0x4a0a8528, 12, 0xe3a01000, "mp_Clear51c"},
};
const size_t mp_stub_count = sizeof mp_stubs / sizeof mp_stubs[0];

// Call sites we take over while keeping `bl` semantics; `orig` is the function
// the replacement still has to run.
const struct mp_hook mp_hooks[] = {
    // Flow::EnterScreen screen 2: kill switch off, Host/Join selector on.
    {0x4a08dba4, 0xe5801524, 0, "mp_Screen2Enter"},
    // MPM_State4to5 drops the result values the game passes it; we keep them.
    {0x4a11fab8, 0xebfe23b3, 0x4a0a898c, "mp_State4to5"},
    // MPM_UpdateCountdown inside SceneGame race state 6: the moment this
    // device is loaded, through the intro and waiting for the start signal.
    {0x4a12c80c, 0xebfdedd8, 0x4a0a7f74, "mp_CountdownGate"},
};
const size_t mp_hook_count = sizeof mp_hooks / sizeof mp_hooks[0];
