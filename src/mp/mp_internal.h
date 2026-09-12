// Shared internals of the LAN multiplayer implementation.
//
//   nfsmp.c  transport: sockets, discovery, snapshot exchange, clock sync
//   glue.c   game side: MPM/GamePeer fields, the stubs, session logic
#ifndef MP_INTERNAL_H
#define MP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nfsmp.h"

// ---------------------------------------------------------------------------
// Guest memory access

extern const struct mp_platform *mp_plat;

static inline void *mp_ptr(uint32_t a) { return mp_plat->guest_ptr(a); }

static inline uint32_t mp_rd32(uint32_t a) {
    uint32_t v;
    memcpy(&v, mp_ptr(a), 4);
    return v;
}
static inline void mp_wr32(uint32_t a, uint32_t v) { memcpy(mp_ptr(a), &v, 4); }
static inline uint8_t mp_rd8(uint32_t a) { return *(const uint8_t *)mp_ptr(a); }
static inline void mp_wr8(uint32_t a, uint8_t v) { *(uint8_t *)mp_ptr(a) = v; }

void mp_log(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Session model
//
// The host owns slot 0, clients get 1..3 in join order. Slot numbers mean the
// same player on every device, so peer[i] can be mirrored verbatim.

enum mp_role { MP_ROLE_IDLE = 0, MP_ROLE_HOST, MP_ROLE_CLIENT };

// What every device knows about one player. The host owns this table and
// mirrors it to the clients; a client only ever writes its own slot locally
// and lets the host's copy win.
struct mp_slot {
    uint8_t used;
    uint8_t ready;      // pressed Ready in the lobby (screen 13)
    uint8_t loaded;     // race scene loaded -> peer+0x62
    uint8_t at_gate;    // sits in race state 6 waiting for the countdown
    uint8_t finished;   // reported its race result
    uint8_t car_valid;
    char name[MP_NAME_MAX];
    int32_t car[MP_CAR_WORDS];  // peer+0x04 .. +0x34
    int32_t res_pos;            // position the peer reported for itself
    int32_t res_score;          // peer+0x54
    int32_t res_a;              // peer+0x58
    int32_t res_b;              // peer+0x5c
    int32_t final_pos;          // final standing, assigned by the host (-1 = open)
};

// Everything the host mirrors to the clients. Serialised as one snapshot with
// a revision counter; losing a datagram is harmless because the next snapshot
// carries the complete state again.
struct mp_shared {
    uint32_t rev;
    uint32_t sid;            // session id, picked by the host
    uint32_t race_seq;       // bumped for every race start
    uint32_t abort_seq;      // bumped for every race abort
    int32_t abort_reason;    // 0..4, see GetErrorTextId / Set53cOnce
    int64_t t0;             // start time on the synchronised clock, 0 = none
    int32_t r0;             // race clock value that belongs to t0
    int32_t set500[3];      // tier, track, event type
    uint8_t set500_valid;
    uint8_t start_allowed;  // -> MPM+0x514
    uint8_t results_ready;  // final_pos is filled in for everyone
    uint8_t kick_mask;      // slots the host has thrown out
    struct mp_slot slot[MP_MAX_PEERS];
};

// What a client mirrors to the host.
struct mp_mine {
    uint32_t rev;
    uint8_t ready, loaded, at_gate, finished, car_valid, leaving;
    uint8_t abort_req;     // bumped for every race abort the client triggers
    int32_t abort_reason;
    char name[MP_NAME_MAX];
    int32_t car[MP_CAR_WORDS];
    int32_t res_pos, res_score, res_a, res_b;
};

// A host as seen by a browsing client.
struct mp_found_host {
    uint8_t used;
    uint32_t addr;      // network byte order IPv4
    uint16_t port;      // session port, host byte order
    uint32_t sid;
    char name[MP_NAME_MAX];
    char label[MP_NAME_MAX + 8];
    uint8_t players;
    int64_t last_seen;
    int32_t set500[3];
};

struct mp_state {
    bool inited;
    enum mp_role role;
    int32_t my_slot;
    uint32_t mpm;

    char my_name[MP_NAME_MAX];

    // Transport
    int disc_sock;      // bound to MP_DISCOVERY_PORT, answers probes
    int probe_sock;     // ephemeral, sends probes and receives the answers
    int sess_sock;      // session traffic
    uint16_t sess_port;

    // Browsing
    bool browsing;
    int64_t next_probe;
    struct mp_found_host hosts[8];
    int host_count;               // stable, only grows while browsing
    int32_t selected_host;

    // Joining
    bool connecting;
    int32_t connect_index;
    int64_t connect_deadline;
    int64_t next_join_send;
    uint32_t host_addr;
    uint16_t host_port;

    // Host side: one pending join request at a time
    bool join_pending;
    bool join_shown;   // the game has actually put the popup on screen
    uint32_t join_addr;
    uint16_t join_port;
    char join_name[MP_NAME_MAX];
    int64_t join_deadline;

    // Session
    struct mp_shared shared;
    struct mp_mine mine;
    uint32_t applied_rev;       // client: last shared rev applied to the MPM
    uint32_t applied_race_seq;  // race the local device has prepared for
    uint32_t applied_abort_seq;
    int64_t applied_t0;         // start time currently written to the MPM
    int32_t applied_r0;
    uint8_t calibrated;         // the race clock offset has been measured once

    // Per remote peer bookkeeping (host: every client, client: the host)
    int64_t last_rx[MP_MAX_PEERS];
    uint32_t peer_addr[MP_MAX_PEERS];
    uint16_t peer_port[MP_MAX_PEERS];
    uint32_t peer_ack[MP_MAX_PEERS];   // shared rev the peer confirmed
    int64_t next_send[MP_MAX_PEERS];
    uint8_t peer_abort_req[MP_MAX_PEERS];

    // A kicked peer keeps its address for a while: the slot is freed at once,
    // so without this the repeat of MSG_KICK would have nowhere to go and the
    // victim would see "the host has left" instead of "you have been kicked".
    struct {
        uint32_t addr;
        uint16_t port;
        int64_t until;
        int64_t next;
    } kicked[MP_MAX_PEERS];
    uint32_t mine_ack;                 // host confirmed this mine.rev
    int64_t next_mine_send;

    // Samples
    int32_t last_sample_t[MP_MAX_PEERS];
    int32_t local_sample_t;
    int32_t local_sample_stamp;
    uint16_t sample_seq;

    // Clock synchronisation (client only)
    int64_t clock_offset;
    int64_t next_ping;
    int64_t ping_sent_mono;
    int64_t best_rtt;
    int64_t best_offset;
    int sync_samples;
    bool clock_valid;

    // Game-side bookkeeping
    int32_t game_state;      // our idea of MPM+0x08
    int32_t game_error;      // pending MPM+0x524
    bool connection_problem;
    bool rematch_sync_failed;
    int64_t gate_since;      // when the local device reached the countdown gate
    int64_t race_started;
    int64_t first_finish;    // host: when the first player reported a result
    int64_t drift_guard;     // earliest time a self-resync may happen again
    int32_t last_set500[3];
    int32_t last_car[MP_CAR_WORDS];
    uint8_t last_car_valid;
    bool in_race;
    bool want_lobby_reset;
    char name_buf[MP_MAX_PEERS][MP_NAME_MAX];
};

extern struct mp_state mp;

// ---------------------------------------------------------------------------
// nfsmp.c (transport)

bool mp_net_open(void);
void mp_net_close(void);
void mp_net_poll(void);            // receive everything pending
void mp_net_tick(void);            // probes, retransmits, pings, timeouts
void mp_net_clear_hosts(void);
void mp_net_start_browsing(void);
void mp_net_stop_browsing(void);
void mp_net_start_host(void);
void mp_net_join(int32_t index);
void mp_net_cancel_join(void);
void mp_net_leave(void);
void mp_net_accept_join(bool accept);
void mp_net_kick(int32_t slot);
void mp_net_send_sample(const void *sample, int32_t race_time);
void mp_net_bump_shared(void);     // host: state changed, resend to everyone
void mp_net_bump_mine(void);       // client: own state changed
int64_t mp_now(void);              // synchronised wall clock in ms

// ---------------------------------------------------------------------------
// glue.c (game side)

void mp_glue_reset_session(void);
void mp_glue_drop_peer(int32_t slot, int32_t error_code);
void mp_glue_on_joined(int32_t slot);
void mp_glue_apply(void);          // write the session state into the MPM
void mp_glue_poll_local(void);     // read local changes out of the MPM
void mp_glue_push_sample(int32_t slot, const void *sample, int32_t t);
void mp_glue_set_error(int32_t code);

// MPM/GamePeer field helpers
#define MP_PEER(mpm, i) ((mpm) + 0x20u + 0x138u * (uint32_t)(i))

#endif
