/*
 * meego/preload.c — LD_PRELOAD-Patcher fuer den originalen N9-Loader
 *                   (/opt/usr/bin/ea-mobile-nfsshift, Harmattan 1.2).
 *
 * Diese Datei ist die MeeGo-Plattformschicht des LAN-Multiplayers.  Der
 * plattformneutrale Kern (src/mp/{nfsmp,glue,patches}.c) wird hier nur
 * angebunden; alles, was Gastspeicher, Gastcode oder Loader-Interna beruehrt,
 * steht in dieser Datei.
 *
 *   1. mprotect() interposieren.  Der Loader ruft genau einmal
 *      mprotect(codebase, 0x1c6000, PROT_READ|PROT_EXEC) — nach Relokation
 *      und Trampolinbau, vor dem Entrypoint (meego-runtime-patch.md §1.5,
 *      §4.1).  Wir machen daraus RWX und patchen sofort.
 *   2. Image-Struct ueber das feste Loader-Global *(void**)0x0007c328 finden
 *      und plausibilisieren (Magic "XE3U", Link-Base, split, Trampolinformat).
 *   3. Wort-Patches (mp_code_patches), Stub-Umleitungen (mp_stubs) und
 *      bl-Hooks (mp_hooks) anwenden.  Kurze Stellen bekommen "b"/"bl" auf ein
 *      8-Byte-Veneer im freien RWX-Schwanz hinter dem Thunk-Pool, Stubs mit
 *      >= 8 Byte Platz inline "ldr pc,[pc,#-4]".
 *   4. Import-Trampoline von s3eDeviceYield (Export 336, Frame-Pump) und
 *      s3eTimerGetUTC (Export 490, Uhrenversatz) umbiegen.
 *   5. struct mp_platform fuellen: Adressumrechnung, Uhren, PushHistory.
 *      midp::String-Objekte fuer die vier sret-Stubs baut diese Datei selbst
 *      aus den C-Strings des Kerns.
 *
 * ABI-Hinweis (meego-runtime-patch.md §3.4): der .s3e-Spielcode benutzt die
 * softfp-Konvention, diese .so wird hardfp gebaut.  Fuer reine Integer-/
 * Zeigersignaturen — und das sind ALLE hier beruehrten Ein- und Ausgaenge —
 * sind beide Konventionen bitgleich (Argumente in r0..r3 und auf dem Stack,
 * Rueckgabe in r0/r1).  Relevant wuerde der Unterschied erst, wenn eine vom
 * Spiel gerufene Funktion float/double entgegennaehme oder zurueckgaebe; dann
 * braucht sie __attribute__((pcs("aapcs"))), siehe §6.3.  Stellen, an denen
 * das eintreten koennte, sind unten mit SOFTFP markiert.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "nfsmp.h"

/* --------------------------------------------------------------------------
 * Feste Adressen im nicht-PIE-Loader (ELF-vaddr == Laufzeitadresse)
 * meego-runtime-patch.md §1.2 / §1.4
 */
#define LDR_IMAGE_GLOBAL 0x0007c328u /* void*  DAT_0007c328 = Image-Struct   */
#define LDR_NEXPORTS     0x0006be38u /* int    DAT_0006be38 = 511            */

/* Offsets im 0x100 Byte grossen Image-Struct (FUN_0000ef20) */
#define IMG_MAGIC   0x88 /* 0x55334558 "XE3U"                      */
#define IMG_VERSION 0x8c
#define IMG_IMGSZ   0xa0
#define IMG_MEMSZ   0xa4 /* 0x2000c4                               */
#define IMG_ENTRY   0xb0
#define IMG_BASE    0xbc /* 0x4a000000                             */
#define IMG_SPLIT   0xcc /* 0x1c1078                               */
#define IMG_CODE    0xd8 /* Codebereich (posix_memalign, RWX->RX)  */
#define IMG_TOTAL   0xdc /* Groesse der Code-Allokation            */
#define IMG_DATA    0xe0 /* Datenbereich (plain malloc, RW)        */
#define IMG_TRAMP   0xf0 /* = CODE + SPLIT                         */
#define IMG_TRAMPSZ 0xf4 /* 8 / 0x10 / 0x14; auf dem N9 0x10       */

#define IMG_MAGIC_VALUE 0x55334558u

/* Reserve des Thunk-Pools hinter der Trampolintabelle (FUN_0000e430) */
#define THUNK_POOL_SIZE 0x2000u

/* Export-Indizes der beiden gehookten Importe */
#define EXP_S3E_DEVICE_YIELD  336
#define EXP_S3E_TIMER_GET_UTC 490

/* ARM-Opcodes */
#define OP_LDR_PC_PC_M4 0xe51ff004u /* ldr pc,[pc,#-4]  */
#define OP_B_BASE       0xea000000u /* b  <imm24>       */
#define OP_BL_BASE      0xeb000000u /* bl <imm24>       */

/*
 * Ruecksprungziele der beiden Aufrufstellen von MPM_SendLocalRaceEnd
 * (0x4a0a7f10).  Der Kern will statt des LR ein Flag; unterscheidbar sind die
 * Stellen nur hier (verify-stubs-lobby-race.md §2.9):
 *   0x4a124f14 bl ...  -> Ziellinie      (position = car+0x398)
 *   0x4a11b5e0 bl ...  -> Rennen verlassen (position = 0)
 */
#define LR_RACE_END_FINISH 0x4a124f18u
#define LR_RACE_END_LEAVE  0x4a11b5e4u

/* Aufrufstellen der bl-Hooks, als Rueckfallzuordnung falls mp_hook.name
 * anders heisst als erwartet (verify-stubs-session.md §1.1, race-sync.md §405,
 * mpm-fields.md §147). */
#define HOOK_ADDR_SCREEN2   0x4a08dba4u /* str r1,[r0,#0x524] (Kill-Switch)   */
#define HOOK_ADDR_STATE4TO5 0x4a11fab8u /* bl MPM_State4to5                   */
#define HOOK_ADDR_COUNTDOWN 0x4a12c80cu /* bl MPM_UpdateCountdown             */

