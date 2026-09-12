// Transport for the LAN multiplayer: UDP sockets, host discovery, the
// snapshot exchange between host and clients, and the clock synchronisation.
//
// Two ports are used (both configurable with NFS_MP_PORT, which sets the
// first of the pair):
//   base+0  discovery, the host answers broadcast probes here
//   base+1  session traffic; the host binds it, clients use an ephemeral port
//
// Reliability is deliberately dumb: host and client each mirror their complete
// state to the other side as a numbered snapshot and repeat it until the peer
// acknowledges that revision. A lost datagram costs one retransmit interval
// and never leaves the two sides disagreeing, because the next snapshot
// carries everything again. Only samples are fire-and-forget.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mp_internal.h"

const struct mp_platform *mp_plat;
struct mp_state mp;

enum {
    MSG_DISCOVER = 1,
    MSG_ADVERT,
    MSG_JOIN_REQ,
    MSG_JOIN_ACCEPT,
    MSG_JOIN_REJECT,
    MSG_SHARED,
    MSG_MINE,
    MSG_ACK,
    MSG_SAMPLE,
    MSG_PING,
    MSG_PONG,
    MSG_LEAVE,
    MSG_KICK,
};

#define HDR_SIZE 16
#define MP_BUF 1024

#define PROBE_INTERVAL 600
#define RESEND_INTERVAL 150
#define KEEPALIVE_INTERVAL 400
#define PEER_WARN_MS 3000
#define PEER_DROP_MS 8000
// Long enough for a human host to notice the popup and decide.
#define JOIN_TIMEOUT 25000
#define ACCEPT_TIMEOUT 22000

static uint16_t g_port_base = MP_DISCOVERY_PORT;
static uint32_t g_probe_token;
static uint32_t g_static_host;
static uint16_t g_static_port;

// --------------------------------------------------------------------------
// little endian serialisation

struct wr {
    uint8_t *p;
    size_t n, cap;
};
struct rd {
    const uint8_t *p;
    size_t n, len;
    bool bad;
};

static void w8(struct wr *w, uint8_t v) {
    if (w->n < w->cap) w->p[w->n] = v;
    w->n++;
}
static void w16(struct wr *w, uint16_t v) {
    w8(w, (uint8_t)v);
    w8(w, (uint8_t)(v >> 8));
}
static void w32(struct wr *w, uint32_t v) {
    w16(w, (uint16_t)v);
    w16(w, (uint16_t)(v >> 16));
}
static void w64(struct wr *w, uint64_t v) {
    w32(w, (uint32_t)v);
    w32(w, (uint32_t)(v >> 32));
}
static void wbuf(struct wr *w, const void *s, size_t n) {
    for (size_t i = 0; i < n; i++) w8(w, ((const uint8_t *)s)[i]);
}

static uint8_t r8(struct rd *r) {
    if (r->n >= r->len) {
        r->bad = true;
        return 0;
    }
    return r->p[r->n++];
}
static uint16_t r16(struct rd *r) {
    uint16_t a = r8(r);
    return (uint16_t)(a | ((uint16_t)r8(r) << 8));
}
static uint32_t r32(struct rd *r) {
    uint32_t a = r16(r);
    return a | ((uint32_t)r16(r) << 16);
}
static uint64_t r64(struct rd *r) {
    uint64_t a = r32(r);
    return a | ((uint64_t)r32(r) << 32);
}
static void rbuf(struct rd *r, void *d, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)d)[i] = r8(r);
}

// --------------------------------------------------------------------------

// The platform log takes a plain string: format here, once, for everyone.
void mp_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    mp_plat->log("%s", buf);
}

int64_t mp_now(void) { return mp_plat->utc_ms() + mp.clock_offset; }

static int64_t mono(void) { return mp_plat->mono_ms(); }

// inet_ntoa on a punned pointer trips strict aliasing on the MADDE gcc 4.4.1
// that builds the MeeGo side, so go through a real struct every time.
static const char *ip_str(uint32_t addr) {
    struct in_addr ia;
    ia.s_addr = addr;
    return inet_ntoa(ia);
}

static int make_socket(uint16_t port, bool reuse) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    if (reuse) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    return s;
}

static uint16_t sock_port(int s) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(s, (struct sockaddr *)&a, &l) < 0) return 0;
    return ntohs(a.sin_port);
}

static void hdr(struct wr *w, uint8_t type, uint32_t arg) {
    w8(w, 'N');
    w8(w, 'F');
    w8(w, 'S');
    w8(w, 'S');
    w8(w, MP_PROTOCOL_VERSION);
    w8(w, type);
    w8(w, mp.my_slot < 0 ? 0xff : (uint8_t)mp.my_slot);
    w8(w, 0);
    w32(w, mp.shared.sid);
    w32(w, arg);
}

static void send_to(int sock, uint32_t addr, uint16_t port, const void *buf, size_t n) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = htons(port);
    if (sendto(sock, buf, n, 0, (struct sockaddr *)&a, sizeof a) < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
        errno != ENETUNREACH && errno != EPERM)
        mp_log("[mp] sendto %s:%u failed: %s", ip_str(addr), port, strerror(errno));
}

// --------------------------------------------------------------------------
// snapshot payloads

