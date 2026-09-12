// Loader side of the LAN multiplayer: patches the game image, exposes the
// stubs as HLE functions and gives the shared C core the handful of guest
// operations it cannot do itself (creating midp::String objects, pushing a
// sample into a GamePeer's history, shifting the game clock).

#include <unistd.h>

#include <cstdarg>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.h"
#include "cpu.h"
#include "guest.h"
#include "hle.h"
#include "mp_host.h"
#include "runtime.h"

extern "C" {
#include "mp/nfsmp.h"
}

namespace {

bool g_enabled = false;

// --- platform interface ----------------------------------------------------

void *plat_guest_ptr(uint32_t a) { return gptr(a); }

int64_t plat_utc_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t plat_mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void plat_set_clock_offset(int64_t off) { runtime::utc_offset_ms = off; }

// GamePeer_PushHistory takes the 0xa4 byte sample by value: r1..r3 hold the
// first twelve bytes, the rest follows on the stack.
void plat_push_sample(uint32_t peer, const void *sample) {
    const uint32_t *w = static_cast<const uint32_t *>(sample);
    std::vector<uint32_t> args;
    args.reserve(1 + MP_SAMPLE_SIZE / 4);
    args.push_back(peer);
    for (size_t i = 0; i < MP_SAMPLE_SIZE / 4; ++i) args.push_back(w[i]);
    Cpu::current().call(MP_ADDR_PUSH_HISTORY, args.data(), args.size());
}

uint32_t plat_call_guest(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return Cpu::current().call(fn, {a0, a1, a2, a3});
}

void plat_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf("%s", buf);
}

const struct mp_platform g_platform = {
    plat_guest_ptr, plat_utc_ms, plat_mono_ms, plat_set_clock_offset, plat_push_sample, plat_call_guest, plat_log,
};

// --- midp::String cache ----------------------------------------------------
//
// The four name stubs return a refcounted midp::String by sret. A fresh object
// starts at refcount 0, and the HUD releases its reference *before* it uses
// the raw pointer, so the transport has to hold one reference permanently and
// add one more for every call.

// One cache entry per place a name can appear, not per distinct text: host
// names come off the network, so a content-keyed cache would grow without
// bound and eventually hand out nothing.
enum : int { NAME_LOBBY = 0, NAME_HUD = 4, NAME_HOST = 8, NAME_REQUEST = 16, NAME_SLOTS = 17 };

struct NameSlot {
    std::string text;
    uint32_t obj = 0;
};
NameSlot g_names[NAME_SLOTS];

uint32_t guest_string_for(int key, const char *text) {
    if (!text || !*text || key < 0 || key >= NAME_SLOTS) return 0;
    NameSlot &s = g_names[key];
    if (s.obj && s.text == text) return s.obj;
    Cpu &cpu = Cpu::current();
    uint32_t obj = cpu.call(MP_ADDR_OPERATOR_NEW, {0x14});
    if (!obj) return 0;
    cpu.call(MP_ADDR_STRING_CTOR, {obj, guest::intern(text)});
    cpu.call(MP_ADDR_STRING_ADDREF, {obj});  // the reference we keep forever
    // The previous object is deliberately left alone. The HUD releases its own
    // reference *before* it uses the raw pointer, so releasing ours here could
    // hand the game a dangling one; 0x14 leaked bytes per rename is cheaper.
    s.text = text;
    s.obj = obj;
    return obj;
}

void return_name(uint32_t sret, int key, const char *text) {
    uint32_t obj = guest_string_for(key, text);
    if (obj) Cpu::current().call(MP_ADDR_STRING_ADDREF, {obj});  // the caller's
    guest::write32(sret, obj);
}

// --- HLE bodies ------------------------------------------------------------

