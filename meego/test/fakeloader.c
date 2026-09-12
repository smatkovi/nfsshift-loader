/*
 * meego/test/fakeloader.c — baut die Speicherlage des MeeGo-Loaders nach und
 * prueft libnfsmp.so unter qemu-arm end-to-end.
 *
 * Nachgebildet werden (meego-runtime-patch.md §1):
 *   - Codeblock per posix_memalign + Guard-Seiten, Datenblock per malloc,
 *   - 511 Import-Trampoline a 0x10 Byte (Variante "mit Thunks, arch != 0/1")
 *     samt Stack-Switch-Thunk,
 *   - der 0x2000 grosse Thunk-Pool dahinter, danach der freie RWX-Schwanz,
 *   - die beiden Loader-Globals an ihren echten Adressen 0x7c328 / 0x6be38
 *     (per --section-start gelegt),
 *   - der EINE mprotect(code, len, PROT_READ|PROT_EXEC)-Aufruf am Ende des
 *     Ladens, der den Patcher ausloest.
 *
 * Im Fake-Image liegen ausserdem:
 *   - alle Stubruempfe (erstes Wort = mp_stubs[].expect, Rest nach Groesse),
 *   - die Originalwoerter aller Wort-Patches (inkl. der 9 Menuewoerter),
 *   - die Originalwoerter aller bl-Hooks, jeweils in einen winzigen Aufrufer
 *     eingebettet, damit sich die Hooks wie im Spiel aufrufen lassen,
 *   - Attrappen von operator new / midp::String-ctor / addRef /
 *     GamePeer_PushHistory / MPM_State4to5 / MPM_UpdateCountdown,
 *   - die MPM-Globale 0x4a1c2050,
 *   - ein Aufrufer, damit das LR an MPM_SendLocalRaceEnd eine Gastadresse ist.
 *
 * Exit-Code 0 = alle Pruefungen bestanden.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "nfsmp.h"
#include "core_dummy.h"

/* Die beiden Loader-Globals; Adressen kommen per --section-start */
__attribute__((section(".imgglob"), used)) volatile void *g_image_global;
__attribute__((section(".nexpglob"), used)) volatile uint32_t g_nexports = 511;

#define SPLIT   MP_GUEST_SPLIT /* 0x1c1078 */
#define MEMSZ   0x2000c4u
#define IMGSZ   0x1c4b14u
#define TRAMPSZ 0x10u
#define NEXP    511u
#define POOL    0x2000u

#define EXP_YIELD 336
#define EXP_UTC   490

#define FAKE_UTC_BASE 1700000000000LL

/* Gastadressen, die der Test im Image belegt */
#define CALLER_RACE_END 0x4a160000u /* -> MPM_SendLocalRaceEnd 0x4a0a7f10 */
#define ADDR_STATE4TO5  0x4a0a898cu /* MPM_State4to5                      */
#define ADDR_UPDATECD   0x4a0a7f74u /* MPM_UpdateCountdown                */

/* Aufrufer um die bl-Hook-Stellen herum */
#define WRAP_SCREEN2  0x4a08db98u /* Stelle 0x4a08dba4 */
#define WRAP_STATE45  0x4a11faacu /* Stelle 0x4a11fab8 */
#define WRAP_COUNTDN  0x4a12c808u /* Stelle 0x4a12c80c */

static unsigned char *g_code, *g_data, *g_tramp;
static unsigned char img[0x100];
static int g_fail;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("  ok     ");                                               \
        } else {                                                               \
            printf("  FEHLER ");                                               \
            g_fail++;                                                          \
        }                                                                      \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)

static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Gast -> Host im Fake-Image */
static unsigned char *gp(uint32_t a)
{
    uint32_t off = a - MP_GUEST_BASE;
    return off < SPLIT ? g_code + off : g_data + (off - SPLIT);
}

/* ---------------------------------------------------------------------------
 * Attrappen der Gastfunktionen
 */
#define FAKE_OBJ_MAX 16
static struct {
    void *obj;
    char text[64];
    int refs;
    int ctor_calls;
} g_objs[FAKE_OBJ_MAX];
static int g_obj_count;
static unsigned char g_obj_pool[FAKE_OBJ_MAX * 0x20];
static uint32_t g_last_new_size;

static int obj_index(void *o)
{
    int i;
    if (!o)
        return -1;
    for (i = 0; i < g_obj_count; i++)
        if (g_objs[i].obj == o)
            return i;
    return -1;
}