static void put_shared(struct wr *w) {
    const struct mp_shared *s = &mp.shared;
    w32(w, s->rev);
    w32(w, s->sid);
    w32(w, s->race_seq);
    w32(w, s->abort_seq);
    w32(w, (uint32_t)s->abort_reason);
    w64(w, (uint64_t)s->t0);
    w32(w, (uint32_t)s->r0);
    for (int i = 0; i < 3; i++) w32(w, (uint32_t)s->set500[i]);
    w8(w, s->set500_valid);
    w8(w, s->start_allowed);
    w8(w, s->results_ready);
    w8(w, s->kick_mask);
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        const struct mp_slot *p = &s->slot[i];
        w8(w, p->used);
        w8(w, p->ready);
        w8(w, p->loaded);
        w8(w, p->at_gate);
        w8(w, p->finished);
        w8(w, p->car_valid);
        wbuf(w, p->name, MP_NAME_MAX);
        for (int k = 0; k < MP_CAR_WORDS; k++) w32(w, (uint32_t)p->car[k]);
        w32(w, (uint32_t)p->res_pos);
        w32(w, (uint32_t)p->res_score);
        w32(w, (uint32_t)p->res_a);
        w32(w, (uint32_t)p->res_b);
        w32(w, (uint32_t)p->final_pos);
    }
}

static bool get_shared(struct rd *r, struct mp_shared *s) {
    s->rev = r32(r);
    s->sid = r32(r);
    s->race_seq = r32(r);
    s->abort_seq = r32(r);
    s->abort_reason = (int32_t)r32(r);
    s->t0 = (int64_t)r64(r);
    s->r0 = (int32_t)r32(r);
    for (int i = 0; i < 3; i++) s->set500[i] = (int32_t)r32(r);
    s->set500_valid = r8(r);
    s->start_allowed = r8(r);
    s->results_ready = r8(r);
    s->kick_mask = r8(r);
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        struct mp_slot *p = &s->slot[i];
        p->used = r8(r);
        p->ready = r8(r);
        p->loaded = r8(r);
        p->at_gate = r8(r);
        p->finished = r8(r);
        p->car_valid = r8(r);
        rbuf(r, p->name, MP_NAME_MAX);
        p->name[MP_NAME_MAX - 1] = 0;
        for (int k = 0; k < MP_CAR_WORDS; k++) p->car[k] = (int32_t)r32(r);
        p->res_pos = (int32_t)r32(r);
        p->res_score = (int32_t)r32(r);
        p->res_a = (int32_t)r32(r);
        p->res_b = (int32_t)r32(r);
        p->final_pos = (int32_t)r32(r);
    }
    return !r->bad;
}

static void put_mine(struct wr *w) {
    const struct mp_mine *m = &mp.mine;
    w32(w, m->rev);
    w8(w, m->ready);
    w8(w, m->loaded);
    w8(w, m->at_gate);
    w8(w, m->finished);
    w8(w, m->car_valid);
    w8(w, m->leaving);
    w8(w, m->abort_req);
    w8(w, 0);
    w32(w, (uint32_t)m->abort_reason);
    wbuf(w, m->name, MP_NAME_MAX);
    for (int k = 0; k < MP_CAR_WORDS; k++) w32(w, (uint32_t)m->car[k]);
    w32(w, (uint32_t)m->res_pos);
    w32(w, (uint32_t)m->res_score);
    w32(w, (uint32_t)m->res_a);
    w32(w, (uint32_t)m->res_b);
}

static bool get_mine(struct rd *r, struct mp_mine *m) {
    m->rev = r32(r);
    m->ready = r8(r);
    m->loaded = r8(r);
    m->at_gate = r8(r);
    m->finished = r8(r);
    m->car_valid = r8(r);
    m->leaving = r8(r);
    m->abort_req = r8(r);
    (void)r8(r);
    m->abort_reason = (int32_t)r32(r);
    rbuf(r, m->name, MP_NAME_MAX);
    m->name[MP_NAME_MAX - 1] = 0;
    for (int k = 0; k < MP_CAR_WORDS; k++) m->car[k] = (int32_t)r32(r);
    m->res_pos = (int32_t)r32(r);
    m->res_score = (int32_t)r32(r);
    m->res_a = (int32_t)r32(r);
    m->res_b = (int32_t)r32(r);
    return !r->bad;
}

// --------------------------------------------------------------------------
// sending

static void send_shared_to(int slot) {
    uint8_t buf[MP_BUF];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, MSG_SHARED, mp.shared.rev);
    put_shared(&w);
    if (w.n > sizeof buf) return;
    send_to(mp.sess_sock, mp.peer_addr[slot], mp.peer_port[slot], buf, w.n);
    mp.next_send[slot] = mono() + RESEND_INTERVAL;
}

static void send_mine(void) {
    uint8_t buf[MP_BUF];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, MSG_MINE, mp.mine.rev);
    put_mine(&w);
    send_to(mp.sess_sock, mp.host_addr, mp.host_port, buf, w.n);
    mp.next_mine_send = mono() + RESEND_INTERVAL;
}

static void send_ack(uint32_t addr, uint16_t port, uint32_t rev) {
    uint8_t buf[HDR_SIZE];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, MSG_ACK, rev);
    send_to(mp.sess_sock, addr, port, buf, w.n);
}

static void send_simple(uint32_t addr, uint16_t port, uint8_t type, uint32_t arg) {
    uint8_t buf[HDR_SIZE + 4];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, type, arg);
    send_to(mp.sess_sock, addr, port, buf, w.n);
}

// Same, but stamped with the peer's protocol version instead of ours, so that
// a peer which drops foreign versions outright still reads the answer.
static void send_versioned(uint32_t addr, uint16_t port, uint8_t type, uint32_t arg, uint8_t version) {
    uint8_t buf[HDR_SIZE + 4];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, type, arg);
    buf[4] = version;
    send_to(mp.sess_sock, addr, port, buf, w.n);
}

