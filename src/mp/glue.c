// Game side of the LAN multiplayer: the stub bodies the original build is
// missing, and the code that keeps the MultiplayerManager in sync with the
// session state the transport maintains.
//
// Field offsets come from scratchpad/wf1/mpm-fields.md; every field this file
// writes is one the shipped game only ever reads (the transport layer that
// used to write them was compiled out).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_internal.h"

// MultiplayerManager
#define M_CONNTYPE 0x004
#define M_STATE 0x008
#define M_ACTIVE 0x00c
#define M_LOCAL 0x010
#define M_WATCHED 0x014
#define M_ISHOST 0x018
#define M_SET500 0x500
#define M_SET500_OK 0x50c
#define M_FINALPOS 0x510
#define M_STARTOK 0x514
#define M_RACEANN 0x516
#define M_TOLOBBY 0x51c
#define M_ARMED 0x51d
#define M_MAXT 0x520
#define M_ERROR 0x524
#define M_ABORTED 0x528
#define M_CLOCKBASE 0x52c
#define M_T0_LO 0x530
#define M_T0_HI 0x534
#define M_TIMEOK 0x538
#define M_SYNCED 0x539
#define M_HOSTLIST 0x53a
#define M_ABORTWHY 0x53c

// GamePeer, relative to MP_PEER(mpm, i)
#define P_CAR 0x04
#define P_SCORE 0x54
#define P_RES_A 0x58
#define P_RES_B 0x5c
#define P_INFO_OK 0x60
#define P_LOADED 0x62
#define P_CONNECTED 0x65
#define P_HAS_SAMPLE 0x66
#define P_GRID 0x68
#define P_LAST_SAMPLE 0x8c

// How far ahead of "everybody is waiting at the gate" the race is scheduled.
#define START_LEAD_MS 700
// The race clock is (now - t0) + r0; let it start at zero.
#define START_CLOCK 0
// Self-correct the local race clock when it drifts further than this.
#define DRIFT_LIMIT 150
#define DRIFT_COOLDOWN 5000
// Give up on a peer that never reports its result.
#define RESULT_TIMEOUT 15000

static int used_count(void) {
    int n = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) n += mp.shared.slot[i].used ? 1 : 0;
    return n;
}

static struct mp_slot *my_slot(void) {
    return mp.my_slot >= 0 && mp.my_slot < MP_MAX_PEERS ? &mp.shared.slot[mp.my_slot] : NULL;
}

// A client edits its own slot locally so the UI reacts immediately, and
// mirrors the change to the host, which owns the authoritative copy.
static void mine_changed(void) {
    struct mp_slot *s = my_slot();
    if (!s) return;
    if (mp.role == MP_ROLE_CLIENT) {
        mp.mine.ready = s->ready;
        mp.mine.loaded = s->loaded;
        mp.mine.at_gate = s->at_gate;
        mp.mine.finished = s->finished;
        mp.mine.car_valid = s->car_valid;
        memcpy(mp.mine.car, s->car, sizeof mp.mine.car);
        mp.mine.res_pos = s->res_pos;
        mp.mine.res_score = s->res_score;
        mp.mine.res_a = s->res_a;
        mp.mine.res_b = s->res_b;
        snprintf(mp.mine.name, sizeof mp.mine.name, "%s", s->name);
        mp_net_bump_mine();
    } else {
        mp_net_bump_shared();
    }
}

static void set_state(uint32_t mpm, int32_t v) {
    if (!mpm) return;
    mp_wr32(mpm + M_STATE, (uint32_t)v);
    mp.game_state = v;
}

void mp_glue_set_error(int32_t code) {
    mp.game_error = code;
    if (mp.mpm && mp_rd32(mp.mpm + M_ERROR) == 0) {
        mp_wr32(mp.mpm + M_ERROR, (uint32_t)code);
        mp.game_error = 0;
    }
}

// --------------------------------------------------------------------------
// session lifecycle