static void *fake_operator_new(uint32_t size)
{
    void *p;
    g_last_new_size = size;
    if (g_obj_count >= FAKE_OBJ_MAX)
        return NULL;
    p = g_obj_pool + g_obj_count * 0x20;
    g_objs[g_obj_count].obj = p;
    g_objs[g_obj_count].refs = 0;
    g_objs[g_obj_count].text[0] = 0;
    g_objs[g_obj_count].ctor_calls = 0;
    g_obj_count++;
    return p;
}

static void *fake_string_ctor(void *self, const char *text)
{
    int i = obj_index(self);
    if (i >= 0) {
        snprintf(g_objs[i].text, sizeof g_objs[i].text, "%s", text ? text : "");
        g_objs[i].ctor_calls++;
    }
    return self;
}

static void fake_string_addref(void *self)
{
    int i = obj_index(self);
    if (i >= 0)
        g_objs[i].refs++;
}

/* Originalfunktionen hinter den bl-Hooks */
static int g_s45_calls;
static void *g_s45_mpm;
static int32_t g_s45_args[4];
static void fake_state4to5(void *mpm, int32_t a, int32_t b, int32_t c, int32_t d)
{
    g_s45_calls++;
    g_s45_mpm = mpm;
    g_s45_args[0] = a;
    g_s45_args[1] = b;
    g_s45_args[2] = c;
    g_s45_args[3] = d;
}

static int g_cd_calls;
static int32_t fake_update_countdown(void *mpm)
{
    (void)mpm;
    g_cd_calls++;
    return 1;
}

/* Host/Join-Selektor, den glue.c per call_guest baut */
static int g_selinit_calls, g_seladd_calls;
static void *g_sel_obj;
static uint32_t g_sel_args[3];
static uint32_t fake_selector_init(void *sel, uint32_t a, uint32_t b, uint32_t text)
{
    g_selinit_calls++;
    g_sel_obj = sel;
    g_sel_args[0] = a;
    g_sel_args[1] = b;
    g_sel_args[2] = text;
    return 0x5e1ec704u;
}
static uint32_t fake_selector_add(void *sel, uint32_t text, uint32_t c, uint32_t d)
{
    (void)sel;
    (void)text;
    (void)c;
    (void)d;
    g_seladd_calls++;
    return 0;
}

/*
 * GamePeer_PushHistory-Attrappe, exakt nach der dokumentierten ABI
 * (mpm-fields.md §11.2): r0 = peer, r1..r3 = Samplebytes 0..11, Bytes
 * 12..0xa3 ab sp.  Die echte Funktion beginnt mit "push {r0-r3}" und liest das
 * Sample danach lueckenlos ab; genau das bildet dieser Rumpf nach.
 */
static void *g_push_peer;
static unsigned char g_push_sample[MP_SAMPLE_SIZE];
static int g_push_calls;

void fph_record(void *peer, const void *sample);
void fph_record(void *peer, const void *sample)
{
    g_push_peer = peer;
    memcpy(g_push_sample, sample, MP_SAMPLE_SIZE);
    g_push_calls++;
}

extern void fake_push_history(void);
__asm__(".text\n"
        ".align 2\n"
        ".arm\n"
        ".global fake_push_history\n"
        ".type fake_push_history, %function\n"
        "fake_push_history:\n"
        "    push {r0-r3}\n"      /* wie im Original                     */
        "    push {lr}\n"
        "    ldr  r0, [sp, #4]\n" /* peer                                */
        "    add  r1, sp, #8\n"   /* Sample ab Byte 0, lueckenlos        */
        "    bl   fph_record\n"
        "    pop  {lr}\n"
        "    add  sp, sp, #16\n"
        "    bx   lr\n"
        ".size fake_push_history, .-fake_push_history\n");

/* ---------------------------------------------------------------------------
 * Attrappen der beiden gehookten Loader-Importe
 */
static int g_yield_calls;
static uint32_t g_yield_ms;
static void fake_real_yield(uint32_t ms)
{
    g_yield_calls++;
    g_yield_ms = ms;
}

static int g_utc_calls;
static int64_t fake_real_utc(void)
{
    g_utc_calls++;
    return FAKE_UTC_BASE;
}

/* Stack-Switch-Thunk des Loaders, vereinfacht: ruft das Ziel in ip auf und
 * laesst r0..r3 unangetastet (auch fuer 64-Bit-Rueckgaben). */