void mp_net_bump_shared(void) {
    if (mp.role != MP_ROLE_HOST) return;
    mp.shared.rev++;
    for (int i = 1; i < MP_MAX_PEERS; i++)
        if (mp.shared.slot[i].used) mp.next_send[i] = 0;
}

void mp_net_bump_mine(void) {
    if (mp.role != MP_ROLE_CLIENT) return;
    mp.mine.rev++;
    mp.next_mine_send = 0;
}

// --------------------------------------------------------------------------
// discovery

static void broadcast_probe(void) {
    uint8_t buf[HDR_SIZE + 4];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, MSG_DISCOVER, g_probe_token);
    w32(&w, g_probe_token);

    // A probe has to reach the *host's* discovery port. Normally that is the
    // default one; NFS_MP_PORT only moves our own sockets out of the way when
    // two instances share a machine, so try both.
    uint16_t ports[2] = {MP_DISCOVERY_PORT, g_port_base};
    int nports = g_port_base == MP_DISCOVERY_PORT ? 1 : 2;

    struct ifaddrs *ifa = NULL;
    getifaddrs(&ifa);
    for (int k = 0; k < nports; k++) {
        send_to(mp.probe_sock, htonl(INADDR_BROADCAST), ports[k], buf, w.n);
        for (struct ifaddrs *it = ifa; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            if (!(it->ifa_flags & IFF_BROADCAST) || !it->ifa_broadaddr) continue;
            uint32_t b = ((struct sockaddr_in *)it->ifa_broadaddr)->sin_addr.s_addr;
            if (b && b != htonl(INADDR_BROADCAST)) send_to(mp.probe_sock, b, ports[k], buf, w.n);
        }
    }
    if (ifa) freeifaddrs(ifa);
    // A host named explicitly answers on the port just below its session one.
    if (g_static_host) send_to(mp.probe_sock, g_static_host, (uint16_t)(g_static_port - 1), buf, w.n);
}

static int find_host(uint32_t addr, uint16_t port) {
    for (int i = 0; i < mp.host_count; i++)
        if (mp.hosts[i].used && mp.hosts[i].addr == addr && mp.hosts[i].port == port) return i;
    return -1;
}

static void note_host(uint32_t addr, uint16_t port, uint32_t sid, const char *name, uint8_t players,
                      const int32_t set500[3]) {
    int i = find_host(addr, port);
    // One host can answer from more than one address (loopback and the LAN
    // address of the same machine), so fall back to matching the session id.
    if (i < 0 && sid)
        for (int k = 0; k < mp.host_count; k++)
            if (mp.hosts[k].used && mp.hosts[k].sid == sid) {
                i = k;
                break;
            }
    if (i < 0) {
        // Indices must not move while the game is connecting to one of them.
        if (!mp.connecting)
            for (int k = 0; k < mp.host_count && i < 0; k++)
                if (!mp.hosts[k].used) i = k;
        if (i < 0) {
            if (mp.host_count >= (int)(sizeof mp.hosts / sizeof mp.hosts[0])) return;
            i = mp.host_count++;
        }
        memset(&mp.hosts[i], 0, sizeof mp.hosts[i]);
        mp_log("[mp] found host %s:%u \"%s\"", ip_str(addr), port, name);
    }
    struct mp_found_host *h = &mp.hosts[i];
    h->used = 1;
    h->addr = addr;
    h->port = port;
    h->sid = sid;
    h->players = players;
    h->last_seen = mono();
    char tmp[MP_NAME_MAX];
    snprintf(tmp, sizeof tmp, "%s", name);
    if (strncmp(h->name, tmp, sizeof h->name) != 0)
        mp_log("[mp] host %d is now \"%s\"", i, tmp);
    memcpy(h->name, tmp, sizeof h->name);
    snprintf(h->label, sizeof h->label, "%s (%u)", tmp, players);
    for (int k = 0; k < 3; k++) h->set500[k] = set500[k];
}

// Forget every host found so far and start over.
void mp_net_clear_hosts(void) {
    mp.host_count = 0;
    memset(mp.hosts, 0, sizeof mp.hosts);
}

void mp_net_start_browsing(void) {
    mp.browsing = true;
    mp.next_probe = 0;
}

void mp_net_stop_browsing(void) {
    // The list and its indices have to stay valid: the Flow keeps polling
    // GetHostCount() against the index it is connecting to.
    mp.browsing = false;
}

// --------------------------------------------------------------------------
// session setup

static void reset_transport_peers(void) {
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        mp.last_rx[i] = 0;
        mp.peer_addr[i] = 0;
        mp.peer_port[i] = 0;
        mp.peer_ack[i] = 0;
        mp.next_send[i] = 0;
        mp.last_sample_t[i] = INT32_MIN;
    }
    mp.mine_ack = 0;
    mp.next_mine_send = 0;
    mp.local_sample_t = INT32_MIN;
}

void mp_net_start_host(void) {
    memset(&mp.shared, 0, sizeof mp.shared);
    memset(mp.kicked, 0, sizeof mp.kicked);
    reset_transport_peers();
    mp.game_error = 0;
    mp.role = MP_ROLE_HOST;
    mp.my_slot = 0;
    mp.clock_offset = 0;
    mp.clock_valid = true;
    mp_plat->set_clock_offset(0);
    uint32_t sid = (uint32_t)(mp_plat->utc_ms() & 0xffffffff) ^ (uint32_t)(uintptr_t)&mp;
    mp.shared.sid = sid ? sid : 1;
    mp.shared.abort_reason = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) mp.shared.slot[i].final_pos = -1;
    struct mp_slot *me = &mp.shared.slot[0];
    me->used = 1;
    snprintf(me->name, sizeof me->name, "%s", mp.my_name);
    mp.join_pending = false;
    mp_log("[mp] hosting as \"%s\", session %08x, port %u", mp.my_name, mp.shared.sid, mp.sess_port);
    mp_net_bump_shared();
}

