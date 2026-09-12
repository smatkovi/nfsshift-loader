/*
 * meego/test/core_dummy.h — Beobachtungsstruktur des Kern-Dummys.
 *
 * NUR FUER DEN QEMU-TEST.  Der Dummy ersetzt src/mp/{nfsmp,glue}.c, solange
 * der echte Kern noch nicht steht, und protokolliert, was preload.c ihm
 * uebergibt.  fakeloader holt sich mpdummy_get() per dlsym(RTLD_DEFAULT,...),
 * weil der Dummy in der vorgeladenen libnfsmp.so steckt.
 */
#ifndef CORE_DUMMY_H
#define CORE_DUMMY_H

#include <stdint.h>
#include "nfsmp.h"

/* Reihenfolge = Deklarationsreihenfolge der mp_*-Funktionen in nfsmp.h */
enum {
    D_HOST_LOBBY_REENTER,
    D_ENTER_LOBBY_STATE,
    D_HOST_CLOSE_LOBBY,
    D_HOST_REQUEST_START,
    D_SET_BROWSING,
    D_GET_HOST_COUNT,
    D_CONNECT_TO_HOST,
    D_ACCEPT_PEER,
    D_DECLINE_PEER,
    D_CANCEL_CONNECT,
    D_RESUME_BROWSING,
    D_HOST_START_COUNTDOWN,
    D_BEGIN_RACE_HANDSHAKE,
    D_ON_HOST_SELECTED,

    D_GET_PLAYER_COUNT,
    D_GET_PEER_LOBBY_STATE,
    D_HAS_CONNECTION_PROBLEM,
    D_HAS_PENDING_JOIN_REQUEST,
    D_HAS_REMATCH_SYNC_FAILED,
    D_IS_PEER_READY,
    D_KICK_PEER,
    D_SEND_RACE_LOADED,
    D_SEND_RACE_ABORT,

    D_SEND_LOCAL_RACE_END,

    D_HOST_NAME,
    D_REQUESTING_PEER_NAME,
    D_LOBBY_PEER_NAME,
    D_HUD_PEER_NAME,

    D_CLEAR_51C, /* 0x4a0a8528 ist ein Stub, kein bl-Hook */
    D_COUNT
};

/* bl-Hooks */
enum { H_SCREEN2_ENTER, H_STATE4TO5, H_COUNTDOWN_GATE, H_COUNT };

/* Rueckgabewerte, auf die der Test prueft */
#define DUMMY_HOST_COUNT   2
#define DUMMY_PLAYER_COUNT 3
#define DUMMY_PEER_STATE   5
#define DUMMY_CLOCK_OFFSET 12345
#define DUMMY_PEER_OFFSET  0x20 /* Peer 0 = MPM+0x20 */

/* Sample-Muster fuer den PushHistory-Test */
#define DUMMY_SAMPLE_BYTE(i) ((uint8_t)((i) * 7u + 3u))

struct mpdummy_state {
    uint32_t calls[D_COUNT];
    uint32_t hooks[H_COUNT];
    uint32_t total;
    int32_t last_id;
    uint32_t last_mpm; /* Gastadresse, wie preload.c sie uebergeben hat */
    int32_t last_i;
    uint32_t last_ptr;       /* SetBrowsing: UIList*                    */
    uint32_t last_flow;      /* mp_screen2_enter: Flow*                 */
    int32_t last_position;   /* mp_send_local_race_end                  */
    int32_t last_from_leave; /* dito                                    */
    int32_t last_slot, last_score, last_stat_a, last_stat_b; /* State4to5 */
    uint32_t init_calls;
    uint32_t pump_calls;
    uint32_t pump_mpm;
    uint32_t name_calls;
    uint32_t push_calls;
    uint32_t push_peer;
    uint32_t call_guest_calls;
    uint32_t call_guest_ret;
    int64_t utc_ms;
    int64_t mono_ms;
    char last_name[32];
};

#ifdef __cplusplus
extern "C" {
#endif
struct mpdummy_state *mpdummy_get(void);
#ifdef __cplusplus
}
#endif

#endif
