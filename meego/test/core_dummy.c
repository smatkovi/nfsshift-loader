/*
 * meego/test/core_dummy.c — Kern-Attrappe fuer den qemu-Test.
 *
 * NUR FUER DEN TEST.  Ersetzt src/mp/{nfsmp,glue}.c, solange der echte Kern
 * parallel entsteht.  Jede Kernfunktion protokolliert, was sie bekommt, und
 * einige loesen zusaetzlich einen Rueckruf in die Plattformschnittstelle aus,
 * damit preload.c vollstaendig durchlaufen wird:
 *
 *   mp_connect_to_host()  -> set_clock_offset(), utc_ms(), mono_ms()
 *   mp_send_race_loaded() -> push_sample()  (GamePeer_PushHistory-ABI)
 *   die vier Namensfunktionen liefern Texte, aus denen preload.c
 *   midp::String-Objekte baut.
 */
#include <stdio.h>
#include <string.h>

#include "nfsmp.h"
#include "core_dummy.h"

static struct mpdummy_state g_st;
static const struct mp_platform *g_plat;

struct mpdummy_state *mpdummy_get(void) { return &g_st; }

static void hit(int id, uint32_t mpm, int32_t i)
{
    g_st.calls[id]++;
    g_st.total++;
    g_st.last_id = id;
    g_st.last_mpm = mpm;
    g_st.last_i = i;
    if (g_plat)
        g_plat->log("core: id=%d mpm=%08x i=%d", id, (unsigned)mpm, (int)i);
}

/* ---- Lebenszyklus ------------------------------------------------------- */
void mp_init(const struct mp_platform *platform, const char *player_name)
{
    g_plat = platform;
    g_st.init_calls++;
    snprintf(g_st.last_name, sizeof g_st.last_name, "%s",
             player_name ? player_name : "");
    if (g_plat)
        g_plat->log("core: mp_init('%s')", g_st.last_name);
}

void mp_shutdown(void) { g_plat = NULL; }

void mp_pump(uint32_t mpm)
{
    g_st.pump_calls++;
    g_st.pump_mpm = mpm;
}

/* ---- Session ------------------------------------------------------------ */
void mp_host_lobby_reenter(uint32_t m) { hit(D_HOST_LOBBY_REENTER, m, 0); }
void mp_enter_lobby_state(uint32_t m) { hit(D_ENTER_LOBBY_STATE, m, 0); }
void mp_host_close_lobby(uint32_t m) { hit(D_HOST_CLOSE_LOBBY, m, 0); }
void mp_host_request_start(uint32_t m) { hit(D_HOST_REQUEST_START, m, 0); }

void mp_set_browsing(uint32_t m, uint32_t ui_list)
{
    hit(D_SET_BROWSING, m, 0);
    g_st.last_ptr = ui_list;
}

int32_t mp_get_host_count(uint32_t m)
{
    hit(D_GET_HOST_COUNT, m, 0);
    return DUMMY_HOST_COUNT;
}

void mp_connect_to_host(uint32_t m, int32_t index)
{
    hit(D_CONNECT_TO_HOST, m, index);
    if (g_plat) {
        g_plat->set_clock_offset(DUMMY_CLOCK_OFFSET);
        g_st.utc_ms = g_plat->utc_ms();
        g_st.mono_ms = g_plat->mono_ms();
    }
}

int32_t mp_accept_peer(uint32_t m)
{
    hit(D_ACCEPT_PEER, m, 0);
    return 1;
}

void mp_decline_peer(uint32_t m) { hit(D_DECLINE_PEER, m, 0); }
void mp_cancel_connect(uint32_t m) { hit(D_CANCEL_CONNECT, m, 0); }
void mp_resume_browsing(uint32_t m) { hit(D_RESUME_BROWSING, m, 0); }
void mp_host_start_countdown(uint32_t m) { hit(D_HOST_START_COUNTDOWN, m, 0); }
void mp_begin_race_handshake(uint32_t m) { hit(D_BEGIN_RACE_HANDSHAKE, m, 0); }
void mp_on_host_selected(uint32_t m, int32_t i) { hit(D_ON_HOST_SELECTED, m, i); }

/* ---- Lobby / Rennen ----------------------------------------------------- */
int32_t mp_get_player_count(uint32_t m)
{
    hit(D_GET_PLAYER_COUNT, m, 0);
    return DUMMY_PLAYER_COUNT;
}

int32_t mp_get_peer_lobby_state(uint32_t m, int32_t i)
{
    hit(D_GET_PEER_LOBBY_STATE, m, i);
    return DUMMY_PEER_STATE;
}