void mp_net_join(int32_t index) {
    if (index < 0 || index >= mp.host_count || !mp.hosts[index].used) {
        mp_glue_set_error(8);  // peer not found
        return;
    }
    reset_transport_peers();
    mp.role = MP_ROLE_CLIENT;
    mp.my_slot = -1;
    mp.connecting = true;
    mp.connect_index = index;
    mp.host_addr = mp.hosts[index].addr;
    mp.host_port = mp.hosts[index].port;
    mp.connect_deadline = mono() + JOIN_TIMEOUT;
    mp.next_join_send = 0;
    mp.game_error = 0;
    mp.clock_offset = 0;
    mp.clock_valid = false;
    mp.best_rtt = INT64_MAX;
    mp.sync_samples = 0;
    mp_plat->set_clock_offset(0);
    memset(&mp.shared, 0, sizeof mp.shared);
    memset(&mp.mine, 0, sizeof mp.mine);
    snprintf(mp.mine.name, sizeof mp.mine.name, "%s", mp.my_name);
    mp.mine.res_pos = mp.mine.res_score = -1;
    mp_log("[mp] joining %s:%u", ip_str(mp.host_addr), mp.host_port);
}

void mp_net_cancel_join(void) {
    // The accept may have arrived in the very pump in which the player hit
    // Cancel, in which case connecting is already false and the slot is
    // assigned. Leaving properly is the right answer either way -- otherwise
    // the client keeps sending keepalives and the host is stuck with a player
    // that never becomes ready.
    if (mp.role == MP_ROLE_CLIENT) mp_net_leave();
    mp.connecting = false;
}

void mp_net_leave(void) {
    if (mp.role == MP_ROLE_HOST) {
        for (int i = 1; i < MP_MAX_PEERS; i++)
            if (mp.shared.slot[i].used)
                for (int k = 0; k < 3; k++) send_simple(mp.peer_addr[i], mp.peer_port[i], MSG_LEAVE, 0);
    } else if (mp.role == MP_ROLE_CLIENT && mp.host_addr) {
        for (int k = 0; k < 3; k++) send_simple(mp.host_addr, mp.host_port, MSG_LEAVE, 0);
    }
    mp.role = MP_ROLE_IDLE;
    mp.my_slot = -1;
    mp.connecting = false;
    mp.join_pending = false;
    mp.clock_offset = 0;
    mp.clock_valid = false;
    mp_plat->set_clock_offset(0);
    memset(&mp.shared, 0, sizeof mp.shared);
    memset(&mp.mine, 0, sizeof mp.mine);
    reset_transport_peers();
    // Take the session flags out of the guest object as well, or the game
    // keeps believing it is in a multiplayer session (MPM_Reset does not
    // clear +0x0c, and nothing else ever clears peer+0x65).
    mp_glue_reset_session();
}

void mp_net_accept_join(bool accept) {
    if (!mp.join_pending) return;
    mp.join_pending = false;
    mp.join_shown = false;
    if (!accept) {
        send_simple(mp.join_addr, mp.join_port, MSG_JOIN_REJECT, 9);  // declined
        return;
    }
    int slot = -1;
    for (int i = 1; i < MP_MAX_PEERS; i++)
        if (!mp.shared.slot[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) {
        send_simple(mp.join_addr, mp.join_port, MSG_JOIN_REJECT, 8);
        return;
    }
    struct mp_slot *p = &mp.shared.slot[slot];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->final_pos = -1;
    p->res_pos = -1;
    p->res_score = -1;
    // The slot may have been vacated by a kick or a timeout; whoever takes it
    // over now must not inherit that verdict.
    mp.shared.kick_mask &= (uint8_t) ~(1u << slot);
    mp.peer_abort_req[slot] = 0;
    snprintf(p->name, sizeof p->name, "%s", mp.join_name);
    mp.peer_addr[slot] = mp.join_addr;
    mp.peer_port[slot] = mp.join_port;
    mp.peer_ack[slot] = 0;
    mp.last_rx[slot] = mono();
    mp.last_sample_t[slot] = INT32_MIN;
    send_simple(mp.join_addr, mp.join_port, MSG_JOIN_ACCEPT, (uint32_t)slot);
    mp_log("[mp] accepted \"%s\" as slot %d", p->name, slot);
    mp_net_bump_shared();
}

void mp_net_kick(int32_t slot) {
    if (mp.role != MP_ROLE_HOST || slot <= 0 || slot >= MP_MAX_PEERS) return;
    if (!mp.shared.slot[slot].used) return;
    // Remember where to repeat it: dropping the peer clears peer_addr/port.
    mp.kicked[slot].addr = mp.peer_addr[slot];
    mp.kicked[slot].port = mp.peer_port[slot];
    mp.kicked[slot].until = mono() + 6000;
    mp.kicked[slot].next = 0;
    mp.shared.kick_mask |= (uint8_t)(1u << slot);
    send_simple(mp.peer_addr[slot], mp.peer_port[slot], MSG_KICK, 0);
    mp_log("[mp] kicked slot %d", slot);
    mp_glue_drop_peer(slot, 0);
}

// --------------------------------------------------------------------------
// samples

void mp_net_send_sample(const void *sample, int32_t race_time) {
    if (mp.role == MP_ROLE_IDLE || mp.my_slot < 0) return;
    uint8_t buf[HDR_SIZE + 8 + MP_SAMPLE_WIRE];
    struct wr w = {buf, 0, sizeof buf};
    hdr(&w, MSG_SAMPLE, ++mp.sample_seq);
    w8(&w, (uint8_t)mp.my_slot);
    w8(&w, 0);
    w16(&w, 0);
    w32(&w, (uint32_t)race_time);
    wbuf(&w, sample, MP_SAMPLE_WIRE);
    if (mp.role == MP_ROLE_HOST) {
        for (int i = 1; i < MP_MAX_PEERS; i++)
            if (mp.shared.slot[i].used) send_to(mp.sess_sock, mp.peer_addr[i], mp.peer_port[i], buf, w.n);
    } else {
        send_to(mp.sess_sock, mp.host_addr, mp.host_port, buf, w.n);
    }
}

static void relay_sample(const uint8_t *buf, size_t n, int from_slot) {
    for (int i = 1; i < MP_MAX_PEERS; i++)
        if (i != from_slot && mp.shared.slot[i].used)
            send_to(mp.sess_sock, mp.peer_addr[i], mp.peer_port[i], buf, n);
}

// --------------------------------------------------------------------------
// receive

static void on_discovery(const uint8_t *buf, size_t len, uint32_t addr, uint16_t port) {
    struct rd r = {buf, HDR_SIZE, len, false};
    uint8_t type = buf[5];
    if (type != MSG_DISCOVER) return;
    if (mp.role != MP_ROLE_HOST) return;
    uint32_t token = r32(&r);
    if (r.bad) return;
    if (token == g_probe_token) return;  // our own probe looping back

    uint8_t players = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) players += mp.shared.slot[i].used ? 1 : 0;

    uint8_t out[HDR_SIZE + 64];
    struct wr w = {out, 0, sizeof out};
    hdr(&w, MSG_ADVERT, token);
    w32(&w, token);
    w16(&w, mp.sess_port);
    w8(&w, players);
    w8(&w, (uint8_t)(mp.shared.start_allowed || mp.in_race));
    wbuf(&w, mp.my_name, MP_NAME_MAX);
    for (int i = 0; i < 3; i++) w32(&w, (uint32_t)mp.shared.set500[i]);
    // Answer in the prober's version so a peer on a different protocol still
    // sees us in its list and gets a proper "needs update" when it joins.
    out[4] = buf[4];
    send_to(mp.disc_sock, addr, port, out, w.n);
}