void mp_glue_reset_session(void) {
    uint32_t mpm = mp.mpm;
    if (mpm) {
        mp_wr32(mpm + M_ACTIVE, 0);
        mp_wr8(mpm + M_STARTOK, 0);
        mp_wr8(mpm + M_RACEANN, 0);
        mp_wr8(mpm + M_ARMED, 0);
        mp_wr8(mpm + M_TIMEOK, 0);
        mp_wr8(mpm + M_HOSTLIST, 0);
        mp_wr8(mpm + M_SET500_OK, 0);
        for (int i = 0; i < MP_MAX_PEERS; i++) {
            uint32_t p = MP_PEER(mpm, i);
            mp_wr8(p + P_CONNECTED, 0);
            mp_wr8(p + P_LOADED, 0);
            mp_wr8(p + P_HAS_SAMPLE, 0);
        }
        if (mp.game_state != 0) set_state(mpm, 0);
    }
    mp.in_race = false;
    // A code that could not be delivered because MPM+0x524 was still occupied
    // must not surface in the next session.
    mp.game_error = 0;
    mp.applied_rev = 0;
    mp.applied_race_seq = 0;
    mp.applied_abort_seq = 0;
    mp.applied_t0 = 0;
    mp.applied_r0 = 0;
    mp.calibrated = 0;
    mp.gate_since = 0;
    mp.race_started = 0;
    mp.last_car_valid = 0;
    memset(mp.last_car, 0, sizeof mp.last_car);
    mp.last_set500[0] = mp.last_set500[1] = mp.last_set500[2] = -2;
}

// Forget everything that belonged to the previous race. The host also rewrites
// the shared table, which every client then mirrors; a client only resets its
// own slot and lets the host's copy win for the others.
static void clear_race_state(void) {
    for (int i = 0; i < MP_MAX_PEERS; i++) mp.last_sample_t[i] = INT32_MIN;
    mp.local_sample_t = INT32_MIN;
    mp.local_sample_stamp = INT32_MIN;
    mp.in_race = false;
    mp.gate_since = 0;
    mp.race_started = 0;
    mp.first_finish = 0;
    mp.drift_guard = 0;
    mp.applied_t0 = 0;
    mp.applied_r0 = 0;
    mp.calibrated = 0;
    // Neither Rematch_6to2 nor ResetPeers clears peer+0x62, so AllPeersFlag62
    // would be true immediately for the second race.
    if (mp.mpm)
        for (int i = 0; i < MP_MAX_PEERS; i++) mp_wr8(MP_PEER(mp.mpm, i) + P_LOADED, 0);

    struct mp_slot *me = my_slot();
    if (me) {
        me->loaded = me->at_gate = me->finished = 0;
        me->res_pos = -1;
        me->res_score = -1;
        me->res_a = me->res_b = 0;
    }
    if (mp.role == MP_ROLE_CLIENT) {
        mp.mine.loaded = mp.mine.at_gate = mp.mine.finished = 0;
        mp.mine.res_pos = mp.mine.res_score = -1;
        mp.mine.res_a = mp.mine.res_b = 0;
        mp_net_bump_mine();
        return;
    }
    if (mp.role != MP_ROLE_HOST) return;
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        struct mp_slot *s = &mp.shared.slot[i];
        s->loaded = s->at_gate = s->finished = 0;
        s->res_pos = -1;
        s->res_score = -1;
        s->res_a = s->res_b = 0;
        s->final_pos = -1;
    }
    mp.shared.t0 = 0;
    mp.shared.results_ready = 0;
    mp_net_bump_shared();
}

void mp_glue_on_joined(int32_t slot) {
    uint32_t mpm = mp.mpm;
    if (!mpm) return;
    mp_wr32(mpm + M_LOCAL, (uint32_t)slot);
    mp_wr32(mpm + M_WATCHED, 0);  // the host always owns slot 0
    mp_wr8(mpm + M_ISHOST, 0);
    mp_wr32(mpm + M_ACTIVE, 1);
    set_state(mpm, 2);
}

void mp_glue_drop_peer(int32_t slot, int32_t error_code) {
    if (slot < 0 || slot >= MP_MAX_PEERS) return;
    if (!mp.shared.slot[slot].used) return;
    mp_log("[mp] slot %d left", slot);
    memset(&mp.shared.slot[slot], 0, sizeof mp.shared.slot[slot]);
    mp.shared.slot[slot].final_pos = -1;
    mp.peer_addr[slot] = 0;
    mp.peer_port[slot] = 0;
    mp.peer_ack[slot] = 0;
    if (mp.mpm) {
        uint32_t p = MP_PEER(mp.mpm, slot);
        mp_wr8(p + P_CONNECTED, 0);
        mp_wr8(p + P_LOADED, 0);
    }
    if (error_code) mp_glue_set_error(error_code);
    else if (used_count() < 2 && mp.role == MP_ROLE_HOST)
        mp_glue_set_error(16);  // all other players have left
    if (mp.role == MP_ROLE_HOST) mp_net_bump_shared();
}