/* --------------------------------------------------------------------------
 * Zustand
 */
static unsigned char *g_code;   /* Host-Adresse von Gast 0x4a000000        */
static unsigned char *g_data;   /* Host-Adresse von Gast 0x4a000000+split  */
static uint32_t g_split;        /* 0x1c1078                                */
static uint32_t g_datasz;       /* memsz - split                           */
static unsigned char *g_tramp;  /* Trampolintabelle                        */
static uint32_t g_trampsz;      /* Eintragsgroesse                         */
static uint32_t g_nexports;     /* 511                                     */
static unsigned char *g_pad;    /* naechster freier Veneer-Platz           */
static unsigned char *g_padend; /* Ende des freien RWX-Schwanzes           */
static unsigned char *g_padstart;

static int g_patched; /* do_patch() gelaufen                            */
static int g_inited;  /* mp_init() gelaufen                             */
static int g_quiet;
static volatile int64_t g_clock_offset; /* ms, aus set_clock_offset       */

/* Originalziele der umgebogenen Import-Trampoline */
typedef void (*yield_fn)(uint32_t ms);
typedef int64_t (*utc_fn)(void);
static yield_fn g_real_yield;
static utc_fn g_real_utc;

/* --------------------------------------------------------------------------
 * Logging
 */
static void mp_vlog(const char *fmt, va_list ap)
{
    char buf[512];
    size_t n;
    if (g_quiet)
        return;
    n = (size_t)vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    while (n > 0 && buf[n - 1] == '\n')
        buf[--n] = 0;
    fprintf(stderr, "[nfsmp] %s\n", buf);
}