static void on_advert(const uint8_t *buf, size_t len, uint32_t addr) {
    struct rd r = {buf, HDR_SIZE, len, false};
    uint32_t sid;
    memcpy(&sid, buf + 8, 4);
    uint32_t token = r32(&r);
    if (token != g_probe_token) return;
    uint16_t port = r16(&r);
    uint8_t players = r8(&r);
    (void)r8(&r);
    char name[MP_NAME_MAX];
    rbuf(&r, name, MP_NAME_MAX);
    name[MP_NAME_MAX - 1] = 0;
    int32_t set500[3];
    for (int i = 0; i < 3; i++) set500[i] = (int32_t)r32(&r);
    if (r.bad || !port) return;
    note_host(addr, port, sid, name, players, set500);
}

static int slot_of_sender(uint32_t addr, uint16_t port) {
    for (int i = 1; i < MP_MAX_PEERS; i++)
        if (mp.shared.slot[i].used && mp.peer_addr[i] == addr && mp.peer_port[i] == port) return i;
    return -1;
}

static void on_session(const uint8_t *buf, size_t len, uint32_t addr, uint16_t port) {
    uint8_t type = buf[5];
    uint8_t sender = buf[6];
    uint32_t sid, arg;
    memcpy(&sid, buf + 8, 4);
    memcpy(&arg, buf + 12, 4);
    struct rd r = {buf, HDR_SIZE, len, false};

    if (mp.role == MP_ROLE_HOST) {
        switch (type) {
        case MSG_JOIN_REQ: {
            uint8_t ver = r8(&r);
            char name[MP_NAME_MAX];
            rbuf(&r, name, MP_NAME_MAX);
            name[MP_NAME_MAX - 1] = 0;
            if (r.bad) return;
            if (ver != MP_PROTOCOL_VERSION) {
                // Echo the sender's version, or their drain() drops our answer
                // and they never learn why the join failed.
                send_versioned(addr, port, MSG_JOIN_REJECT, ver < MP_PROTOCOL_VERSION ? 11 : 10, ver);
                return;
            }
            int known = slot_of_sender(addr, port);
            if (known >= 0) {
                // Our accept was sent once and can be lost, so answer every
                // repeat. This branch also catches a client that gave up and
                // pressed Join again from the same port: clear what it might
                // otherwise inherit from the previous attempt.
                struct mp_slot *p = &mp.shared.slot[known];
                if (p->ready || p->loaded || p->at_gate || p->finished) {
                    p->ready = p->loaded = p->at_gate = p->finished = 0;
                    mp_net_bump_shared();
                }
                mp.last_rx[known] = mono();
                send_simple(addr, port, MSG_JOIN_ACCEPT, (uint32_t)known);
                return;
            }
            if (mp.join_pending && (mp.join_addr != addr || mp.join_port != port)) {
                // Another decision is already on screen. Staying quiet turns
                // the client's own retransmit into the queue: it keeps its
                // "Connecting to ..." popup and becomes the next request as
                // soon as the host is done with this one.
                return;
            }
            int free_slots = 0;
            for (int i = 1; i < MP_MAX_PEERS; i++) free_slots += mp.shared.slot[i].used ? 0 : 1;
            if (!free_slots) {
                send_simple(addr, port, MSG_JOIN_REJECT, 8);
                return;
            }
            if (mp.in_race || mp.shared.start_allowed) {
                send_simple(addr, port, MSG_JOIN_REJECT, 8);
                return;
            }
            if (!mp.join_pending) {
                mp.join_pending = true;
                mp.join_addr = addr;
                mp.join_port = port;
                snprintf(mp.join_name, sizeof mp.join_name, "%s", name);
                mp.join_deadline = mono() + ACCEPT_TIMEOUT;
                mp_log("[mp] join request from \"%s\"", name);
            }
            return;
        }
        case MSG_MINE: {
            int slot = slot_of_sender(addr, port);
            if (slot < 0 || sid != mp.shared.sid) return;
            struct mp_mine m;
            memset(&m, 0, sizeof m);
            if (!get_mine(&r, &m)) return;
            mp.last_rx[slot] = mono();
            send_ack(addr, port, m.rev);
            struct mp_slot *p = &mp.shared.slot[slot];
            bool changed = false;
            if (m.leaving) {
                mp_glue_drop_peer(slot, 0);
                return;
            }
#define UPD(field, src)                  \
    if ((int32_t)p->field != (int32_t)(src)) { \
        p->field = (src);                \
        changed = true;                  \
    }
            UPD(ready, m.ready)
            UPD(loaded, m.loaded)
            UPD(at_gate, m.at_gate)
            UPD(finished, m.finished)
            UPD(car_valid, m.car_valid)
            UPD(res_pos, m.res_pos)
            UPD(res_score, m.res_score)
            UPD(res_a, m.res_a)
            UPD(res_b, m.res_b)
#undef UPD
            if (memcmp(p->car, m.car, sizeof p->car) != 0) {
                memcpy(p->car, m.car, sizeof p->car);
                changed = true;
            }
            if (m.name[0] && strncmp(p->name, m.name, MP_NAME_MAX) != 0) {
                snprintf(p->name, sizeof p->name, "%s", m.name);
                changed = true;
            }
            // A client that aborted the race tells everyone through us.
            if (m.abort_req != mp.peer_abort_req[slot]) {
                mp.peer_abort_req[slot] = m.abort_req;
                mp.shared.abort_reason = m.abort_reason;
                mp.shared.abort_seq++;
                changed = true;
            }
            if (changed) mp_net_bump_shared();
            return;
        }
        case MSG_ACK: {
            int slot = slot_of_sender(addr, port);
            if (slot < 0) return;
            mp.last_rx[slot] = mono();
            if (arg > mp.peer_ack[slot]) mp.peer_ack[slot] = arg;
            return;
        }
        case MSG_PING: {
            // Only peers that actually hold a slot may keep themselves alive;
            // a kicked client must run into its timeout rather than be fed.
            if (slot_of_sender(addr, port) < 0) return;
            int64_t t = (int64_t)r64(&r);
            if (r.bad) return;
            uint8_t out[HDR_SIZE + 16];
            struct wr w = {out, 0, sizeof out};
            hdr(&w, MSG_PONG, 0);
            w64(&w, (uint64_t)t);
            w64(&w, (uint64_t)mp_now());
            send_to(mp.sess_sock, addr, port, out, w.n);
            int slot = slot_of_sender(addr, port);
            if (slot >= 0) mp.last_rx[slot] = mono();
            return;
        }
        case MSG_SAMPLE: {
            int slot = slot_of_sender(addr, port);
            if (slot < 0) return;
            mp.last_rx[slot] = mono();
            uint8_t from = r8(&r);
            (void)r8(&r);
            (void)r16(&r);
            int32_t t = (int32_t)r32(&r);
            if (r.bad || from != slot) return;
            uint8_t sample[MP_SAMPLE_SIZE];
            memset(sample, 0, sizeof sample);
            rbuf(&r, sample, MP_SAMPLE_WIRE);
            if (r.bad) return;
            relay_sample(buf, len, slot);
            mp_glue_push_sample(slot, sample, t);
            return;
        }
        case MSG_LEAVE: {
            int slot = slot_of_sender(addr, port);
            if (slot >= 0) {
                mp_glue_drop_peer(slot, 0);
            } else if (mp.join_pending && mp.join_addr == addr && mp.join_port == port) {
                mp.join_pending = false;
                if (mp.join_shown) mp_glue_set_error(12);  // peer cancelled
                mp.join_shown = false;
            }
            return;
        }
        default:
            return;
        }
    }

    if (mp.role != MP_ROLE_CLIENT) return;
    if (addr != mp.host_addr || port != mp.host_port) return;

    switch (type) {
    case MSG_JOIN_ACCEPT:
        if (mp.my_slot >= 0) return;
        if (arg >= MP_MAX_PEERS) return;
        mp.connecting = false;
        mp.my_slot = (int32_t)arg;
        mp.shared.sid = sid;
        mp.shared.slot[mp.my_slot].used = 1;
        snprintf(mp.shared.slot[mp.my_slot].name, MP_NAME_MAX, "%s", mp.my_name);
        mp.last_rx[0] = mono();
        mp.next_ping = 0;
        mp_log("[mp] joined as slot %d", mp.my_slot);
        mp_glue_on_joined(mp.my_slot);
        mp_net_bump_mine();
        return;
    case MSG_JOIN_REJECT:
        mp_log("[mp] join rejected (%u)", arg);
        mp.connecting = false;
        mp.role = MP_ROLE_IDLE;
        mp.my_slot = -1;
        mp_glue_set_error(arg ? (int32_t)arg : 2);
        return;
    case MSG_SHARED: {
        struct mp_shared s;
        memset(&s, 0, sizeof s);
        if (!get_shared(&r, &s)) return;
        mp.last_rx[0] = mono();
        // Do not confirm anything before we are actually in the session: an
        // acknowledgement is also the host's proof that we are alive, and a
        // slot we never took has to run into its timeout. A duplicate of a
        // revision we already hold must still be acknowledged, otherwise the
        // host repeats the full snapshot forever.
        if (mp.my_slot < 0) return;
        send_ack(addr, port, s.rev);
        if (s.rev <= mp.shared.rev && mp.shared.rev != 0) return;
        mp.shared = s;
        // Our own flags travel to the host, not back: keeping the local copy
        // stops the UI from flickering while a change is in flight.
        struct mp_slot *me = &mp.shared.slot[mp.my_slot];
        if (me->used) {
            me->ready = mp.mine.ready;
            me->loaded = mp.mine.loaded;
            me->at_gate = mp.mine.at_gate;
            me->finished = mp.mine.finished;
        }
        if (mp.shared.kick_mask & (1u << mp.my_slot)) {
            mp_glue_set_error(13);  // you have been kicked
            mp_net_leave();
            return;
        }
        if (!mp.shared.slot[mp.my_slot].used) {
            mp_glue_set_error(1);  // connection to peer was lost
            mp_net_leave();
            return;
        }
        return;
    }
    case MSG_ACK:
        mp.last_rx[0] = mono();
        if (arg > mp.mine_ack) mp.mine_ack = arg;
        return;
    case MSG_PONG: {
        int64_t sent = (int64_t)r64(&r);
        int64_t host_time = (int64_t)r64(&r);
        if (r.bad) return;
        mp.last_rx[0] = mono();
        int64_t rtt = mono() - sent;
        if (rtt < 0 || rtt > 2000) return;
        int64_t offset = host_time + rtt / 2 - mp_plat->utc_ms();
        if (rtt < mp.best_rtt || mp.sync_samples == 0) {
            mp.best_rtt = rtt;
            mp.best_offset = offset;
        }
        mp.sync_samples++;
        if (mp.sync_samples >= 3 && mp.best_offset != mp.clock_offset) {
            int64_t delta = mp.best_offset - mp.clock_offset;
            if (!mp.clock_valid || delta > 8 || delta < -8) {
                mp.clock_offset = mp.best_offset;
                mp_plat->set_clock_offset(mp.clock_offset);
                mp_log("[mp] clock offset %lld ms (rtt %lld)", (long long)mp.clock_offset, (long long)mp.best_rtt);
            }
            mp.clock_valid = true;
        }
        return;
    }
    case MSG_SAMPLE: {
        uint8_t from = r8(&r);
        (void)r8(&r);
        (void)r16(&r);
        int32_t t = (int32_t)r32(&r);
        if (r.bad || from >= MP_MAX_PEERS || from == mp.my_slot) return;
        uint8_t sample[MP_SAMPLE_SIZE];
        memset(sample, 0, sizeof sample);
        rbuf(&r, sample, MP_SAMPLE_WIRE);
        if (r.bad) return;
        mp.last_rx[0] = mono();
        mp_glue_push_sample(from, sample, t);
        return;
    }
    case MSG_KICK:
        mp_glue_set_error(13);
        mp_net_leave();
        return;
    case MSG_LEAVE:
        mp_glue_set_error(14);  // the host has left
        mp_net_leave();
        return;
    default:
        (void)sender;
        return;
    }
}