// --------------------------------------------------------------------------
// applying the session state to the guest object

static void apply_peer(uint32_t mpm, int i) {
    const struct mp_slot *s = &mp.shared.slot[i];
    uint32_t p = MP_PEER(mpm, i);
    mp_wr8(p + P_CONNECTED, s->used ? 1 : 0);
    if (!s->used) return;
    mp_wr32(p + P_GRID, (uint32_t)i);
    if (s->car_valid && i != mp.my_slot) {
        for (int k = 0; k < MP_CAR_WORDS; k++) mp_wr32(p + P_CAR + 4u * (uint32_t)k, (uint32_t)s->car[k]);
        mp_wr8(p + P_INFO_OK, 1);
    }
    mp_wr8(p + P_LOADED, s->loaded ? 1 : 0);
    if (s->finished) {
        mp_wr32(p + P_SCORE, (uint32_t)s->res_score);
        mp_wr32(p + P_RES_A, (uint32_t)s->res_a);
        mp_wr32(p + P_RES_B, (uint32_t)s->res_b);
    }
}

void mp_glue_apply(void) {
    uint32_t mpm = mp.mpm;
    if (!mpm || mp.role == MP_ROLE_IDLE) return;

    if (mp.game_error && mp_rd32(mpm + M_ERROR) == 0) {
        mp_wr32(mpm + M_ERROR, (uint32_t)mp.game_error);
        mp.game_error = 0;
    }

    for (int i = 0; i < MP_MAX_PEERS; i++) apply_peer(mpm, i);

    // The client takes the race settings from the host; the Flow uses them as
    // the success criterion of ConnectToHost, so they have to be in place
    // before we claim to be in a session.
    if (mp.role == MP_ROLE_CLIENT && mp.shared.set500_valid) {
        mp_wr32(mpm + M_SET500 + 0, (uint32_t)mp.shared.set500[0]);
        mp_wr32(mpm + M_SET500 + 4, (uint32_t)mp.shared.set500[1]);
        mp_wr32(mpm + M_SET500 + 8, (uint32_t)mp.shared.set500[2]);
        mp_wr8(mpm + M_SET500_OK, 1);
    }

    mp_wr8(mpm + M_STARTOK, mp.shared.start_allowed);

    // Race start and every later resync go through the same three fields: the
    // numbers are identical on all devices, because a client's s3eTimerGetUTC
    // is shifted onto the host's clock. Writing +0x538 makes the game adopt
    // them (and drop the sample histories) on its next frame.
    if (mp.shared.t0 && (mp.shared.t0 != mp.applied_t0 || mp.shared.r0 != mp.applied_r0)) {
        bool first = mp.applied_t0 != mp.shared.t0;
        mp_wr32(mpm + M_CLOCKBASE, (uint32_t)mp.shared.r0);
        mp_wr32(mpm + M_T0_LO, (uint32_t)(uint64_t)mp.shared.t0);
        mp_wr32(mpm + M_T0_HI, (uint32_t)((uint64_t)mp.shared.t0 >> 32));
        mp_wr8(mpm + M_ARMED, 1);
        mp_wr8(mpm + M_TIMEOK, 1);
        mp.applied_t0 = mp.shared.t0;
        mp.applied_r0 = mp.shared.r0;
        mp.applied_race_seq = mp.shared.race_seq;
        mp.race_started = mp.shared.t0;
        mp.in_race = true;
        if (first) {
            mp.calibrated = 0;
            mp.drift_guard = mp.shared.t0 + 10000;
            mp_log("[mp] race %u starts at t0=%lld", mp.shared.race_seq, (long long)mp.shared.t0);
        } else {
            mp_log("[mp] race clock base now %d", mp.shared.r0);
        }
    }

    // A remote abort is a pulse the game reads once through TestClear528.
    if (mp.shared.abort_seq != mp.applied_abort_seq) {
        mp.applied_abort_seq = mp.shared.abort_seq;
        if (mp_rd32(mpm + M_ABORTWHY) == 0) mp_wr32(mpm + M_ABORTWHY, (uint32_t)mp.shared.abort_reason);
        mp_wr8(mpm + M_ABORTED, 1);
    }

    if (mp.shared.results_ready) {
        const struct mp_slot *me = my_slot();
        if (me && me->final_pos >= 0) mp_wr32(mpm + M_FINALPOS, (uint32_t)me->final_pos);
        if (mp.game_state >= 2 && mp.game_state <= 5) set_state(mpm, 6);
    }
}