void mp_HostLobbyReenter(uint32_t mpm) { mp_host_lobby_reenter(mpm); }
void mp_EnterLobbyState(uint32_t mpm) { mp_enter_lobby_state(mpm); }
void mp_HostCloseLobby(uint32_t mpm) { mp_host_close_lobby(mpm); }
void mp_HostRequestStart(uint32_t mpm) { mp_host_request_start(mpm); }
void mp_SetBrowsing(uint32_t mpm, uint32_t list) { mp_set_browsing(mpm, list); }
int32_t mp_GetHostCount(uint32_t mpm) { return mp_get_host_count(mpm); }
void mp_ConnectToHost(uint32_t mpm, int32_t i) { mp_connect_to_host(mpm, i); }
int32_t mp_AcceptPeer(uint32_t mpm) { return mp_accept_peer(mpm); }
void mp_DeclinePeer(uint32_t mpm) { mp_decline_peer(mpm); }
void mp_CancelConnect(uint32_t mpm) { mp_cancel_connect(mpm); }
void mp_ResumeBrowsing(uint32_t mpm) { mp_resume_browsing(mpm); }
void mp_HostStartCountdown(uint32_t mpm) { mp_host_start_countdown(mpm); }
void mp_BeginRaceHandshake(uint32_t mpm) { mp_begin_race_handshake(mpm); }
void mp_OnHostSelected(uint32_t mpm, int32_t i) { mp_on_host_selected(mpm, i); }
int32_t mp_GetPlayerCount(uint32_t mpm) { return mp_get_player_count(mpm); }
int32_t mp_GetPeerLobbyState(uint32_t mpm, int32_t i) { return mp_get_peer_lobby_state(mpm, i); }
int32_t mp_HasConnectionProblem(uint32_t mpm) { return mp_has_connection_problem(mpm); }
int32_t mp_HasPendingJoinRequest(uint32_t mpm) { return mp_has_pending_join_request(mpm); }
int32_t mp_HasRematchSyncFailed(uint32_t mpm) { return mp_has_rematch_sync_failed(mpm); }
int32_t mp_IsPeerReady(uint32_t mpm, int32_t i) { return mp_is_peer_ready(mpm, i); }
void mp_KickPeer(uint32_t mpm, int32_t i) { mp_kick_peer(mpm, i); }
void mp_SendRaceLoaded(uint32_t mpm) { mp_send_race_loaded(mpm); }
void mp_SendRaceAbort(uint32_t mpm, int32_t reason) { mp_send_race_abort(mpm, reason); }
void mp_Clear51c(uint32_t mpm) { mp_clear_51c(mpm); }

// r0 is the sret cell, r1 the MultiplayerManager, r2 the index.
void mp_GetHostName(uint32_t ret, uint32_t mpm, int32_t i) {
    return_name(ret, i >= 0 && i < 8 ? NAME_HOST + i : -1, mp_host_name(mpm, i));
}
void mp_GetRequestingPeerName(uint32_t ret, uint32_t mpm) {
    return_name(ret, NAME_REQUEST, mp_requesting_peer_name(mpm));
}
void mp_GetLobbyPeerName(uint32_t ret, uint32_t mpm, int32_t i) {
    return_name(ret, i >= 0 && i < MP_MAX_PEERS ? NAME_LOBBY + i : -1, mp_lobby_peer_name(mpm, i));
}
void mp_GetHudPeerName(uint32_t ret, uint32_t mpm, int32_t i) {
    return_name(ret, i >= 0 && i < MP_MAX_PEERS ? NAME_HUD + i : -1, mp_hud_peer_name(mpm, i));
}

// The two call sites of SendLocalRaceEnd mean different things; only the
// return address tells them apart (0x4a124f18 = finish line, 0x4a11b5e4 =
// leaving the race). See stubs-lobby-race.md 3.9.
void hle_SendLocalRaceEnd(Cpu &cpu) {
    uint32_t mpm = cpu.reg(0);
    int32_t pos = static_cast<int32_t>(cpu.reg(1));
    bool from_leave = cpu.reg(14) == 0x4a11b5e4;
    mp_send_local_race_end(mpm, pos, from_leave ? 1 : 0);
}