static void drain(int sock, bool discovery) {
    if (sock < 0) return;
    for (;;) {
        uint8_t buf[MP_BUF];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        if (n < HDR_SIZE) continue;
        if (buf[0] != 'N' || buf[1] != 'F' || buf[2] != 'S' || buf[3] != 'S') continue;
        if (buf[4] != MP_PROTOCOL_VERSION) {
            // A peer running a different protocol version has to be able to
            // reach us far enough to be told so: let the handshake messages
            // through and drop everything that would touch session state.
            switch (buf[5]) {
            case MSG_DISCOVER:
            case MSG_ADVERT:
            case MSG_JOIN_REQ:
            case MSG_JOIN_REJECT:
                break;
            default:
                continue;
            }
        }
        uint32_t addr = from.sin_addr.s_addr;
        uint16_t port = ntohs(from.sin_port);
        if (discovery) {
            if (buf[5] == MSG_DISCOVER) on_discovery(buf, (size_t)n, addr, port);
            else if (buf[5] == MSG_ADVERT) on_advert(buf, (size_t)n, addr);
        } else {
            on_session(buf, (size_t)n, addr, port);
        }
    }
}

void mp_net_poll(void) {
    drain(mp.disc_sock, true);
    drain(mp.probe_sock, true);
    drain(mp.sess_sock, false);
}