// --------------------------------------------------------------------------
// host decisions

static void host_update(void) {
    if (mp.role != MP_ROLE_HOST) return;
    struct mp_shared *sh = &mp.shared;
    int n = used_count();

    // Release the start as soon as everybody has pressed Ready. Entering any
    // lobby screen takes a player's ready flag away again, so this also drops
    // MPM+0x514 when somebody backs out of screen 13.
    {
        bool all_ready = n >= 2;
        for (int i = 0; i < MP_MAX_PEERS; i++)
            if (sh->slot[i].used && !sh->slot[i].ready) all_ready = false;
        uint8_t want = (uint8_t)(all_ready && sh->set500_valid);
        if (want != sh->start_allowed) {
            sh->start_allowed = want;
            mp_net_bump_shared();
        }
    }

    // Start: every device has loaded the scene and sits in race state 6.
    if (sh->start_allowed && !sh->t0) {
        bool all_gate = n >= 2;
        for (int i = 0; i < MP_MAX_PEERS; i++)
            if (sh->slot[i].used && !sh->slot[i].at_gate) all_gate = false;
        if (all_gate) {
            sh->race_seq++;
            sh->t0 = mp_now() + START_LEAD_MS;
            sh->r0 = START_CLOCK;
            mp_net_bump_shared();
        }
    }

    // Results: wait for everybody, then rank and hand each device its place.
    if (sh->t0 && !sh->results_ready) {
        bool all_done = true, any_done = false;
        for (int i = 0; i < MP_MAX_PEERS; i++)
            if (sh->slot[i].used) {
                if (sh->slot[i].finished) any_done = true;
                else all_done = false;
            }
        if (any_done && !mp.first_finish) mp.first_finish = mp_now();
        bool timed_out = mp.first_finish && mp_now() - mp.first_finish > RESULT_TIMEOUT;
        if (all_done || timed_out) {
            int order[MP_MAX_PEERS], m = 0;
            for (int i = 0; i < MP_MAX_PEERS; i++)
                if (sh->slot[i].used) order[m++] = i;
            // Position events report a finishing place, score events a score.
            bool by_pos = false;
            for (int i = 0; i < m; i++)
                if (sh->slot[order[i]].res_pos >= 0) by_pos = true;
            for (int a = 0; a < m; a++)
                for (int b = a + 1; b < m; b++) {
                    const struct mp_slot *x = &sh->slot[order[a]], *y = &sh->slot[order[b]];
                    bool swap;
                    if (by_pos) {
                        int px = x->res_pos < 0 ? 99 : x->res_pos, py = y->res_pos < 0 ? 99 : y->res_pos;
                        swap = py < px;
                    } else {
                        swap = y->res_score > x->res_score;
                    }
                    if (swap) {
                        int t = order[a];
                        order[a] = order[b];
                        order[b] = t;
                    }
                }
            for (int i = 0; i < m; i++) sh->slot[order[i]].final_pos = i;
            sh->results_ready = 1;
            mp_log("[mp] results ready (%s)", by_pos ? "by position" : "by score");
            mp_net_bump_shared();
        }
    }
}

// --------------------------------------------------------------------------
// reading local changes out of the guest object

static void poll_car_setup(uint32_t mpm) {
    struct mp_slot *me = my_slot();
    if (!me) return;
    uint32_t p = MP_PEER(mpm, mp.my_slot);
    if (!mp_rd8(p + P_INFO_OK)) return;
    int32_t car[MP_CAR_WORDS];
    for (int k = 0; k < MP_CAR_WORDS; k++) car[k] = (int32_t)mp_rd32(p + P_CAR + 4u * (uint32_t)k);
    if (mp.last_car_valid && memcmp(car, mp.last_car, sizeof car) == 0) return;
    memcpy(mp.last_car, car, sizeof car);
    mp.last_car_valid = 1;
    memcpy(me->car, car, sizeof car);
    me->car_valid = 1;
    mine_changed();
}