void hle_Screen2Enter(Cpu &cpu) { mp_screen2_enter(cpu.reg(0), cpu.reg(1)); }

void hle_State4to5(Cpu &cpu) {
    uint32_t mpm = cpu.reg(0);
    int32_t slot = static_cast<int32_t>(cpu.reg(1));
    int32_t score = static_cast<int32_t>(cpu.reg(2));
    int32_t a = static_cast<int32_t>(cpu.reg(3));
    int32_t b = static_cast<int32_t>(guest::read32(cpu.reg(13)));
    mp_on_state4to5(mpm, slot, score, a, b);
    cpu.reg(0) = cpu.call(0x4a0a898c, {mpm});
}

void hle_CountdownGate(Cpu &cpu) {
    uint32_t mpm = cpu.reg(0);
    mp_on_countdown_gate(mpm);
    cpu.reg(0) = cpu.call(0x4a0a7f74, {mpm});
}

// --- patching --------------------------------------------------------------

// Every write is recorded so a later failure can put the image back exactly as
// it was. Half a patch set is worse than none: the menu entry would appear,
// the lobby would open, and the stubs behind it would still be empty.
std::vector<std::pair<addr_t, uint32_t>> g_undo;

void note_patch(addr_t site, uint32_t original) { g_undo.emplace_back(site, original); }

void roll_back() {
    for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it) guest::write32(it->first, it->second);
    logf("[mp] rolled back %zu patched words", g_undo.size());
    g_undo.clear();
}

bool patch_call(addr_t site, uint32_t expect, const char *name) {
    uint32_t insn = guest::read32(site);
    if (insn != expect) {
        logf("[mp] call site %#x: expected %08x, found %08x", site, expect, insn);
        return false;
    }
    addr_t target = hle::stub_for(name);
    int64_t delta = (static_cast<int64_t>(target) - (static_cast<int64_t>(site) + 8)) >> 2;
    if (delta < -(1 << 23) || delta >= (1 << 23)) {
        logf("[mp] call site %#x: stub %s out of branch range", site, name);
        return false;
    }
    note_patch(site, insn);
    guest::write32(site, 0xeb000000 | (static_cast<uint32_t>(delta) & 0xffffff));  // bl stub
    return true;
}

void register_all() {
    hle::reg("mp_HostLobbyReenter", HLE_WRAP(mp_HostLobbyReenter));
    hle::reg("mp_EnterLobbyState", HLE_WRAP(mp_EnterLobbyState));
    hle::reg("mp_HostCloseLobby", HLE_WRAP(mp_HostCloseLobby));
    hle::reg("mp_HostRequestStart", HLE_WRAP(mp_HostRequestStart));
    hle::reg("mp_SetBrowsing", HLE_WRAP(mp_SetBrowsing));
    hle::reg("mp_GetHostCount", HLE_WRAP(mp_GetHostCount));
    hle::reg("mp_GetHostName", HLE_WRAP(mp_GetHostName));
    hle::reg("mp_ConnectToHost", HLE_WRAP(mp_ConnectToHost));
    hle::reg("mp_GetRequestingPeerName", HLE_WRAP(mp_GetRequestingPeerName));
    hle::reg("mp_AcceptPeer", HLE_WRAP(mp_AcceptPeer));
    hle::reg("mp_DeclinePeer", HLE_WRAP(mp_DeclinePeer));
    hle::reg("mp_CancelConnect", HLE_WRAP(mp_CancelConnect));
    hle::reg("mp_ResumeBrowsing", HLE_WRAP(mp_ResumeBrowsing));
    hle::reg("mp_HostStartCountdown", HLE_WRAP(mp_HostStartCountdown));
    hle::reg("mp_BeginRaceHandshake", HLE_WRAP(mp_BeginRaceHandshake));
    hle::reg("mp_OnHostSelected", HLE_WRAP(mp_OnHostSelected));
    hle::reg("mp_GetPlayerCount", HLE_WRAP(mp_GetPlayerCount));
    hle::reg("mp_GetLobbyPeerName", HLE_WRAP(mp_GetLobbyPeerName));
    hle::reg("mp_GetPeerLobbyState", HLE_WRAP(mp_GetPeerLobbyState));
    hle::reg("mp_HasConnectionProblem", HLE_WRAP(mp_HasConnectionProblem));
    hle::reg("mp_HasPendingJoinRequest", HLE_WRAP(mp_HasPendingJoinRequest));
    hle::reg("mp_HasRematchSyncFailed", HLE_WRAP(mp_HasRematchSyncFailed));
    hle::reg("mp_IsPeerReady", HLE_WRAP(mp_IsPeerReady));
    hle::reg("mp_KickPeer", HLE_WRAP(mp_KickPeer));
    hle::reg("mp_SendRaceLoaded", HLE_WRAP(mp_SendRaceLoaded));
    hle::reg("mp_GetHudPeerName", HLE_WRAP(mp_GetHudPeerName));
    hle::reg("mp_SendRaceAbort", HLE_WRAP(mp_SendRaceAbort));
    hle::reg("mp_Clear51c", HLE_WRAP(mp_Clear51c));
    hle::reg("mp_SendLocalRaceEnd", hle_SendLocalRaceEnd);
    hle::reg("mp_Screen2Enter", hle_Screen2Enter);
    hle::reg("mp_State4to5", hle_State4to5);
    hle::reg("mp_CountdownGate", hle_CountdownGate);
}