// --------------------------------------------------------------------------

void mp_net_tick(void) {
    int64_t now = mono();

    if (mp.browsing && now >= mp.next_probe) {
        broadcast_probe();
        mp.next_probe = now + PROBE_INTERVAL;
    }

    if (mp.connecting) {
        if (now >= mp.next_join_send) {
            uint8_t buf[HDR_SIZE + 4 + MP_NAME_MAX];
            struct wr w = {buf, 0, sizeof buf};
            hdr(&w, MSG_JOIN_REQ, 0);
            w8(&w, MP_PROTOCOL_VERSION);
            wbuf(&w, mp.my_name, MP_NAME_MAX);
            send_to(mp.sess_sock, mp.host_addr, mp.host_port, buf, w.n);
            mp.next_join_send = now + 300;
        }
        if (now >= mp.connect_deadline) {
            mp_log("[mp] join timed out");
            mp.connecting = false;
            mp.role = MP_ROLE_IDLE;
            mp.my_slot = -1;
            mp_glue_set_error(2);
        }
    }

    if (mp.role == MP_ROLE_HOST) {
        if (mp.join_pending && now >= mp.join_deadline) {
            send_simple(mp.join_addr, mp.join_port, MSG_JOIN_REJECT, 9);
            mp.join_pending = false;
            // The popup is only ever closed by a keypress, so tell the host
            // that the request is gone (error 12, "Peer cancelled connection").
            if (mp.join_shown) mp_glue_set_error(12);
            mp.join_shown = false;
        }
        // Repeat a kick for a few seconds; the slot is already free.
        for (int i = 1; i < MP_MAX_PEERS; i++) {
            if (!mp.kicked[i].until) continue;
            if (now >= mp.kicked[i].until) {
                mp.kicked[i].until = 0;
                continue;
            }
            if (now >= mp.kicked[i].next) {
                send_simple(mp.kicked[i].addr, mp.kicked[i].port, MSG_KICK, 0);
                mp.kicked[i].next = now + 400;
            }
        }
        for (int i = 1; i < MP_MAX_PEERS; i++) {
            if (!mp.shared.slot[i].used) continue;
            if (mp.peer_ack[i] != mp.shared.rev) {
                if (now >= mp.next_send[i]) send_shared_to(i);
            } else if (now >= mp.next_send[i] + KEEPALIVE_INTERVAL) {
                send_shared_to(i);
            }
            int64_t quiet = now - mp.last_rx[i];
            if (quiet > PEER_DROP_MS) {
                mp_log("[mp] slot %d timed out", i);
                mp_glue_drop_peer(i, 0);
            }
        }
    } else if (mp.role == MP_ROLE_CLIENT && mp.my_slot >= 0) {
        if (mp.mine_ack != mp.mine.rev) {
            if (now >= mp.next_mine_send) send_mine();
        } else if (now >= mp.next_mine_send + KEEPALIVE_INTERVAL) {
            send_mine();
        }
        if (now >= mp.next_ping) {
            uint8_t buf[HDR_SIZE + 8];
            struct wr w = {buf, 0, sizeof buf};
            hdr(&w, MSG_PING, 0);
            w64(&w, (uint64_t)now);
            send_to(mp.sess_sock, mp.host_addr, mp.host_port, buf, w.n);
            mp.next_ping = now + (mp.sync_samples < 8 ? 250 : 2000);
        }
        if (now - mp.last_rx[0] > PEER_DROP_MS) {
            mp_log("[mp] lost the host");
            mp_glue_set_error(14);
            mp_net_leave();
        }
    }

    // A connection problem is anything that keeps a connected peer quiet for
    // longer than PEER_WARN_MS; the game locks the lobby and aborts a running
    // race while this is true.
    bool problem = false;
    if (mp.role == MP_ROLE_HOST) {
        for (int i = 1; i < MP_MAX_PEERS; i++)
            if (mp.shared.slot[i].used && now - mp.last_rx[i] > PEER_WARN_MS) problem = true;
    } else if (mp.role == MP_ROLE_CLIENT && mp.my_slot >= 0) {
        problem = now - mp.last_rx[0] > PEER_WARN_MS;
    }
    mp.connection_problem = problem;

    // Forget hosts that stopped answering, but never move the remaining
    // entries: their indices are what the Flow is holding on to.
    for (int i = 0; i < mp.host_count; i++)
        if (mp.hosts[i].used && mp.browsing && now - mp.hosts[i].last_seen > 4000) mp.hosts[i].used = 0;
}