static void poll_set500(uint32_t mpm) {
    if (mp.role != MP_ROLE_HOST) return;
    if (!mp_rd8(mpm + M_SET500_OK)) return;
    int32_t v[3];
    for (int i = 0; i < 3; i++) v[i] = (int32_t)mp_rd32(mpm + M_SET500 + 4u * (uint32_t)i);
    if (memcmp(v, mp.last_set500, sizeof v) == 0) return;
    memcpy(mp.last_set500, v, sizeof v);
    memcpy(mp.shared.set500, v, sizeof v);
    mp.shared.set500_valid = 1;
    mp_net_bump_shared();
}

// PushLocalSample keeps the newest local sample in peer[local]+0x8c and the
// highest race time it ever saw in MPM+0x520, so we can spot a new sample
// without hooking anything.
static void poll_local_sample(uint32_t mpm) {
    if (mp.game_state < 4 || mp.game_state > 5) return;
    uint32_t p = MP_PEER(mpm, mp.my_slot) + P_LAST_SAMPLE;
    int32_t maxt = (int32_t)mp_rd32(mpm + M_MAXT);
    int32_t t = (int32_t)mp_rd32(p);
    // +0x520 is a maximum, so after the race clock is snapped backwards it
    // stops moving; the sample's own timestamp catches that case.
    if (maxt == mp.local_sample_t && t == mp.local_sample_stamp) return;
    mp.local_sample_t = maxt;
    mp.local_sample_stamp = t;
    uint8_t sample[MP_SAMPLE_SIZE];
    memcpy(sample, mp_ptr(p), MP_SAMPLE_SIZE);
    mp_net_send_sample(sample, t);

    if (!mp.in_race || !mp.applied_t0) return;
    int64_t want = mp_now() - mp.applied_t0 + mp.applied_r0;
    int64_t err = (int64_t)t - want;

    // The race clock does not start at t0: the game freezes it until t0, then
    // plays a local 3-2-1 animation and only starts counting at "GO". How long
    // that takes is not knowable in advance, so the host measures it once from
    // its own first sample and publishes the offset as the clock base. Every
    // device then snaps onto the same line.
    if (!mp.calibrated) {
        mp.calibrated = 1;
        if (mp.role == MP_ROLE_HOST && (err > 20 || err < -20)) {
            mp.shared.r0 += (int32_t)err;
            mp_log("[mp] measured race clock base %d", mp.shared.r0);
            mp_net_bump_shared();
        }
        return;
    }

    // Afterwards the game advances the clock with its own frame delta, so a
    // device that stutters falls behind the others. Snap it back when the
    // error grows past what a sample interval can hide.
    if ((err > DRIFT_LIMIT || err < -DRIFT_LIMIT) && mp_now() > mp.drift_guard) {
        mp.drift_guard = mp_now() + DRIFT_COOLDOWN;
        mp_wr8(mpm + M_TIMEOK, 1);
        mp_log("[mp] race clock off by %lld ms, resyncing", (long long)err);
    }
}

void mp_glue_poll_local(void) {
    uint32_t mpm = mp.mpm;
    if (!mpm) return;

    int32_t st = (int32_t)mp_rd32(mpm + M_STATE);
    if (st != mp.game_state) {
        int32_t was = mp.game_state;
        mp.game_state = st;
        if (st == 0 && mp.role != MP_ROLE_IDLE) {
            // MPM_Reset: the player left multiplayer. It clears +4 and +8 but
            // not +0x0c, so this is the only place we can notice.
            mp_log("[mp] session reset by the game");
            mp_net_leave();
            mp_glue_reset_session();
            return;
        }
        if (st == 2 && was == 6) {
            // Rematch_6to2: back in the lobby for another race.
            mp_log("[mp] rematch");
            clear_race_state();
            struct mp_slot *me = my_slot();
            if (me) me->ready = 0;
            mine_changed();
        }
    }

    // The host opens the session lazily: entering screen 5 for the first time
    // sets isHost and the race settings but calls no stub at all.
    if (mp.role == MP_ROLE_IDLE && mp_rd8(mpm + M_ISHOST) && mp_rd8(mpm + M_SET500_OK)) {
        mp_net_start_host();
        mp_wr32(mpm + M_LOCAL, 0);
        mp_wr32(mpm + M_WATCHED, 0);
        mp_wr32(mpm + M_ACTIVE, 1);
        set_state(mpm, 2);
        mp.last_set500[0] = -2;
    }

    if (mp.role == MP_ROLE_IDLE || mp.my_slot < 0) return;
    poll_set500(mpm);
    poll_car_setup(mpm);
    poll_local_sample(mpm);
}