static void mp_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mp_vlog(fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------------------------------
 * Kleine Helfer
 */
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t get32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void clear_cache(void *a, void *b)
{
#if defined(__GNUC__) && defined(__arm__)
    __builtin___clear_cache((char *)a, (char *)b);
#else
    (void)a;
    (void)b;
#endif
}

/* Ist [addr, addr+len) abgebildet?  msync() liefert ENOMEM fuer Luecken und
 * ist damit die billigste Pruefung, bevor wir ein festes Loader-Global
 * dereferenzieren (die .so koennte versehentlich in einen fremden Prozess
 * vorgeladen werden). */
static int addr_mapped(uint32_t addr, size_t len)
{
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xfff;
    size_t span = ((addr + len) - page + 0xfff) & ~(size_t)0xfff;
    if (msync((void *)page, span, MS_ASYNC) == 0)
        return 1;
    return errno != ENOMEM;
}

/* --------------------------------------------------------------------------
 * Adressumrechnung Gast <-> Host
 *
 * Der .s3e wird an eine Laufzeitbasis geladen, deshalb ist die Abbildung aus
 * DESIGN_LAN.md §1 noetig:
 *     host = (off < split) ? code + off : data + off - split
 * Adressen ausserhalb des Images (MultiplayerManager, GamePeer, midp::String
 * — alles Objekte aus dem Spiel-Heap) werden 1:1 durchgereicht.  Damit ein
 * Heap-Objekt niemals faelschlich als Image-Adresse gelesen wird, reserviert
 * der Konstruktor das Gastfenster [0x4a000000, +0x202000) mit PROT_NONE;
 * danach kann dort kein malloc/mmap mehr landen.
 */
static void *g2h(uint32_t a)
{
    uint32_t off;
    if (!g_code)
        return (void *)(uintptr_t)a;
    off = a - MP_GUEST_BASE;
    if (off < g_split)
        return g_code + off;
    if (off < g_split + g_datasz)
        return g_data + (off - g_split);
    return (void *)(uintptr_t)a;
}

static uint32_t h2g(const void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    uint32_t off;
    if (!g_code)
        return a;
    off = a - (uint32_t)(uintptr_t)g_code;
    if (off < g_split)
        return MP_GUEST_BASE + off;
    off = a - (uint32_t)(uintptr_t)g_data;
    if (off < g_datasz)
        return MP_GUEST_BASE + g_split + off;
    return a;
}

/* Der MultiplayerManager-Singleton.  MPM_GetInstance (0x4a0a7c2c) ist der
 * einzige Zugriff auf die Globale 0x4a1c2050 (mpm-fields.md §1); wir lesen sie
 * direkt, statt Gastcode zu rufen.
 * ACHTUNG (mpm-fields.md §46): 0x4a107aa4 loescht den Singleton, ohne die
 * Globale zu nullen — danach zeigt sie auf freigegebenen Speicher. */
static uint32_t current_mpm(void)
{
    uint32_t host;
    if (!g_code)
        return 0;
    host = get32(g2h(MP_ADDR_MPM_GLOBAL));
    return host ? h2g((const void *)(uintptr_t)host) : 0u;
}

/* --------------------------------------------------------------------------
 * struct mp_platform
 */
static void *plat_guest_ptr(uint32_t addr) { return g2h(addr); }

/* Rohe Uhr — exakt die Quelle, die auch das Spiel ueber s3eTimerGetUTC sieht,
 * ohne unseren Versatz. */
static int64_t plat_utc_ms(void)
{
    if (g_real_utc)
        return g_real_utc();
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
}

static int64_t plat_mono_ms(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
}

static void plat_set_clock_offset(int64_t offset_ms)
{
    g_clock_offset = offset_ms; /* absolut, nicht kumulativ */
    mp_logf("Uhrenversatz: %+lld ms", (long long)offset_ms);
}

/*
 * Gastaufrufe.  ARM/Thumb: 0x4a16e8e4, 0x4a0dbc30, 0x4a0cf99c und 0x4a0a8610
 * sind ARM-Einsprungpunkte (Bit 0 = 0, stubs-lobby-race.md §2.1).  Der Aufruf
 * ueber einen C-Funktionszeiger wird zu blx uebersetzt und wertet Bit 0
 * ohnehin korrekt aus — ein gesetztes Bit 0 wuerde also nach Thumb wechseln
 * statt abzustuerzen; wir maskieren es deshalb nicht weg.
 * SOFTFP: alle vier Signaturen sind rein Zeiger/Integer -> identisch.
 */
typedef void *(*guest_new_fn)(uint32_t size);
typedef void *(*guest_strctor_fn)(void *self, const char *text);
typedef void (*guest_addref_fn)(void *self);

/* GamePeer_PushHistory(peer, Sample byval): r0 = peer, r1..r3 = Samplebytes
 * 0..11, Bytes 12..0xa3 ab sp (mpm-fields.md §11.2).  Genau das erzeugt AAPCS
 * fuer eine Struktur der Ausrichtung 4, wenn r0..r3 schon belegt sind: der
 * komplette Rest liegt lueckenlos ab sp+0. */
struct mp_sample_tail {
    uint8_t b[MP_SAMPLE_SIZE - 12];
};
typedef void (*guest_push_fn)(void *peer, uint32_t w0, uint32_t w1, uint32_t w2,
                              struct mp_sample_tail tail);

static void plat_push_sample(uint32_t peer_addr, const void *sample)
{
    guest_push_fn push = (guest_push_fn)(uintptr_t)g2h(MP_ADDR_PUSH_HISTORY);
    struct mp_sample_tail tail;
    uint32_t w[3];
    if (!g_code || !peer_addr)
        return;
    memcpy(w, sample, sizeof w);
    memcpy(tail.b, (const uint8_t *)sample + 12, sizeof tail.b);
    push(g2h(peer_addr), w[0], w[1], w[2], tail);
}

/* Beliebige Gastfunktion mit bis zu vier Integer-/Zeigerargumenten rufen.
 * Jedes Argument laeuft durch g2h(): Werte im Gastfenster
 * [0x4a000000, +memsz) werden zu Hostzeigern, alles andere bleibt unveraendert
 * — kleine Konstanten und Text-IDs also auch.  (Grenzfall: eine reine
 * Ganzzahl, die zufaellig im Gastfenster laege, wuerde umgerechnet; bei den
 * Selektor-Aufrufen aus glue.c kommt das nicht vor.)
 * SOFTFP: rein Integer/Zeiger, also konventionsunabhaengig. */
typedef uint32_t (*guest4_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

static uint32_t plat_call_guest(uint32_t fn, uint32_t a0, uint32_t a1,
                                uint32_t a2, uint32_t a3)
{
    guest4_fn f;
    if (!g_code || !fn)
        return 0;
    f = (guest4_fn)(uintptr_t)g2h(fn);
    return f((uint32_t)(uintptr_t)g2h(a0), (uint32_t)(uintptr_t)g2h(a1),
             (uint32_t)(uintptr_t)g2h(a2), (uint32_t)(uintptr_t)g2h(a3));
}

static void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mp_vlog(fmt, ap);
    va_end(ap);
}

static const struct mp_platform g_platform = {
    plat_guest_ptr,        plat_utc_ms,      plat_mono_ms,
    plat_set_clock_offset, plat_push_sample, plat_call_guest,
    plat_log,
};

/* --------------------------------------------------------------------------
 * midp::String fuer die vier sret-Stubs
 *
 * Der Kern liefert nur C-Strings; das Gastobjekt baut diese Schicht.  Ein
 * frisch konstruiertes midp::String hat Refcount 0 (verify-stubs-lobby-race.md
 * §1.2).  Wir halten pro Text genau eine Dauerreferenz (0->1) und geben pro
 * Getter-Aufruf eine weitere aus (1->2); der Aufrufer macht genau ein release
 * (2->1) und loescht das Objekt damit nicht.
 */
#define STR_CACHE_MAX 12
#define STR_TEXT_MAX 48
struct str_entry {
    char text[STR_TEXT_MAX];
    void *obj; /* Hostadresse des midp::String */
};
static struct str_entry g_strings[STR_CACHE_MAX];
static int g_string_count;

static void *make_guest_string(const char *text)
{
    guest_new_fn gnew;
    guest_strctor_fn gctor;
    guest_addref_fn gaddref;
    void *obj;
    int i;

    if (!text || !g_code)
        return NULL;

    gaddref = (guest_addref_fn)(uintptr_t)g2h(MP_ADDR_STRING_ADDREF);

    for (i = 0; i < g_string_count; i++) {
        if (strncmp(g_strings[i].text, text, STR_TEXT_MAX - 1) == 0) {
            gaddref(g_strings[i].obj); /* Referenz fuer den Aufrufer */
            return g_strings[i].obj;
        }
    }
    if (g_string_count >= STR_CACHE_MAX) {
        mp_logf("String-Cache voll, '%s' nicht erzeugt", text);
        return NULL;
    }

    gnew = (guest_new_fn)(uintptr_t)g2h(MP_ADDR_OPERATOR_NEW);
    gctor = (guest_strctor_fn)(uintptr_t)g2h(MP_ADDR_STRING_CTOR);

    obj = gnew(0x14);
    if (!obj) {
        mp_logf("operator new(0x14) lieferte 0");
        return NULL;
    }
    gctor(obj, text);
    gaddref(obj); /* Dauerreferenz der Transportschicht (0 -> 1) */

    i = g_string_count++;
    snprintf(g_strings[i].text, STR_TEXT_MAX, "%s", text);
    g_strings[i].obj = obj;

    gaddref(obj); /* Referenz fuer den Aufrufer (1 -> 2) */
    return obj;
}

/* --------------------------------------------------------------------------
 * Stub-Thunks
 *
 * Aufrufkonvention wie im Spiel (stubs-session.md §4, stubs-lobby-race.md §4):
 *   4 Byte  : void f(MPM*)           bzw. void f(MPM*, int)
 *   8 Byte  : int  f(MPM*)           bzw. int  f(MPM*, int)
 *   12 Byte : void* f(Ptr<String>* sret, MPM*[, int]) — r0 ist der
 *             Rueckgabeslot, der Rueckgabewert ist wieder r0.
 * SOFTFP: alle rein Zeiger/Integer.
 */
static void t_host_lobby_reenter(void *m) { mp_host_lobby_reenter(h2g(m)); }
static void t_enter_lobby_state(void *m) { mp_enter_lobby_state(h2g(m)); }
static void t_host_close_lobby(void *m) { mp_host_close_lobby(h2g(m)); }
static void t_host_request_start(void *m) { mp_host_request_start(h2g(m)); }
static void t_set_browsing(void *m, void *list) { mp_set_browsing(h2g(m), h2g(list)); }
static int32_t t_get_host_count(void *m) { return mp_get_host_count(h2g(m)); }
static void t_connect_to_host(void *m, int32_t i) { mp_connect_to_host(h2g(m), i); }
static int32_t t_accept_peer(void *m) { return mp_accept_peer(h2g(m)); }
static void t_decline_peer(void *m) { mp_decline_peer(h2g(m)); }
static void t_cancel_connect(void *m) { mp_cancel_connect(h2g(m)); }
static void t_resume_browsing(void *m) { mp_resume_browsing(h2g(m)); }
static void t_host_start_countdown(void *m) { mp_host_start_countdown(h2g(m)); }
static void t_begin_race_handshake(void *m) { mp_begin_race_handshake(h2g(m)); }
static void t_on_host_selected(void *m, int32_t i) { mp_on_host_selected(h2g(m), i); }

static int32_t t_get_player_count(void *m) { return mp_get_player_count(h2g(m)); }
static int32_t t_get_peer_lobby_state(void *m, int32_t i)
{
    return mp_get_peer_lobby_state(h2g(m), i);
}
static int32_t t_has_connection_problem(void *m) { return mp_has_connection_problem(h2g(m)); }
static int32_t t_has_pending_join_request(void *m) { return mp_has_pending_join_request(h2g(m)); }
static int32_t t_has_rematch_sync_failed(void *m) { return mp_has_rematch_sync_failed(h2g(m)); }
static int32_t t_is_peer_ready(void *m, int32_t i) { return mp_is_peer_ready(h2g(m), i); }
static void t_kick_peer(void *m, int32_t i) { mp_kick_peer(h2g(m), i); }
static void t_send_race_loaded(void *m) { mp_send_race_loaded(h2g(m)); }
static void t_send_race_abort(void *m, int32_t reason) { mp_send_race_abort(h2g(m), reason); }
/* MPM_Clear51c 0x4a0a8528 ist eine echte Funktion (12 Byte: mov r1,#0 ;
 * strb r1,[r0,#0x51c] ; bx lr), die der Kern komplett ersetzt — kein
 * sret-Stub, obwohl sie 12 Byte gross ist. */
static void t_clear_51c(void *m) { mp_clear_51c(h2g(m)); }

/* Die beiden Aufrufstellen sind nur am Ruecksprungziel unterscheidbar. */
static void t_send_local_race_end(void *m, int32_t position)
{
    uint32_t lr = h2g(__builtin_return_address(0));
    int32_t from_leave = (lr == LR_RACE_END_LEAVE) ? 1 : 0;
    if (lr != LR_RACE_END_LEAVE && lr != LR_RACE_END_FINISH) {
        static int warned;
        if (!warned) {
            warned = 1;
            mp_logf("SendLocalRaceEnd aus unbekannter Stelle (LR %08x), "
                    "behandle als Ziellinie", (unsigned)lr);
        }
    }
    mp_send_local_race_end(h2g(m), position, from_leave);
}

/* sret-Stubs: der Kern liefert den Text, wir das Gastobjekt. */
static void *t_host_name(void *sret, void *m, int32_t index)
{
    *(void **)sret = make_guest_string(mp_host_name(h2g(m), index));
    return sret;
}
static void *t_requesting_peer_name(void *sret, void *m)
{
    *(void **)sret = make_guest_string(mp_requesting_peer_name(h2g(m)));
    return sret;
}
static void *t_lobby_peer_name(void *sret, void *m, int32_t i)
{
    *(void **)sret = make_guest_string(mp_lobby_peer_name(h2g(m), i));
    return sret;
}
static void *t_hud_peer_name(void *sret, void *m, int32_t i)
{
    *(void **)sret = make_guest_string(mp_hud_peer_name(h2g(m), i));
    return sret;
}

/* Zuordnung Gastadresse -> Thunk.  Nach Adresse, nicht nach Position, damit
 * eine Umsortierung von mp_stubs[] nichts kaputtmacht. */
struct stub_impl {
    uint32_t addr;
    void *fn;
    const char *name;
};
static const struct stub_impl g_stub_impls[] = {
    { 0x4a0a7ec8u, (void *)t_host_lobby_reenter,      "HostLobbyReenter" },
    { 0x4a0a7c28u, (void *)t_enter_lobby_state,       "EnterLobbyState" },
    { 0x4a0a7cb8u, (void *)t_host_close_lobby,        "HostCloseLobby" },
    { 0x4a0a8060u, (void *)t_host_request_start,      "HostRequestStart" },
    { 0x4a0a7ea4u, (void *)t_set_browsing,            "SetBrowsing" },
    { 0x4a0a7eacu, (void *)t_get_host_count,          "GetHostCount" },
    { 0x4a0a7e8cu, (void *)t_host_name,               "GetHostName" },
    { 0x4a0a7ef8u, (void *)t_connect_to_host,         "ConnectToHost" },
    { 0x4a0a7e78u, (void *)t_requesting_peer_name,    "GetRequestingPeerName" },
    { 0x4a0a7ffcu, (void *)t_accept_peer,             "AcceptPeer" },
    { 0x4a0a7ea8u, (void *)t_decline_peer,            "DeclinePeer" },
    { 0x4a0a8004u, (void *)t_cancel_connect,          "CancelConnect" },
    { 0x4a0a7e54u, (void *)t_resume_browsing,         "ResumeBrowsing" },
    { 0x4a0a7cb4u, (void *)t_host_start_countdown,    "HostStartCountdown" },
    { 0x4a0a81e0u, (void *)t_begin_race_handshake,    "BeginRaceHandshake" },
    { 0x4a0a803cu, (void *)t_on_host_selected,        "OnHostSelected" },
    { 0x4a0a8064u, (void *)t_get_player_count,        "GetPlayerCount" },
    { 0x4a0a7c64u, (void *)t_lobby_peer_name,         "GetLobbyPeerName" },
    { 0x4a0a7e84u, (void *)t_get_peer_lobby_state,    "GetPeerLobbyState" },
    { 0x4a0a81acu, (void *)t_has_connection_problem,  "HasConnectionProblem" },
    { 0x4a0a7cacu, (void *)t_has_pending_join_request,"HasPendingJoinRequest" },
    { 0x4a0a81e4u, (void *)t_has_rematch_sync_failed, "HasRematchSyncFailed" },
    { 0x4a0a8ad0u, (void *)t_is_peer_ready,           "IsPeerReady" },
    { 0x4a0a8988u, (void *)t_kick_peer,               "KickPeer" },
    { 0x4a0a7f10u, (void *)t_send_local_race_end,     "SendLocalRaceEnd" },
    { 0x4a0a7ea0u, (void *)t_send_race_loaded,        "SendRaceLoaded" },
    { 0x4a0a806cu, (void *)t_hud_peer_name,           "GetHudPeerName" },
    { 0x4a0a89a0u, (void *)t_send_race_abort,         "SendRaceAbort" },
    { 0x4a0a8528u, (void *)t_clear_51c,               "Clear51c" },
};
#define STUB_IMPL_COUNT (sizeof g_stub_impls / sizeof g_stub_impls[0])

static const struct stub_impl *find_stub_impl(uint32_t addr)
{
    size_t i;
    for (i = 0; i < STUB_IMPL_COUNT; i++)
        if (g_stub_impls[i].addr == addr)
            return &g_stub_impls[i];
    return NULL;
}

/* --------------------------------------------------------------------------
 * bl-Hooks auf echte Spielfunktionen (mp_hooks)
 *
 * Die Aufrufstelle bekommt "bl <Veneer>"; das Veneer springt (kein zweites
 * bl!) in unsere Implementierung, deren Rueckkehr damit direkt hinter die
 * Aufrufstelle fuehrt.  Registerbelegung = die der ersetzten Instruktion.
 */
static uint32_t g_hook_orig[3]; /* Gastadresse der Originalfunktion je Hook */
enum { HK_SCREEN2, HK_STATE4TO5, HK_COUNTDOWN };

typedef void (*orig_v5_fn)(void *a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4);
typedef int32_t (*orig_i1_fn)(void *a0);

/* 0x4a08dba4: ersetzt "str r1,[r0,#0x524]" im Fall Screen 2 (Fehler 17).
 * r0 = MPM (aus MPM_GetInstance @0x4a08db9c); r1 traegt dank des Wort-Patches
 * auf 0x4a08dba0 ("mov r1,#0x11" -> "mov r1,r4") den Flow-Zeiger, denn r4 ist
 * dort das Flow-Objekt (verify-stubs-session.md §1.1: 0x4a08db78
 * "strb r0,[r4,#0x85f]").  Deshalb reicht hier eine gewoehnliche C-Funktion;
 * ohne diesen Wort-Patch braeuchte es ein Veneer, das r4 nach r1 schiebt. */
static void h_screen2_enter(void *mpm, void *flow)
{
    mp_screen2_enter(h2g(mpm), h2g(flow));
}

/* 0x4a11fab8: bl MPM_State4to5(MPM*, idx, score, stat1, stat2).  Der Kern
 * bekommt die Werte, danach laeuft das Original (das sie verwirft). */
static void h_state4to5(void *mpm, int32_t slot, int32_t score, int32_t a, int32_t b)
{
    mp_on_state4to5(h2g(mpm), slot, score, a, b);
    if (g_hook_orig[HK_STATE4TO5])
        ((orig_v5_fn)(uintptr_t)g2h(g_hook_orig[HK_STATE4TO5]))(mpm, slot, score, a, b);
}

/* 0x4a12c80c: bl MPM_UpdateCountdown(MPM*) -> int.  Der Kern laeuft ZUERST,
 * damit er +0x51d/+0x538/T0 noch im selben Frame setzen kann, bevor das
 * Original sie auswertet; zurueckgegeben wird das Ergebnis des Originals. */
static int32_t h_countdown_gate(void *mpm)
{
    mp_on_countdown_gate(h2g(mpm));
    if (g_hook_orig[HK_COUNTDOWN])
        return ((orig_i1_fn)(uintptr_t)g2h(g_hook_orig[HK_COUNTDOWN]))(mpm);
    return 0;
}

struct hook_impl {
    uint32_t addr;    /* dokumentierte Aufrufstelle, Rueckfallzuordnung */
    const char *key;  /* Teilstring in mp_hook.name                      */
    void *fn;
    int slot;
    const char *name;
};
static const struct hook_impl g_hook_impls[] = {
    { HOOK_ADDR_SCREEN2,   "screen2",   (void *)h_screen2_enter,  HK_SCREEN2,   "Screen2Enter" },
    { HOOK_ADDR_STATE4TO5, "4to5",      (void *)h_state4to5,      HK_STATE4TO5, "State4to5" },
    { HOOK_ADDR_COUNTDOWN, "countdown", (void *)h_countdown_gate, HK_COUNTDOWN, "CountdownGate" },
};
#define HOOK_IMPL_COUNT (sizeof g_hook_impls / sizeof g_hook_impls[0])

static int name_has(const char *name, const char *key)
{
    size_t i, j;
    if (!name)
        return 0;
    for (i = 0; name[i]; i++) {
        for (j = 0; key[j]; j++) {
            char a = name[i + j];
            char b = key[j];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (!key[j])
            return 1;
    }
    return 0;
}

static const struct hook_impl *find_hook_impl(const struct mp_hook *h)
{
    size_t i;
    for (i = 0; i < HOOK_IMPL_COUNT; i++)
        if (name_has(h->name, g_hook_impls[i].key))
            return &g_hook_impls[i];
    for (i = 0; i < HOOK_IMPL_COUNT; i++)
        if (h->addr == g_hook_impls[i].addr)
            return &g_hook_impls[i];
    return NULL;
}

/* --------------------------------------------------------------------------
 * Import-Trampoline
 */
/* Offset des absoluten Zielzeigers, aus dem Opcode abgeleitet, damit alle vier
 * Varianten aus FUN_0000ef20 abgedeckt sind (meego-runtime-patch.md §2). */
static int tramp_target_off(const unsigned char *t)
{
    uint32_t w0 = get32(t);
    if (w0 == OP_LDR_PC_PC_M4)
        return 4; /* ohne Thunks, ARM        */
    if (w0 == 0xe59fc000u)
        return 8; /* mit Thunks, ARM  <- N9  */
    if ((w0 & 0xffffu) == 0x4778u)
        return 0xc; /* Thumb-Einsprung       */
    return -1;
}

static void *hook_import(int export_index, void *impl)
{
    unsigned char *t;
    int off;
    void *old = NULL;
    if ((uint32_t)export_index >= g_nexports) {
        mp_logf("Export %d liegt ausserhalb der Trampolintabelle (%u)",
                export_index, (unsigned)g_nexports);
        return NULL;
    }
    t = g_tramp + (uint32_t)export_index * g_trampsz;
    off = tramp_target_off(t);
    if (off < 0) {
        mp_logf("Trampolin %d: unbekanntes Format (%08x)", export_index,
                (unsigned)get32(t));
        return NULL;
    }
    memcpy(&old, t + off, 4);
    put32(t + off, (uint32_t)(uintptr_t)impl);
    return old;
}

/* Frame-Pump: s3eDeviceYield wird an 34 Stellen gerufen (Mainloop und alle
 * Ladeschleifen).  Der Hook laeuft hinter dem Stack-Switch-Thunk, also auf dem
 * grossen Loader-Stack — genau richtig fuer Socket-I/O.
 * SOFTFP: s3eDeviceYield(uint32 ms) ist rein integer. */
static void hook_yield(uint32_t ms)
{
    if (g_inited)
        mp_pump(current_mpm());
    if (g_real_yield)
        g_real_yield(ms);
}

/* Uhrenversatz: das Spiel leitet JEDE Multiplayer-Zeit aus s3eTimerGetUTC ab
 * (DESIGN_LAN.md §2.3, genau eine Aufrufstelle in GetTimeMs64 0x4a0dbf74).
 * s3eTimerGetMs (Export 488) bleibt absichtlich unangetastet.
 * Die int64-Rueckgabe liegt in r0/r1 — genau wie beim Original, sonst wuerde
 * der Stack-Switch-Thunk (FUN_0004e980, nach Rueckgabegroesse ausgewaehlt) das
 * obere Wort verwerfen. */
static int64_t hook_utc(void)
{
    int64_t v = g_real_utc ? g_real_utc() : 0;
    return v + g_clock_offset;
}

/* --------------------------------------------------------------------------
 * Veneers im freien RWX-Schwanz
 */
static uint32_t emit_abs_jump(uint32_t target)
{
    unsigned char *p = g_pad;
    if (!p || p + 8 > g_padend)
        return 0;
    put32(p + 0, OP_LDR_PC_PC_M4);
    put32(p + 4, target);
    g_pad += 8;
    return (uint32_t)(uintptr_t)p;
}

/* Schreibt "b"/"bl <veneer>" an site; liefert 0 bei Erfolg. */
static int emit_branch(unsigned char *site, uint32_t opbase, void *impl)
{
    uint32_t veneer = emit_abs_jump((uint32_t)(uintptr_t)impl);
    int32_t rel;
    if (!veneer)
        return -1; /* Schwanz voll */
    rel = (int32_t)(veneer - ((uint32_t)(uintptr_t)site + 8)) >> 2;
    if (rel > 0x7fffff || rel < -0x800000)
        return -2; /* ausserhalb +-32 MB */
    put32(site, opbase | ((uint32_t)rel & 0x00ffffffu));
    clear_cache(site, site + 4);
    return 0;
}

/* room >= 8 : inline "ldr pc,[pc,#-4]; .word impl"
 * room == 4 : "b <veneer>" mit Veneer im Schwanz                           */
static int patch_stub(uint32_t guest_addr, uint8_t room, void *impl)
{
    unsigned char *site = g2h(guest_addr);
    if (room >= 8) {
        put32(site + 0, OP_LDR_PC_PC_M4);
        put32(site + 4, (uint32_t)(uintptr_t)impl);
        clear_cache(site, site + 8);
        return 0;
    }
    if (room != 4)
        return -3;
    return emit_branch(site, OP_B_BASE, impl);
}

/* --------------------------------------------------------------------------
 * Patchlauf
 */
static void apply_code_patches(void)
{
    size_t i, ok = 0, bad = 0;
    for (i = 0; i < mp_code_patch_count; i++) {
        const struct mp_patch *p = &mp_code_patches[i];
        unsigned char *site = g2h(p->addr);
        uint32_t cur = get32(site);
        if (cur == p->value) {
            ok++;
            continue; /* schon gepatcht */
        }
        if (cur != p->expect) {
            mp_logf("Patch %08x (%s): %08x erwartet, %08x gefunden — uebersprungen",
                    (unsigned)p->addr, p->what ? p->what : "?",
                    (unsigned)p->expect, (unsigned)cur);
            bad++;
            continue;
        }
        put32(site, p->value);
        clear_cache(site, site + 4);
        ok++;
    }
    mp_logf("Wort-Patches: %u von %u angewandt%s", (unsigned)ok,
            (unsigned)mp_code_patch_count, bad ? " (mit Abweichungen!)" : "");
}

static void apply_stub_patches(void)
{
    size_t i, ok = 0, bad = 0;
    for (i = 0; i < mp_stub_count; i++) {
        const struct mp_stub *s = &mp_stubs[i];
        const struct stub_impl *impl = find_stub_impl(s->addr);
        uint32_t cur;
        int rc;
        if (!impl) {
            mp_logf("Stub %08x (%s): keine Implementierung in preload.c",
                    (unsigned)s->addr, s->name ? s->name : "?");
            bad++;
            continue;
        }
        cur = get32(g2h(s->addr));
        if (s->expect && cur != s->expect) {
            mp_logf("Stub %08x (%s): Rumpf %08x, erwartet %08x — uebersprungen",
                    (unsigned)s->addr, impl->name, (unsigned)cur,
                    (unsigned)s->expect);
            bad++;
            continue;
        }
        rc = patch_stub(s->addr, s->size, impl->fn);
        if (rc != 0) {
            mp_logf("Stub %08x (%s, %u B): Umleitung fehlgeschlagen (%d)",
                    (unsigned)s->addr, impl->name, (unsigned)s->size, rc);
            bad++;
            continue;
        }
        ok++;
    }
    mp_logf("Stub-Umleitungen: %u von %u", (unsigned)ok, (unsigned)mp_stub_count);
    if (bad)
        mp_logf("ACHTUNG: %u Stubs nicht umgeleitet", (unsigned)bad);
}

static void apply_hooks(void)
{
    size_t i, ok = 0, bad = 0;
    for (i = 0; i < mp_hook_count; i++) {
        const struct mp_hook *h = &mp_hooks[i];
        const struct hook_impl *impl = find_hook_impl(h);
        unsigned char *site;
        uint32_t cur;
        int rc;
        if (!impl) {
            mp_logf("Hook %08x (%s): keine Implementierung in preload.c",
                    (unsigned)h->addr, h->name ? h->name : "?");
            bad++;
            continue;
        }
        site = g2h(h->addr);
        cur = get32(site);
        if (h->expect && cur != h->expect) {
            mp_logf("Hook %08x (%s): %08x, erwartet %08x — uebersprungen",
                    (unsigned)h->addr, impl->name, (unsigned)cur,
                    (unsigned)h->expect);
            bad++;
            continue;
        }
        g_hook_orig[impl->slot] = h->orig;
        rc = emit_branch(site, OP_BL_BASE, impl->fn);
        if (rc != 0) {
            mp_logf("Hook %08x (%s): bl-Umleitung fehlgeschlagen (%d)",
                    (unsigned)h->addr, impl->name, rc);
            bad++;
            continue;
        }
        ok++;
    }
    mp_logf("bl-Hooks: %u von %u (Veneer-Schwanz: %u von %u Byte belegt)",
            (unsigned)ok, (unsigned)mp_hook_count,
            (unsigned)(g_pad - g_padstart), (unsigned)(g_padend - g_padstart));
    if (bad)
        mp_logf("ACHTUNG: %u Hooks nicht gesetzt", (unsigned)bad);
}

static const char *player_name(void)
{
    const char *n = getenv("NFSMP_NAME");
    if (n && *n)
        return n;
    n = getenv("USER");
    if (n && *n)
        return n;
    return "N9";
}

static void do_patch(void *imgv)
{
    unsigned char *img = imgv;
    uint32_t magic, base, total, memsz, trampsz;
    unsigned char *code = NULL, *data = NULL, *tramp = NULL;

    if (g_patched)
        return;

    magic = get32(img + IMG_MAGIC);
    base = get32(img + IMG_BASE);
    trampsz = get32(img + IMG_TRAMPSZ);
    memcpy(&code, img + IMG_CODE, 4);
    memcpy(&data, img + IMG_DATA, 4);
    memcpy(&tramp, img + IMG_TRAMP, 4);
    total = get32(img + IMG_TOTAL);
    memsz = get32(img + IMG_MEMSZ);
    g_split = get32(img + IMG_SPLIT);

    /* Plausibilitaet (meego-runtime-patch.md §4.1) */
    if (magic != IMG_MAGIC_VALUE || base != MP_GUEST_BASE ||
        (trampsz != 8 && trampsz != 0x10 && trampsz != 0x14) ||
        g_split != MP_GUEST_SPLIT || !code || !data || !tramp ||
        memsz <= g_split || total < g_split) {
        mp_logf("unerwartetes Image: magic=%08x base=%08x split=%08x tsz=%u "
                "code=%p data=%p tramp=%p total=%08x memsz=%08x — kein Patch",
                (unsigned)magic, (unsigned)base, (unsigned)g_split,
                (unsigned)trampsz, (void *)code, (void *)data, (void *)tramp,
                (unsigned)total, (unsigned)memsz);
        return;
    }
    if (tramp != code + g_split)
        mp_logf("WARNUNG: Trampolinbasis %p != code+split %p", (void *)tramp,
                (void *)(code + g_split));

    g_code = code;
    g_data = data;
    g_tramp = tramp;
    g_trampsz = trampsz;
    g_datasz = memsz - g_split;

    g_nexports = 511;
    if (addr_mapped(LDR_NEXPORTS, 4)) {
        uint32_t n = *(volatile uint32_t *)(uintptr_t)LDR_NEXPORTS;
        if (n >= 16 && n <= 4096)
            g_nexports = n;
        else
            mp_logf("WARNUNG: Exportzahl %u unplausibel, benutze 511", (unsigned)n);
    }

    /* Freier RWX-Schwanz: hinter Trampolintabelle + Thunk-Pool bis zum Ende der
     * auf 4 KB aufgerundeten Code-Allokation (meego-runtime-patch.md §1.4). */
    g_padstart = g_tramp + g_nexports * g_trampsz + THUNK_POOL_SIZE;
    g_pad = g_padstart;
    g_padend = g_code + ((total + 0xfff) & ~0xfffu);
    if (g_pad > g_padend)
        g_pad = g_padend = g_padstart; /* kein Platz -> Veneers scheitern sauber */

    mp_logf("Image: code=%p data=%p split=%08x memsz=%08x tramp=%p tsz=%u "
            "nexp=%u pad=%p..%p (%d B)",
            (void *)g_code, (void *)g_data, (unsigned)g_split, (unsigned)memsz,
            (void *)g_tramp, (unsigned)g_trampsz, (unsigned)g_nexports,
            (void *)g_pad, (void *)g_padend, (int)(g_padend - g_pad));

    /* 1) Import-Hooks (nur ein Datenwort, kein Reichweitenproblem) */
    g_real_yield = (yield_fn)hook_import(EXP_S3E_DEVICE_YIELD, (void *)hook_yield);
    if (!g_real_yield)
        mp_logf("ACHTUNG: s3eDeviceYield (336) nicht gehookt — keine Pump!");
    g_real_utc = (utc_fn)hook_import(EXP_S3E_TIMER_GET_UTC, (void *)hook_utc);
    if (!g_real_utc)
        mp_logf("ACHTUNG: s3eTimerGetUTC (490) nicht gehookt — keine Uhrensync!");
    mp_logf("Trampoline: yield336 alt=%p, utc490 alt=%p", (void *)g_real_yield,
            (void *)g_real_utc);

    /* 2) Wort-Patches, 3) Stubs, 4) bl-Hooks */
    apply_code_patches();
    apply_stub_patches();
    apply_hooks();

    /* Der Loader flusht direkt hinter diesem mprotect() ohnehin den gesamten
     * Codebereich (FUN_0000ef20 @0xf41c); wir tun es zur Sicherheit selbst. */
    clear_cache(g_code, g_padend);

    g_patched = 1;

    mp_init(&g_platform, player_name());
    g_inited = 1;
    mp_logf("bereit, Spielername '%s'", player_name());
}

/* --------------------------------------------------------------------------
 * Image finden
 */
static void *find_image(void)
{
    void *img;
    if (!addr_mapped(LDR_IMAGE_GLOBAL, 4))
        return NULL;
    img = *(void **)(uintptr_t)LDR_IMAGE_GLOBAL;
    if (!img || ((uintptr_t)img & 3))
        return NULL;
    if (!addr_mapped((uint32_t)(uintptr_t)img, 0x100))
        return NULL;
    return img;
}

/* --------------------------------------------------------------------------
 * mprotect-Interposition — der deterministische Patch-Zeitpunkt
 */
int mprotect(void *addr, size_t len, int prot)
{
    static int (*real)(void *, size_t, int);
    if (!real)
        real = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "mprotect");

    if (!g_patched && prot == (PROT_READ | PROT_EXEC) && len > 0x100000) {
        void *img = find_image();
        uint32_t code = 0;
        if (img)
            memcpy(&code, (unsigned char *)img + IMG_CODE, 4);
        if (img && code == (uint32_t)(uintptr_t)addr) {
            int rc = real(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC);
            if (rc == 0) {
                do_patch(img);
            } else {
                mp_logf("mprotect(RWX) fehlgeschlagen: %s", strerror(errno));
                rc = real(addr, len, prot);
            }
            return rc;
        }
        mp_logf("mprotect(%p,%08x,R|X) ohne passendes Image (img=%p code=%08x)",
                addr, (unsigned)len, img, (unsigned)code);
    }
    return real(addr, len, prot);
}

/* --------------------------------------------------------------------------
 * Fallback (meego-runtime-patch.md §4.2): nur mit NFSMP_FALLBACK=1.
 * Pollt das Image-Global, bis das Laden nachweislich fertig ist, und schaltet
 * selbst auf RWX.  Gedacht fuer den Fall, dass die mprotect-Interposition auf
 * dem Geraet wider Erwarten nicht greift.
 */
static void *poll_thread(void *arg)
{
    int tries;
    (void)arg;
    for (tries = 0; tries < 600 && !g_patched; tries++) {
        void *img = find_image();
        if (img) {
            unsigned char *code = NULL;
            uint32_t total = get32((unsigned char *)img + IMG_TOTAL);
            uint32_t split = get32((unsigned char *)img + IMG_SPLIT);
            memcpy(&code, (unsigned char *)img + IMG_CODE, 4);
            if (code && split == MP_GUEST_SPLIT && total > split &&
                get32((unsigned char *)img + 0xec) == 0 &&
                get32(code + split) != 0 && (get32(code + 0x634) >> 24) == 0xea &&
                get32(code + 0x634) != 0xeafffffeu) {
                size_t len = (total + 0xfff) & ~(size_t)0xfff;
                mp_logf("Fallback: patche ohne mprotect-Hook");
                if (mprotect(code, len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
                    do_patch(img);
                else
                    mp_logf("Fallback-mprotect fehlgeschlagen: %s", strerror(errno));
                return NULL;
            }
        }
        usleep(50000);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Konstruktor / Destruktor
 */
static void reserve_guest_window(void)
{
    /* Siehe g2h/h2g: Heap-Adressen werden 1:1 als Gastadressen benutzt.  Damit
     * das eindeutig bleibt, wird das Gastfenster einmalig belegt, bevor der
     * Loader seine grossen Allokationen macht. */
    size_t len = 0x202000; /* memsz 0x2000c4, aufgerundet + Reserve */
    void *p = mmap((void *)(uintptr_t)MP_GUEST_BASE, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        mp_logf("WARNUNG: Gastfenster 0x%08x nicht reservierbar: %s",
                (unsigned)MP_GUEST_BASE, strerror(errno));
        return;
    }
    if (p != (void *)(uintptr_t)MP_GUEST_BASE) {
        munmap(p, len);
        mp_logf("WARNUNG: Gastfenster 0x%08x bereits belegt — Heap-Adressen in "
                "[0x4a000000,+0x2000c4) wuerden fehlinterpretiert",
                (unsigned)MP_GUEST_BASE);
    }
}

__attribute__((constructor)) static void nfsmp_ctor(void)
{
    const char *q = getenv("NFSMP_QUIET");
    g_quiet = (q && *q && *q != '0');
    if (getenv("NFSMP_DISABLE")) {
        g_patched = 1; /* nichts tun, mprotect bleibt transparent */
        mp_logf("NFSMP_DISABLE gesetzt — Patcher inaktiv");
        return;
    }
    reserve_guest_window();
    mp_logf("geladen (Protokoll v%d, Ports %d/%d)", MP_PROTOCOL_VERSION,
            MP_DISCOVERY_PORT, MP_SESSION_PORT);
    if (getenv("NFSMP_FALLBACK")) {
        pthread_t th;
        if (pthread_create(&th, NULL, poll_thread, NULL) == 0)
            pthread_detach(th);
    }
}

__attribute__((destructor)) static void nfsmp_dtor(void)
{
    if (g_inited) {
        g_inited = 0;
        mp_shutdown();
    }
}