// --------------------------------------------------------------------------

bool mp_net_open(void) {
    const char *p = getenv("NFS_MP_PORT");
    if (p) {
        int v = atoi(p);
        if (v > 1024 && v < 65000) g_port_base = (uint16_t)v;
    }
    const char *h = getenv("NFS_MP_HOST");
    if (h) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", h);
        char *colon = strchr(tmp, ':');
        uint16_t hp = (uint16_t)(g_port_base + 1);
        if (colon) {
            *colon = 0;
            hp = (uint16_t)atoi(colon + 1);
        }
        struct in_addr ia;
        if (inet_aton(tmp, &ia)) {
            g_static_host = ia.s_addr;
            g_static_port = hp;
        }
    }

    // Discovery has to be shareable so every instance on a machine sees the
    // broadcasts. Session traffic is pure unicast and must NOT be shared, or
    // two instances silently steal each other's datagrams -- the host carries
    // its actual port in the advert, so falling back to an ephemeral one costs
    // nothing.
    mp.disc_sock = make_socket(g_port_base, true);
    mp.probe_sock = make_socket(0, false);
    mp.sess_sock = make_socket((uint16_t)(g_port_base + 1), false);
    if (mp.sess_sock < 0) mp.sess_sock = make_socket(0, false);
    if (mp.disc_sock < 0 || mp.probe_sock < 0 || mp.sess_sock < 0) {
        mp_log("[mp] cannot open sockets: %s", strerror(errno));
        mp_net_close();
        return false;
    }
    mp.sess_port = sock_port(mp.sess_sock);
    g_probe_token = (uint32_t)(mp_plat->utc_ms() * 2654435761u) ^ (uint32_t)mp.sess_port ^ (uint32_t)getpid();
    if (!g_probe_token) g_probe_token = 1;
    mp_log("[mp] discovery on %u, session on %u", g_port_base, mp.sess_port);
    mp_net_clear_hosts();
    return true;
}

void mp_net_close(void) {
    if (mp.disc_sock >= 0) close(mp.disc_sock);
    if (mp.probe_sock >= 0) close(mp.probe_sock);
    if (mp.sess_sock >= 0) close(mp.sess_sock);
    mp.disc_sock = mp.probe_sock = mp.sess_sock = -1;
}