void pump() {
    if (!g_enabled) return;
    mp_pump(guest::read32(MP_ADDR_MPM_GLOBAL));
}

}  // namespace

namespace mp {

void init() {
    if (config::get_int("s3e", "LanMultiplayer", 1) == 0) {
        logf("[mp] disabled by configuration");
        return;
    }
    register_all();

    int failed = 0;
    g_undo.clear();
    for (size_t i = 0; i < mp_code_patch_count && !failed; ++i) {
        const mp_patch &p = mp_code_patches[i];
        if (hle::patch_bytes(p.addr, reinterpret_cast<const uint8_t *>(&p.expect),
                             reinterpret_cast<const uint8_t *>(&p.value), 4)) {
            note_patch(p.addr, p.expect);
        } else {
            logf("[mp] patch failed: %s", p.what);
            ++failed;
        }
    }
    for (size_t i = 0; i < mp_stub_count && !failed; ++i) {
        const mp_stub &s = mp_stubs[i];
        if (hle::patch_arm_function(s.addr, s.expect, s.name)) note_patch(s.addr, s.expect);
        else ++failed;
    }
    for (size_t i = 0; i < mp_hook_count && !failed; ++i)
        if (!patch_call(mp_hooks[i].addr, mp_hooks[i].expect, mp_hooks[i].name)) ++failed;
    if (failed) {
        // Put the image back byte for byte and leave multiplayer off, rather
        // than run the game on a half-patched image.
        roll_back();
        logf("[mp] a patch did not apply, multiplayer stays off");
        return;
    }
    g_undo.clear();

    std::string name;
    if (const char *env = getenv("NFS_MP_NAME")) name = env;
    if (name.empty())
        if (auto cfg = config::get("s3e", "LanPlayerName")) name = *cfg;
    if (name.empty()) {
        char host[64] = {0};
        if (gethostname(host, sizeof host - 1) == 0 && host[0]) name = host;
    }
    if (name.empty()) name = "Player";
    if (name.size() >= MP_NAME_MAX) name.resize(MP_NAME_MAX - 1);

    mp_init(&g_platform, name.c_str());
    g_enabled = true;
    runtime::pump_hook = pump;
    logf("[mp] LAN multiplayer active");
}

void shutdown() {
    if (!g_enabled) return;
    runtime::pump_hook = nullptr;
    g_enabled = false;
    mp_shutdown();
}

}  // namespace mp