// --------------------------------------------------------------------------

void mp_glue_push_sample(int32_t slot, const void *sample, int32_t t) {
    if (slot < 0 || slot >= MP_MAX_PEERS || slot == mp.my_slot) return;
    if (!mp.shared.slot[slot].used) return;
    if (t <= mp.last_sample_t[slot]) return;  // out of order or duplicate
    mp.last_sample_t[slot] = t;
    if (!mp.mpm) return;
    // Only feed the history while the race is actually consuming it; outside
    // that window nothing pops and the guest vector would grow forever.
    if (mp.game_state < 3 || mp.game_state > 5) return;
    uint32_t p = MP_PEER(mp.mpm, slot);
    mp_wr8(p + P_HAS_SAMPLE, 1);
    mp_plat->push_sample(p, sample);
}

// --------------------------------------------------------------------------
// lifecycle

void mp_init(const struct mp_platform *platform, const char *player_name) {
    memset(&mp, 0, sizeof mp);
    mp_plat = platform;
    mp.disc_sock = mp.probe_sock = mp.sess_sock = -1;
    mp.my_slot = -1;
    mp.selected_host = -1;
    mp.game_state = 0;
    mp.last_set500[0] = mp.last_set500[1] = mp.last_set500[2] = -2;
    mp.local_sample_t = INT32_MIN;
    for (int i = 0; i < MP_MAX_PEERS; i++) mp.last_sample_t[i] = INT32_MIN;
    snprintf(mp.my_name, sizeof mp.my_name, "%s", player_name && *player_name ? player_name : "Player");
    mp.inited = mp_net_open();
    if (mp.inited) mp_log("[mp] ready as \"%s\"", mp.my_name);
}

void mp_shutdown(void) {
    if (!mp.inited) return;
    mp_net_leave();
    mp_net_close();
    mp.inited = false;
}

void mp_pump(uint32_t mpm) {
    if (!mp.inited) return;
    mp.mpm = mpm;
    mp_net_poll();
    mp_glue_poll_local();
    host_update();
    mp_net_tick();
    mp_glue_apply();
}

// --------------------------------------------------------------------------
// the stubs

void mp_host_lobby_reenter(uint32_t mpm) {
    mp.mpm = mpm;
    // Back in the host lobby: take new connections again and stand down.
    struct mp_slot *me = my_slot();
    if (!me || !me->ready) return;
    me->ready = 0;
    mine_changed();
}

void mp_enter_lobby_state(uint32_t mpm) {
    mp.mpm = mpm;
    // Screens 5, 6, 10 and 11 all land here; in every one of them the local
    // player is back in a lobby and no longer ready. MPM+0x514 follows from
    // that on its own (the host derives it from everybody's ready flag), so
    // there is no need to clear it here -- doing so would strand the in-game
    // rematch, whose path 10/11 -> 13 calls no other stub.
    struct mp_slot *me = my_slot();
    if (!me || !me->ready) return;
    me->ready = 0;
    mine_changed();
}

void mp_host_close_lobby(uint32_t mpm) {
    mp.mpm = mpm;
    // No new players from here on; the host is on its way into the race. Turn
    // a request that is still open into a proper refusal rather than letting
    // the other side sit in its "Connecting to ..." popup until it times out.
    if (mp.join_pending) mp_net_accept_join(false);
}

void mp_host_request_start(uint32_t mpm) {
    mp.mpm = mpm;
    struct mp_slot *me = my_slot();
    if (me) me->ready = 1;
    if (mp.role == MP_ROLE_HOST) mp_net_bump_shared();
}

void mp_set_browsing(uint32_t mpm, uint32_t ui_list) {
    mp.mpm = mpm;
    if (ui_list) {
        mp_net_clear_hosts();
        mp.selected_host = -1;
        mp_wr8(mpm + M_HOSTLIST, 0);
        mp_net_start_browsing();
    } else {
        mp_net_stop_browsing();
    }
}

int32_t mp_get_host_count(uint32_t mpm) {
    mp.mpm = mpm;
    return mp.host_count;
}

const char *mp_host_name(uint32_t mpm, int32_t index) {
    mp.mpm = mpm;
    if (index < 0 || index >= mp.host_count || !mp.hosts[index].used) return NULL;
    return mp.hosts[index].label;
}