extern void fake_thunk(void);
__asm__(".text\n"
        ".align 2\n"
        ".arm\n"
        ".global fake_thunk\n"
        ".type fake_thunk, %function\n"
        "fake_thunk:\n"
        "    push {r4, lr}\n"
        "    mov  r4, sp\n"
        "    mov  lr, pc\n"
        "    bx   ip\n"
        "    mov  sp, r4\n"
        "    pop  {r4, pc}\n"
        ".size fake_thunk, .-fake_thunk\n");

/* ---------------------------------------------------------------------------
 * Image aufbauen
 */
static void plant_abs_jump(uint32_t guest_addr, void *target)
{
    unsigned char *p = gp(guest_addr);
    put32(p + 0, 0xe51ff004u); /* ldr pc,[pc,#-4] */
    put32(p + 4, (uint32_t)(uintptr_t)target);
}

/* push {r4,lr} ; bl <target> ; pop {r4,pc}  — LR am Ziel = site+8 */
static void plant_caller(uint32_t site, uint32_t target)
{
    unsigned char *p = gp(site);
    int32_t rel = (int32_t)(target - (site + 4 + 8)) >> 2;
    put32(p + 0, 0xe92d4010u);
    put32(p + 4, 0xeb000000u | ((uint32_t)rel & 0x00ffffffu));
    put32(p + 8, 0xe8bd8010u);
}

static void plant_stub_bodies(void)
{
    size_t i;
    for (i = 0; i < mp_stub_count; i++) {
        unsigned char *p = gp(mp_stubs[i].addr);
        put32(p + 0, mp_stubs[i].expect);
        if (mp_stubs[i].size == 8) {
            put32(p + 4, 0xe12fff1eu); /* bx lr        */
        } else if (mp_stubs[i].size >= 12) {
            put32(p + 4, 0xe5801000u); /* str r1,[r0]  */
            put32(p + 8, 0xe12fff1eu); /* bx lr        */
        }
    }
}

static void plant_patch_sites(void)
{
    size_t i;
    for (i = 0; i < mp_code_patch_count; i++)
        put32(gp(mp_code_patches[i].addr), mp_code_patches[i].expect);
}

/* Die vier Hook-Stellen samt Aufrufer drumherum. */
static void plant_hook_sites(void)
{
    size_t i;
    unsigned char *p;

    for (i = 0; i < mp_hook_count; i++)
        put32(gp(mp_hooks[i].addr), mp_hooks[i].expect);

    /* Wie Flow::EnterScreen, Fall Screen 2:
     *   0x4a08db98  push {r4,lr}
     *   0x4a08db9c  mov r4,r1        (Aufrufer liefert den Flow in r1)
     *   0x4a08dba0  mov r1,#0x11     <- Wort-Patch -> "mov r1,r4"
     *   0x4a08dba4  str r1,[r0,#0x524] <- Hook-Stelle -> "bl <Veneer>"
     *   0x4a08dba8  pop {r4,pc}                                          */
    p = gp(WRAP_SCREEN2);
    put32(p + 0, 0xe92d4010u);
    put32(p + 4, 0xe1a04001u); /* mov r4, r1 */
    put32(p + 16, 0xe8bd8010u);

    /* 0x4a11faac: push {r4,lr} ; ldr r4,[sp,#8] ; push {r4} ;
     * <0x4a11fab8 = bl State4to5> ; add sp,sp,#4 ; pop {r4,pc}
     * (schiebt das 5. Argument wieder auf den Stack) */
    p = gp(WRAP_STATE45);
    put32(p + 0, 0xe92d4010u);
    put32(p + 4, 0xe59d4008u);  /* ldr r4,[sp,#8]      */
    put32(p + 8, 0xe52d4004u);  /* str r4,[sp,#-4]!    */
    put32(p + 16, 0xe28dd004u); /* add sp,sp,#4        */
    put32(p + 20, 0xe8bd8010u);

    /* 0x4a12c808: push {r4,lr} ; <0x4a12c80c = bl UpdateCountdown> ;
     * pop {r4,pc} */
    p = gp(WRAP_COUNTDN);
    put32(p + 0, 0xe92d4010u);
    put32(p + 8, 0xe8bd8010u);
}

/* ---------------------------------------------------------------------------
 * Pruefungen nach dem Patchlauf
 */
static unsigned char *pad_start(void) { return g_tramp + NEXP * TRAMPSZ + POOL; }
static unsigned char *pad_end(uint32_t total)
{
    return g_code + ((total + 0xfff) & ~0xfffu);
}

static int in_image(uint32_t a, uint32_t total)
{
    uint32_t c = (uint32_t)(uintptr_t)g_code;
    return a - c < total;
}