int32_t mp_has_connection_problem(uint32_t m)
{
    hit(D_HAS_CONNECTION_PROBLEM, m, 0);
    return 0;
}

int32_t mp_has_pending_join_request(uint32_t m)
{
    hit(D_HAS_PENDING_JOIN_REQUEST, m, 0);
    return 1;
}

int32_t mp_has_rematch_sync_failed(uint32_t m)
{
    hit(D_HAS_REMATCH_SYNC_FAILED, m, 0);
    return 0;
}

int32_t mp_is_peer_ready(uint32_t m, int32_t i)
{
    hit(D_IS_PEER_READY, m, i);
    return 1;
}

void mp_kick_peer(uint32_t m, int32_t i) { hit(D_KICK_PEER, m, i); }

void mp_send_race_loaded(uint32_t m)
{
    uint8_t sample[MP_SAMPLE_SIZE];
    int i;
    hit(D_SEND_RACE_LOADED, m, 0);
    for (i = 0; i < MP_SAMPLE_SIZE; i++)
        sample[i] = DUMMY_SAMPLE_BYTE(i);
    g_st.push_peer = m + DUMMY_PEER_OFFSET;
    g_st.push_calls++;
    if (g_plat)
        g_plat->push_sample(g_st.push_peer, sample);
}

void mp_send_race_abort(uint32_t m, int32_t reason) { hit(D_SEND_RACE_ABORT, m, reason); }

void mp_send_local_race_end(uint32_t m, int32_t position, int32_t from_leave)
{
    hit(D_SEND_LOCAL_RACE_END, m, position);
    g_st.last_position = position;
    g_st.last_from_leave = from_leave;
}

/* ---- Namen (der Kern liefert nur C-Strings) ------------------------------ */
static char g_namebuf[4][32];

const char *mp_host_name(uint32_t m, int32_t index)
{
    hit(D_HOST_NAME, m, index);
    g_st.name_calls++;
    snprintf(g_namebuf[0], sizeof g_namebuf[0], "HOST-%d", (int)index);
    return g_namebuf[0];
}

const char *mp_requesting_peer_name(uint32_t m)
{
    hit(D_REQUESTING_PEER_NAME, m, 0);
    g_st.name_calls++;
    snprintf(g_namebuf[1], sizeof g_namebuf[1], "JOINER");
    return g_namebuf[1];
}

const char *mp_lobby_peer_name(uint32_t m, int32_t i)
{
    hit(D_LOBBY_PEER_NAME, m, i);
    g_st.name_calls++;
    if (i == 3)
        return NULL; /* leerer Slot -> Nullzeiger in die sret-Zelle */
    snprintf(g_namebuf[2], sizeof g_namebuf[2], "LOBBY-%d", (int)i);
    return g_namebuf[2];
}

const char *mp_hud_peer_name(uint32_t m, int32_t i)
{
    hit(D_HUD_PEER_NAME, m, i);
    g_st.name_calls++;
    snprintf(g_namebuf[3], sizeof g_namebuf[3], "HUD-%d", (int)i);
    return g_namebuf[3];
}

/* ---- MPM_Clear51c: ein Stub, kein bl-Hook -------------------------------- */
void mp_clear_51c(uint32_t m) { hit(D_CLEAR_51C, m, 0); }

/* ---- Hooks auf echte Spielfunktionen ------------------------------------ */
void mp_screen2_enter(uint32_t m, uint32_t flow)
{
    g_st.hooks[H_SCREEN2_ENTER]++;
    g_st.last_mpm = m;
    g_st.last_flow = flow;
    /* wie glue.c: den Host/Join-Selektor per Gastaufruf bauen */
    if (g_plat) {
        g_st.call_guest_calls++;
        g_st.call_guest_ret = g_plat->call_guest(MP_ADDR_SELECTOR_INIT,
                                                 flow + 0x680, 0xaa, 0x78,
                                                 MP_TEXT_HOST_JOIN);
        g_st.call_guest_calls++;
        g_plat->call_guest(MP_ADDR_SELECTOR_ADD, flow + 0x680,
                           MP_TEXT_HOST_GAME, 0, 0);
    }
}

void mp_on_state4to5(uint32_t m, int32_t slot, int32_t score, int32_t a, int32_t b)
{
    g_st.hooks[H_STATE4TO5]++;
    g_st.last_mpm = m;
    g_st.last_slot = slot;
    g_st.last_score = score;
    g_st.last_stat_a = a;
    g_st.last_stat_b = b;
}

void mp_on_countdown_gate(uint32_t m)
{
    g_st.hooks[H_COUNTDOWN_GATE]++;
    g_st.last_mpm = m;
}