void mp_on_host_selected(uint32_t mpm, int32_t index) {
    mp.mpm = mpm;
    if (index < 0 || index >= mp.host_count || !mp.hosts[index].used) {
        mp_wr8(mpm + M_HOSTLIST, 0);
        return;
    }
    mp.selected_host = index;
    const struct mp_found_host *h = &mp.hosts[index];
    for (int i = 0; i < 3; i++) mp_wr32(mpm + M_SET500 + 4u * (uint32_t)i, (uint32_t)h->set500[i]);
    // +0x53a is the only thing that unlocks the Join button on screen 3.
    mp_wr8(mpm + M_HOSTLIST, 1);
}

void mp_connect_to_host(uint32_t mpm, int32_t index) {
    mp.mpm = mpm;
    mp_net_join(index);
}

const char *mp_requesting_peer_name(uint32_t mpm) {
    mp.mpm = mpm;
    return mp.join_pending ? mp.join_name : NULL;
}

int32_t mp_has_pending_join_request(uint32_t mpm) {
    mp.mpm = mpm;
    // A non-zero answer is what opens the popup, and only a keypress closes
    // it again -- so remember that one is on screen and owes an answer.
    if (mp.join_pending) mp.join_shown = true;
    return mp.join_pending ? 1 : 0;
}

int32_t mp_accept_peer(uint32_t mpm) {
    mp.mpm = mpm;
    mp_net_accept_join(true);
    return 0;
}

void mp_decline_peer(uint32_t mpm) {
    mp.mpm = mpm;
    mp_net_accept_join(false);
}

void mp_cancel_connect(uint32_t mpm) {
    mp.mpm = mpm;
    mp_net_cancel_join();
    mp_wr32(mpm + M_ACTIVE, 0);
    mp_wr8(mpm + M_SET500_OK, 0);
    if (mp.game_state != 0) set_state(mpm, 0);
}

void mp_resume_browsing(uint32_t mpm) {
    mp.mpm = mpm;
    mp_net_cancel_join();
    mp_wr32(mpm + M_ACTIVE, 0);
    mp_wr8(mpm + M_SET500_OK, 0);
    mp_wr8(mpm + M_HOSTLIST, mp.selected_host >= 0 ? 1 : 0);
    if (mp.game_state != 0) set_state(mpm, 0);
    mp_net_start_browsing();
}

void mp_host_start_countdown(uint32_t mpm) {
    mp.mpm = mpm;
    // Screen 14 only counts down locally; the real start time is agreed on
    // later, once every device sits at the countdown gate in the race scene.
}

void mp_begin_race_handshake(uint32_t mpm) {
    mp.mpm = mpm;
    clear_race_state();
    // Screen 15 leaves as soon as +0x516 is set and either +0x539 is set or
    // the start time has passed. The start time is not known yet, so use the
    // pulse; the real synchronisation happens in the race scene.
    mp_wr8(mpm + M_RACEANN, 1);
    mp_wr8(mpm + M_SYNCED, 1);
    mine_changed();
}

int32_t mp_get_player_count(uint32_t mpm) {
    mp.mpm = mpm;
    if (mp.role == MP_ROLE_IDLE) return 0;
    return used_count();
}

const char *mp_lobby_peer_name(uint32_t mpm, int32_t i) {
    mp.mpm = mpm;
    if (i < 0 || i >= MP_MAX_PEERS || !mp.shared.slot[i].used) return NULL;
    return mp.shared.slot[i].name;
}

const char *mp_hud_peer_name(uint32_t mpm, int32_t i) { return mp_lobby_peer_name(mpm, i); }

int32_t mp_get_peer_lobby_state(uint32_t mpm, int32_t i) {
    mp.mpm = mpm;
    (void)i;
    // The three icons behind 0 / 1 / anything else could not be identified;
    // keep the neutral one the original stub returned.
    return 3;
}

int32_t mp_is_peer_ready(uint32_t mpm, int32_t i) {
    mp.mpm = mpm;
    if (i < 0 || i >= MP_MAX_PEERS) return 0;
    return mp.shared.slot[i].used && mp.shared.slot[i].ready ? 1 : 0;
}

int32_t mp_has_connection_problem(uint32_t mpm) {
    mp.mpm = mpm;
    return mp.connection_problem ? 1 : 0;
}

int32_t mp_has_rematch_sync_failed(uint32_t mpm) {
    mp.mpm = mpm;
    return mp.rematch_sync_failed ? 1 : 0;
}