/* Prueft "b"/"bl <veneer>" an site und das Veneer selbst. */
static int check_branch(unsigned char *site, uint32_t opbyte, uint32_t total,
                        const char *what, const char *who)
{
    uint32_t w0 = get32(site);
    int32_t rel;
    unsigned char *tgt;
    uint32_t vw0, vw1;
    if ((w0 >> 24) != opbyte) {
        printf("  FEHLER %s %s: %08x ist kein %s\n", what, who, (unsigned)w0,
               opbyte == 0xea ? "b" : "bl");
        return 1;
    }
    rel = (int32_t)(w0 << 8) >> 6; /* sext24 * 4 */
    tgt = site + 8 + rel;
    if (tgt < pad_start() || tgt + 8 > pad_end(total)) {
        printf("  FEHLER %s %s: Ziel %p ausserhalb des Pads %p..%p\n", what, who,
               (void *)tgt, (void *)pad_start(), (void *)pad_end(total));
        return 1;
    }
    vw0 = get32(tgt);
    vw1 = get32(tgt + 4);
    if (vw0 != 0xe51ff004u || vw1 == 0 || in_image(vw1, total)) {
        printf("  FEHLER %s %s: Veneer %08x %08x\n", what, who, (unsigned)vw0,
               (unsigned)vw1);
        return 1;
    }
    return 0;
}

static void check_patches(void)
{
    size_t i, menu = 0, menu_ok = 0;
    int bad = 0;
    for (i = 0; i < mp_code_patch_count; i++) {
        const struct mp_patch *p = &mp_code_patches[i];
        uint32_t got = get32(gp(p->addr));
        int is_menu = (p->addr >= 0x4a1489e8u && p->addr <= 0x4a148af4u);
        if (is_menu)
            menu++;
        if (got == p->value) {
            if (is_menu)
                menu_ok++;
            continue;
        }
        bad++;
        printf("  FEHLER Wort-Patch %08x: %08x statt %08x (%s)\n",
               (unsigned)p->addr, (unsigned)got, (unsigned)p->value, p->what);
    }
    CHECK(bad == 0, "alle %u Wort-Patches angewandt", (unsigned)mp_code_patch_count);
    CHECK(menu_ok == menu && menu > 0, "Menue-Patch: %u von %u Woertern gesetzt",
          (unsigned)menu_ok, (unsigned)menu);
}

static void check_stub_redirects(uint32_t total)
{
    size_t i;
    int bad4 = 0, bad8 = 0, bad12 = 0, n4 = 0, n8 = 0, n12 = 0;
    for (i = 0; i < mp_stub_count; i++) {
        const struct mp_stub *s = &mp_stubs[i];
        unsigned char *site = gp(s->addr);
        uint32_t w0 = get32(site);
        int *bad = s->size == 4 ? &bad4 : (s->size == 8 ? &bad8 : &bad12);
        if (s->size == 4)
            n4++;
        else if (s->size == 8)
            n8++;
        else
            n12++;
        if (s->size >= 8) {
            uint32_t w1 = get32(site + 4);
            if (w0 != 0xe51ff004u || w1 == 0 || in_image(w1, total)) {
                (*bad)++;
                printf("  FEHLER Stub %08x (%s): %08x %08x\n", (unsigned)s->addr,
                       s->name, (unsigned)w0, (unsigned)w1);
            }
        } else {
            *bad += check_branch(site, 0xea, total, "Stub", s->name);
        }
    }
    CHECK(bad4 == 0, "%d 4-Byte-Stubs: b <Veneer> im RWX-Schwanz", n4);
    CHECK(bad8 == 0, "%d 8-Byte-Stubs: ldr pc,[pc,#-4] inline", n8);
    CHECK(bad12 == 0, "%d 12-Byte-Stubs: ldr pc,[pc,#-4] inline", n12);
}

static void check_hook_redirects(uint32_t total)
{
    size_t i;
    int bad = 0;
    for (i = 0; i < mp_hook_count; i++)
        bad += check_branch(gp(mp_hooks[i].addr), 0xeb, total, "Hook",
                            mp_hooks[i].name);
    CHECK(bad == 0, "%u bl-Hooks auf Veneer umgebogen", (unsigned)mp_hook_count);
}