void mp_kick_peer(uint32_t mpm, int32_t i) {
    mp.mpm = mpm;
    mp_net_kick(i);
}

void mp_send_race_loaded(uint32_t mpm) {
    mp.mpm = mpm;
    struct mp_slot *me = my_slot();
    if (!me) return;
    me->loaded = 1;
    // AllPeersFlag62 checks every connected peer including the local one.
    if (mp.my_slot >= 0) mp_wr8(MP_PEER(mpm, mp.my_slot) + P_LOADED, 1);
    mine_changed();
}

void mp_send_local_race_end(uint32_t mpm, int32_t position, int32_t from_leave) {
    mp.mpm = mpm;
    struct mp_slot *me = my_slot();
    if (!me) return;
    if (!me->finished) {
        me->finished = 1;
        me->res_pos = from_leave ? -1 : position;  // giving up is not a place
        mine_changed();
    }
    if (mp.game_state == 4) set_state(mpm, 5);
}

void mp_on_state4to5(uint32_t mpm, int32_t slot, int32_t score, int32_t a, int32_t b) {
    mp.mpm = mpm;
    (void)slot;
    struct mp_slot *me = my_slot();
    if (!me || me->finished) return;
    me->finished = 1;
    me->res_pos = -1;  // a score event, ranked by points
    me->res_score = score;
    me->res_a = a;
    me->res_b = b;
    mine_changed();
}

void mp_send_race_abort(uint32_t mpm, int32_t reason) {
    mp.mpm = mpm;
    mp_log("[mp] race aborted, reason %d", reason);
    if (mp.role == MP_ROLE_HOST) {
        mp.shared.abort_reason = reason;
        mp.shared.abort_seq++;
        mp.applied_abort_seq = mp.shared.abort_seq;  // do not echo it back at us
        mp_net_bump_shared();
    } else if (mp.role == MP_ROLE_CLIENT) {
        mp.mine.abort_reason = reason;
        mp.mine.abort_req++;
        mp_net_bump_mine();
    }
    if (mp.game_state >= 2 && mp.game_state <= 5) set_state(mpm, 6);
}

void mp_on_countdown_gate(uint32_t mpm) {
    mp.mpm = mpm;
    struct mp_slot *me = my_slot();
    if (!me || me->at_gate) return;
    me->at_gate = 1;
    mp.gate_since = mp_now();
    mine_changed();
}

void mp_clear_51c(uint32_t mpm) {
    mp.mpm = mpm;
    mp_wr8(mpm + M_TOLOBBY, 0);  // what the original function did
    // The only signal that the local player pressed Ready; it fires on the
    // host and the client, in the menu and in the in-game lobby.
    struct mp_slot *me = my_slot();
    if (!me || me->ready) return;
    me->ready = 1;
    mine_changed();
}

void mp_screen2_enter(uint32_t mpm, uint32_t flow) {
    mp.mpm = mpm;
    // This replaces `MPM->error = 17`, the unconditional kill switch that ends
    // every multiplayer attempt in "Multiplayer unavailable".
    mp_wr32(mpm + M_ERROR, 0);

    // Always LAN: index 0 of the connection selector, which also gives us the
    // four-row host list and the kick column.
    mp_wr8(flow + 0x861, 0);
    mp_wr32(flow + 0x5b8, 0);
    mp_wr8(flow + 0x5bc, 0);  // hide the tier selector left over from screen 7
    mp_wr8(flow + 0x794, 0);

    // Screen 2 never built its Host/Join selector in this build, so "Join" was
    // unreachable. Build it the same way screen 7 builds its own selectors.
    uint32_t sel = flow + 0x680;
    mp_plat->call_guest(MP_ADDR_SELECTOR_INIT, sel, 0xaa, 0x78, MP_TEXT_HOST_JOIN);
    mp_plat->call_guest(MP_ADDR_SELECTOR_ADD, sel, MP_TEXT_HOST_GAME, 0, 0);
    mp_plat->call_guest(MP_ADDR_SELECTOR_ADD, sel, MP_TEXT_JOIN_GAME, 0, 0);
    mp_wr32(flow + 0x6a4, 0);
    mp_wr8(flow + 0x6a8, 1);

    // Screen entry 2: keep the default "Next -> screen 7 (host)" until the
    // player switches the selector.
    mp_wr32(flow + 0xc4, 7);
    mp_wr8(flow + 0x860, 1);
}