int main(void)
{
    uint32_t tramp_total = TRAMPSZ * NEXP + POOL;
    uint32_t total = tramp_total + SPLIT;
    uint32_t alloc = ((total + 0xfff) >> 12) + 2;
    size_t mlen = (total + 0xfff) & ~0xfffu;
    void *raw = NULL;
    uint32_t i;
    struct mpdummy_state *(*probe)(void);
    struct mpdummy_state *st;
    void *mpm;
    uint32_t mpm_guest;

    if (posix_memalign(&raw, 0x1000, (size_t)alloc * 0x1000) != 0)
        return 1;
    g_code = (unsigned char *)raw + 0x1000;
    memset(g_code, 0, mlen);
    mprotect(raw, 0x1000, PROT_NONE);
    mprotect(g_code + mlen, 0x1000, PROT_NONE);
    mprotect(g_code, mlen, PROT_READ | PROT_WRITE | PROT_EXEC);

    g_data = malloc(MEMSZ - SPLIT);
    memset(g_data, 0, MEMSZ - SPLIT);
    g_tramp = g_code + SPLIT;

    /* Import-Trampoline (arch != 0/1, Thunks an) */
    for (i = 0; i < NEXP; i++) {
        unsigned char *t = g_tramp + i * TRAMPSZ;
        put32(t + 0, 0xe59fc000u); /* ldr r12,[pc] */
        put32(t + 4, 0xe59ff000u); /* ldr pc,[pc]  */
        put32(t + 8, 0xdead0000u + i);
        put32(t + 12, (uint32_t)(uintptr_t)fake_thunk);
    }
    put32(g_tramp + EXP_YIELD * TRAMPSZ + 8, (uint32_t)(uintptr_t)fake_real_yield);
    put32(g_tramp + EXP_UTC * TRAMPSZ + 8, (uint32_t)(uintptr_t)fake_real_utc);

    /* Thunk-Pool wie FUN_0004e4a0 */
    for (i = 0; i < POOL; i += 0x10) {
        unsigned char *t = g_tramp + NEXP * TRAMPSZ + i;
        put32(t + 0, 0xe59fc000u);
        put32(t + 4, 0xe59ff000u);
        put32(t + 8, 0);
        put32(t + 12, 0);
    }

    plant_stub_bodies();
    plant_patch_sites();
    plant_hook_sites();
    plant_abs_jump(MP_ADDR_OPERATOR_NEW, (void *)fake_operator_new);
    plant_abs_jump(MP_ADDR_STRING_CTOR, (void *)fake_string_ctor);
    plant_abs_jump(MP_ADDR_STRING_ADDREF, (void *)fake_string_addref);
    plant_abs_jump(MP_ADDR_PUSH_HISTORY, (void *)fake_push_history);
    plant_abs_jump(ADDR_STATE4TO5, (void *)fake_state4to5);
    plant_abs_jump(ADDR_UPDATECD, (void *)fake_update_countdown);
    plant_abs_jump(MP_ADDR_SELECTOR_INIT, (void *)fake_selector_init);
    plant_abs_jump(MP_ADDR_SELECTOR_ADD, (void *)fake_selector_add);
    plant_caller(CALLER_RACE_END, 0x4a0a7f10u);

    /* MPM-Singleton und die Globale 0x4a1c2050 */
    mpm = malloc(0x600);
    memset(mpm, 0, 0x600);
    mpm_guest = (uint32_t)(uintptr_t)mpm;
    put32(gp(MP_ADDR_MPM_GLOBAL), mpm_guest);

    /* Image-Struct */
    memset(img, 0, sizeof img);
    put32(img + 0x88, 0x55334558u);
    put32(img + 0x8c, 0x41e01u);
    put32(img + 0xa0, IMGSZ);
    put32(img + 0xa4, MEMSZ);
    put32(img + 0xb0, 0x17a024u);
    put32(img + 0xbc, MP_GUEST_BASE);
    put32(img + 0xc8, 8);
    put32(img + 0xcc, SPLIT);
    memcpy(img + 0xd8, &g_code, 4);
    put32(img + 0xdc, total);
    memcpy(img + 0xe0, &g_data, 4);
    memcpy(img + 0xf0, &g_tramp, 4);
    put32(img + 0xf4, TRAMPSZ);
    g_image_global = img;

    __builtin___clear_cache((char *)g_code, (char *)g_code + mlen);

    printf("fake: global@%p nexp@%p code=%p data=%p tramp=%p total=%08x\n",
           (void *)&g_image_global, (void *)&g_nexports, (void *)g_code,
           (void *)g_data, (void *)g_tramp, (unsigned)total);
    printf("fake: Pad erwartet %p..%p (%d B)\n", (void *)pad_start(),
           (void *)pad_end(total), (int)(pad_end(total) - pad_start()));
    printf("fake: Stub 0x4a0a8064 vor dem Patch = %d\n",
           ((int (*)(void *))gp(0x4a0a8064u))(NULL));

    /* === der eine Aufruf, der den Patcher ausloest === */
    mprotect(g_code, mlen, PROT_READ | PROT_EXEC);

    probe = (struct mpdummy_state * (*)(void)) dlsym(RTLD_DEFAULT, "mpdummy_get");
    if (!probe) {
        printf("fake: kein Preload aktiv (mpdummy_get nicht gefunden) — "
               "Basislauf beendet\n");
        printf("fake: Stub 0x4a0a8064 nach dem Patch = %d (unveraendert 0)\n",
               ((int (*)(void *))gp(0x4a0a8064u))(NULL));
        return 0;
    }
    st = probe();

    printf("\n== Image und Patches ==\n");
    CHECK(st->init_calls == 1, "mp_init() genau einmal gerufen, Name '%s'",
          st->last_name);
    check_patches();
    check_stub_redirects(total);
    check_hook_redirects(total);

    printf("\n== Stub-Umleitung funktional ==\n");
    CHECK(!(mpm_guest >= MP_GUEST_BASE && mpm_guest < MP_GUEST_BASE + MEMSZ),
          "Heap-MPM %08x liegt ausserhalb des Gastfensters", (unsigned)mpm_guest);

    /* 4 Byte: void f(MPM*) */
    ((void (*)(void *))gp(0x4a0a7cb8u))(mpm);
    CHECK(st->calls[D_HOST_CLOSE_LOBBY] == 1 && st->last_mpm == mpm_guest,
          "4-Byte-Stub HostCloseLobby: calls=%u mpm=%08x",
          (unsigned)st->calls[D_HOST_CLOSE_LOBBY], (unsigned)st->last_mpm);

    /* 4 Byte mit Index: void f(MPM*, int) */
    ((void (*)(void *, int))gp(0x4a0a803cu))(mpm, 7);
    CHECK(st->calls[D_ON_HOST_SELECTED] == 1 && st->last_i == 7,
          "4-Byte-Stub OnHostSelected(i=7): i=%d", (int)st->last_i);

    /* 8 Byte: int f(MPM*) */
    {
        int v = ((int (*)(void *))gp(0x4a0a8064u))(mpm);
        CHECK(v == DUMMY_PLAYER_COUNT, "8-Byte-Stub GetPlayerCount liefert %d", v);
        v = ((int (*)(void *))gp(0x4a0a7eacu))(mpm);
        CHECK(v == DUMMY_HOST_COUNT, "8-Byte-Stub GetHostCount liefert %d", v);
        v = ((int (*)(void *, int))gp(0x4a0a7e84u))(mpm, 2);
        CHECK(v == DUMMY_PEER_STATE && st->last_i == 2,
              "8-Byte-Stub GetPeerLobbyState(2) liefert %d", v);
    }

    /* Adressumrechnung */
    {
        unsigned char *dmpm = g_data + 0x100;
        uint32_t expect = MP_GUEST_BASE + SPLIT + 0x100;
        ((int (*)(void *))gp(0x4a0a8064u))(dmpm);
        CHECK(st->last_mpm == expect, "Host->Gast im Datenbereich: %08x (erwartet %08x)",
              (unsigned)st->last_mpm, (unsigned)expect);
        ((int (*)(void *))gp(0x4a0a8064u))(g_code + 0x2000);
        CHECK(st->last_mpm == MP_GUEST_BASE + 0x2000,
              "Host->Gast im Codebereich: %08x (erwartet %08x)",
              (unsigned)st->last_mpm, (unsigned)(MP_GUEST_BASE + 0x2000));
    }

    printf("\n== sret-Stubs und midp::String ==\n");
    {
        uint32_t slot = 0xdeadbeefu;
        void *ret = ((void *(*)(void *, void *, int))gp(0x4a0a7e8cu))(&slot, mpm, 1);
        int idx = obj_index((void *)(uintptr_t)slot);
        CHECK(ret == &slot, "12-Byte-Stub GetHostName: r0 zurueckgegeben");
        CHECK(idx >= 0, "sret-Slot zeigt auf ein erzeugtes midp::String (%08x)",
              (unsigned)slot);
        if (idx >= 0) {
            CHECK(g_last_new_size == 0x14, "operator new(0x%x)",
                  (unsigned)g_last_new_size);
            CHECK(strcmp(g_objs[idx].text, "HOST-1") == 0, "String-ctor bekam '%s'",
                  g_objs[idx].text);
            CHECK(g_objs[idx].refs == 2,
                  "Refcount nach erstem Getter = %d (1 Dauer + 1 Aufrufer)",
                  g_objs[idx].refs);
        }
        {   /* zweiter Aufruf: Cache-Treffer, nur addRef */
            uint32_t slot2 = 0;
            int before = g_obj_count;
            ((void *(*)(void *, void *, int))gp(0x4a0a7e8cu))(&slot2, mpm, 1);
            CHECK(slot2 == slot && g_obj_count == before,
                  "zweiter Aufruf liefert dasselbe Objekt (kein neues new)");
            if (idx >= 0)
                CHECK(g_objs[idx].refs == 3, "Refcount nach zweitem Getter = %d",
                      g_objs[idx].refs);
        }
    }
    {
        uint32_t slot = 0;
        int idx;
        ((void *(*)(void *, void *))gp(0x4a0a7e78u))(&slot, mpm);
        idx = obj_index((void *)(uintptr_t)slot);
        CHECK(idx >= 0 && strcmp(g_objs[idx].text, "JOINER") == 0,
              "GetRequestingPeerName (2 Argumente, sret) -> '%s'",
              idx >= 0 ? g_objs[idx].text : "?");
    }
    {   /* NULL vom Kern -> Nullzeiger in die sret-Zelle */
        uint32_t slot = 0xdeadbeefu;
        ((void *(*)(void *, void *, int))gp(0x4a0a7c64u))(&slot, mpm, 3);
        CHECK(slot == 0, "leerer Slot: sret-Zelle = %08x", (unsigned)slot);
    }

    printf("\n== GamePeer_PushHistory-ABI ==\n");
    {
        int ok = 1;
        ((void (*)(void *))gp(0x4a0a7ea0u))(mpm); /* SendRaceLoaded */
        for (i = 0; i < MP_SAMPLE_SIZE; i++)
            if (g_push_sample[i] != DUMMY_SAMPLE_BYTE(i))
                ok = 0;
        CHECK(g_push_calls == 1, "PushHistory einmal gerufen");
        CHECK(g_push_peer == (void *)((unsigned char *)mpm + DUMMY_PEER_OFFSET),
              "r0 = GamePeer %p (erwartet %p)", g_push_peer,
              (void *)((unsigned char *)mpm + DUMMY_PEER_OFFSET));
        CHECK(ok, "alle %d Samplebytes korrekt (r1..r3 + Stack)", MP_SAMPLE_SIZE);
    }

    printf("\n== SendLocalRaceEnd: Aufrufstelle aus dem LR ==\n");
    {
        ((void (*)(void *, int))gp(CALLER_RACE_END))(mpm, 3);
        CHECK(st->calls[D_SEND_LOCAL_RACE_END] == 1 && st->last_position == 3,
              "SendLocalRaceEnd(position=3)");
        CHECK(st->last_from_leave == 0,
              "unbekanntes LR %08x wird als Ziellinie behandelt (from_leave=%d)",
              (unsigned)(CALLER_RACE_END + 8), (int)st->last_from_leave);
    }

    printf("\n== MPM_Clear51c (12-Byte-Stub, aber kein sret) ==\n");
    {
        ((void (*)(void *))gp(0x4a0a8528u))(mpm);
        CHECK(st->calls[D_CLEAR_51C] == 1 && st->last_mpm == mpm_guest,
              "mp_clear_51c gerufen, mpm=%08x", (unsigned)st->last_mpm);
    }

    printf("\n== bl-Hooks ==\n");
    {
        void *flow = malloc(0x900);
        ((void (*)(void *, void *))gp(WRAP_SCREEN2))(mpm, flow);
        CHECK(st->hooks[H_SCREEN2_ENTER] == 1 && st->last_mpm == mpm_guest &&
                  st->last_flow == (uint32_t)(uintptr_t)flow,
              "mp_screen2_enter(mpm=%08x, flow=%08x ueber r4/r1-Patch)",
              (unsigned)st->last_mpm, (unsigned)st->last_flow);
        CHECK(g_selinit_calls == 1 && g_seladd_calls == 1 &&
                  g_sel_obj == (void *)((unsigned char *)flow + 0x680) &&
                  g_sel_args[0] == 0xaa && g_sel_args[1] == 0x78 &&
                  g_sel_args[2] == MP_TEXT_HOST_JOIN,
              "call_guest: Selektor %p, Argumente %02x/%02x/%03x", g_sel_obj,
              (unsigned)g_sel_args[0], (unsigned)g_sel_args[1],
              (unsigned)g_sel_args[2]);
        CHECK(st->call_guest_ret == 0x5e1ec704u,
              "call_guest liefert r0 zurueck: %08x", (unsigned)st->call_guest_ret);
    }
    {
        ((void (*)(void *, int32_t, int32_t, int32_t, int32_t))gp(WRAP_STATE45))(
            mpm, 1, 4242, 77, 88);
        CHECK(st->hooks[H_STATE4TO5] == 1 && st->last_slot == 1 &&
                  st->last_score == 4242 && st->last_stat_a == 77 &&
                  st->last_stat_b == 88,
              "mp_on_state4to5(slot=%d score=%d a=%d b=%d)", (int)st->last_slot,
              (int)st->last_score, (int)st->last_stat_a, (int)st->last_stat_b);
        CHECK(g_s45_calls == 1 && g_s45_mpm == mpm && g_s45_args[0] == 1 &&
                  g_s45_args[1] == 4242 && g_s45_args[2] == 77 && g_s45_args[3] == 88,
              "Originalfunktion danach mit denselben 5 Argumenten gerufen");
    }
    {
        int v = ((int (*)(void *))gp(WRAP_COUNTDN))(mpm);
        CHECK(st->hooks[H_COUNTDOWN_GATE] == 1, "mp_on_countdown_gate gerufen");
        CHECK(g_cd_calls == 1 && v == 1,
              "Original UpdateCountdown gerufen, Rueckgabe %d durchgereicht", v);
    }

    printf("\n== Trampolin-Hooks ==\n");
    {
        uint32_t pumps = st->pump_calls;
        void (*yield)(uint32_t) = (void (*)(uint32_t))(g_tramp + EXP_YIELD * TRAMPSZ);
        int yc = g_yield_calls;
        yield(7);
        CHECK(st->pump_calls == pumps + 1, "s3eDeviceYield-Hook pumpt (%u -> %u)",
              (unsigned)pumps, (unsigned)st->pump_calls);
        CHECK(st->pump_mpm == mpm_guest,
              "mp_pump bekommt den Singleton aus 0x4a1c2050: %08x",
              (unsigned)st->pump_mpm);
        CHECK(g_yield_calls == yc + 1 && g_yield_ms == 7,
              "Originalfunktion mit ms=%u weitergerufen", (unsigned)g_yield_ms);
    }
    {
        int64_t (*utc)(void) = (int64_t(*)(void))(g_tramp + EXP_UTC * TRAMPSZ);
        int64_t t0 = utc();
        int64_t t1;
        CHECK(t0 == FAKE_UTC_BASE, "s3eTimerGetUTC vor der Sync = %lld", (long long)t0);
        ((void (*)(void *, int))gp(0x4a0a7ef8u))(mpm, 1); /* ConnectToHost */
        t1 = utc();
        CHECK(t1 == FAKE_UTC_BASE + DUMMY_CLOCK_OFFSET,
              "nach set_clock_offset(%d): %lld (erwartet %lld)", DUMMY_CLOCK_OFFSET,
              (long long)t1, (long long)(FAKE_UTC_BASE + DUMMY_CLOCK_OFFSET));
        CHECK(st->utc_ms == FAKE_UTC_BASE,
              "plattform utc_ms bleibt roh: %lld", (long long)st->utc_ms);
        CHECK(st->mono_ms > 0, "plattform mono_ms = %lld", (long long)st->mono_ms);
    }

    printf("\n== Alle Stubs erreichbar ==\n");
    {
        size_t k;
        uint32_t before = st->total;
        for (k = 0; k < mp_stub_count; k++) {
            uint32_t a = mp_stubs[k].addr;
            /* nur diese vier sind sret; 0x4a0a8528 ist trotz 12 Byte
               ein gewoehnliches void f(MPM*) */
            if (a == 0x4a0a7e8cu || a == 0x4a0a7e78u || a == 0x4a0a7c64u ||
                a == 0x4a0a806cu) {
                uint32_t slot = 0;
                ((void *(*)(void *, void *, int))gp(a))(&slot, mpm, 0);
            } else {
                ((void (*)(void *, int))gp(a))(mpm, 0);
            }
        }
        CHECK(st->total == before + (uint32_t)mp_stub_count,
              "%u Stubs landen im Kern (%u Aufrufe gezaehlt)",
              (unsigned)mp_stub_count, (unsigned)(st->total - before));
    }

    printf("\n%s (%d Fehler)\n",
           g_fail ? "FEHLGESCHLAGEN" : "ALLE TESTS BESTANDEN", g_fail);
    return g_fail ? 1 : 0;
}
