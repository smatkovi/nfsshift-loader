#include "guest.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL.h>

#include "dlmalloc.h"


void fatal(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[fatal] %s\n", msg);
    fflush(stderr);
    // On a phone stderr is nowhere to be seen: without this a fatal during
    // startup is indistinguishable from "the app does not start". The box needs
    // the video subsystem; before that it simply fails and we lose nothing.
    // The guard is for a fatal raised from inside SDL itself.
    static bool inside = false;
    if (!inside) {
        inside = true;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NFS Shift", msg, nullptr);
    }
    abort();
}

void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

namespace guest {
namespace {
// The page map is indexed in 4 KB units because that is the guest's page size,
// but mmap has to be aligned to the *host* page size, which is 16 KB on some
// Android devices.
constexpr size_t PAGE = 0x1000;
size_t host_page() {
    static const size_t p = [] {
        long v = sysconf(_SC_PAGESIZE);
        return v > 0 ? static_cast<size_t>(v) : PAGE;
    }();
    return p;
}
std::vector<uint8_t> g_page_map(1u << 20);
std::mutex g_arena_mutex;
mspace g_arena;
std::unordered_map<std::string, addr_t> g_interned;
}  // namespace

uintptr_t g_base;
namespace {
bool g_ready;
constexpr uint8_t OURS = 1;      // page mapped by map_fixed()
constexpr uint8_t FOREIGN = 2;   // native host: page the process had before we started
constexpr uint8_t RESERVED = 3;  // native host: PROT_NONE reservation of ours, free to map
inline bool page_used(uint64_t pg) { return g_page_map[pg] != 0 && g_page_map[pg] != RESERVED; }
}  // namespace

#ifdef GUEST_NATIVE
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
// 32-bit ARM host: guest address == host address, the game code runs natively.
// Mark what the process already occupies as foreign, then reserve the free
// gaps of the guest window (PROT_NONE, address space only) so that drivers
// loaded later and malloc arenas do not move in where the heaps, the stacks
// and the game image (fixed at 0x4a000000) are going to be.
void init_address_space() {
    if (g_ready) return;
    g_ready = true;
    g_base = 0;
    constexpr uint64_t kLimit = 0x100000000ull;
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) fatal("cannot read /proc/self/maps: %s", strerror(errno));
    char line[512];
    while (fgets(line, sizeof line, m)) {
        unsigned long long s = 0, e = 0;
        if (sscanf(line, "%llx-%llx", &s, &e) != 2) continue;
        for (uint64_t a = s & ~static_cast<uint64_t>(PAGE - 1); a < e && a < kLimit; a += PAGE)
            g_page_map[a >> 12] = FOREIGN;
    }
    fclose(m);

    constexpr uint64_t kLow = 0x01000000, kHigh = 0x70000000;
    uint64_t pg = kLow >> 12;
    const uint64_t end = kHigh >> 12;
    size_t reserved = 0;
    while (pg < end) {
        while (pg < end && g_page_map[pg]) ++pg;
        uint64_t gap = pg;
        while (pg < end && !g_page_map[pg]) ++pg;
        if (pg == gap) continue;
        void *want = reinterpret_cast<void *>(static_cast<uintptr_t>(gap << 12));
        size_t size = static_cast<size_t>((pg - gap) << 12);
        void *p = mmap(want, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED || p != want) {
            // An old kernel (or qemu-user) ignores NOREPLACE and hands out
            // something else. The gap stays unreserved: map_fixed() then maps
            // it with NOREPLACE page by page and checks what it got.
            if (p != MAP_FAILED) munmap(p, size);
            logf("[guest] could not reserve %#llx-%#llx: %s", (unsigned long long)(gap << 12),
                 (unsigned long long)(pg << 12), p == MAP_FAILED ? strerror(errno) : "placed elsewhere");
            continue;
        }
        for (uint64_t a = gap; a < pg; ++a) g_page_map[a] = RESERVED;
        reserved += size;
    }
    logf("[guest] native host: guest addresses are host addresses, %zu MB reserved", reserved >> 20);
}
#else
void init_address_space() {
    if (g_ready) return;
    g_ready = true;
    // 4 GB for the guest and a guard behind them: a read of up to 8 bytes at
    // 0xfffffffc must fault inside our own reservation, not in a neighbour's
    // page. PROT_NONE with MAP_NORESERVE costs address space, not memory.
    constexpr size_t kSpan = (static_cast<size_t>(1) << 32) + 0x10000;
    void *p = mmap(nullptr, kSpan, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve 4 GB of address space for the guest: %s", strerror(errno));
    g_base = reinterpret_cast<uintptr_t>(p);
    logf("[guest] address space at %p", p);
}

#endif

bool in_guest_space(const void *p) {
#ifdef GUEST_NATIVE
    return g_ready && p;
#else
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return g_base && v >= g_base && v - g_base < (static_cast<uint64_t>(1) << 32);
#endif
}

void dump_low_mappings() {
    // The guest window holds only our own mappings, so the page map is the whole
    // truth; print it as merged ranges.
    uint64_t start = 0;
    bool in = false;
    for (uint64_t pg = 0; pg <= g_page_map.size(); ++pg) {
        bool m = pg < g_page_map.size() && g_page_map[pg];
        if (m && !in) {
            start = pg;
            in = true;
        } else if (!m && in) {
            fprintf(stderr, "  %08llx-%08llx\n", (unsigned long long)(start << 12), (unsigned long long)(pg << 12));
            in = false;
        }
    }
}

addr_t find_free_span(addr_t preferred, size_t want, size_t least, size_t *got) {
    // The lowest 16 MB stay untouched, as in the old identity-mapped layout; the
    // guest never expected anything down there.
    constexpr uint64_t kFloor = 0x01000000ull;
    constexpr uint64_t kLimit = 0x100000000ull;
    constexpr uint64_t kAlign = 0x100000ull;  // 1 MB, the step the heaps use
    *got = 0;

    auto range_free = [](uint64_t begin, uint64_t end) {
        for (uint64_t a = begin & ~(PAGE - 1); a < end; a += PAGE)
            if (page_used(a >> 12)) return false;
        return true;
    };
    if (preferred >= kFloor && static_cast<uint64_t>(preferred) + want <= kLimit &&
        range_free(preferred, static_cast<uint64_t>(preferred) + want)) {
        *got = want;
        return preferred;
    }

    // Gaps between our mappings, then smallest-that-fits / largest.
    uint64_t fit_base = 0, fit_size = ~0ull, best_base = 0, best_size = 0;
    uint64_t pg = kFloor >> 12;
    constexpr uint64_t kPages = kLimit >> 12;
    while (pg < kPages) {
        while (pg < kPages && page_used(pg)) ++pg;
        uint64_t gap_begin = pg << 12;
        while (pg < kPages && !page_used(pg)) ++pg;
        uint64_t gap_end = pg << 12;
        uint64_t b = (gap_begin + kAlign - 1) & ~(kAlign - 1);
        if (b >= gap_end) continue;
        uint64_t size = gap_end - b;
        if (size >= want && size < fit_size) {
            fit_size = size;
            fit_base = b;
        }
        if (size > best_size) {
            best_size = size;
            best_base = b;
        }
    }
    if (fit_base) {
        *got = want;
        return static_cast<addr_t>(fit_base);
    }
    if (best_size < least) return 0;
    *got = static_cast<size_t>(std::min<uint64_t>(want, best_size));
    return static_cast<addr_t>(best_base);
}

bool map_fixed(addr_t start, size_t size) {
    if (!g_ready) fatal("map_fixed before init_address_space");
    if (!size) return true;
    const size_t hp = host_page();
    uint64_t end_req = static_cast<uint64_t>(start) + size;
    if (end_req > (static_cast<uint64_t>(1) << 32)) return false;
    // Refuse what is already ours: callers use the failure to find a free spot.
    for (uint64_t a = start & ~static_cast<uint64_t>(PAGE - 1); a < end_req; a += PAGE)
        if (page_used(a >> 12)) {
            logf("[guest] %#llx-%#llx is already mapped, guest mappings:", (unsigned long long)start,
                 (unsigned long long)end_req);
            dump_low_mappings();
            return false;
        }
    // mmap works in host pages, which are 16 KB on some Android devices. A host
    // page that already carries another of our regions is RW already: skip it
    // instead of mapping over (and zeroing) its contents.
    uint64_t begin = start & ~static_cast<uint64_t>(hp - 1);
    uint64_t end = (end_req + hp - 1) & ~static_cast<uint64_t>(hp - 1);
    for (uint64_t h = begin; h < end; h += hp) {
        bool used = false, reserved = false;
        for (uint64_t a = h; a < h + hp && !used; a += PAGE) {
            if (a >= (static_cast<uint64_t>(1) << 32)) break;
            if (page_used(a >> 12)) used = true;
            if (g_page_map[a >> 12] == RESERVED) reserved = true;
        }
        if (used) continue;
        void *want = reinterpret_cast<void *>(g_base + h);
#ifdef GUEST_NATIVE
        // The game image, the stubs and the patches are code the CPU executes.
        // Inside our own reservation MAP_FIXED is safe; elsewhere the page must
        // not replace anything the process owns.
        const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        const int fixed = reserved ? MAP_FIXED : MAP_FIXED_NOREPLACE;
#else
        (void)reserved;
        const int prot = PROT_READ | PROT_WRITE;
        const int fixed = MAP_FIXED;
#endif
        void *p = mmap(want, hp, prot, MAP_PRIVATE | MAP_ANONYMOUS | fixed, -1, 0);
        if (p != MAP_FAILED && p != want) {
            munmap(p, hp);  // NOREPLACE unsupported and the address was taken
            errno = EEXIST;
            p = MAP_FAILED;
        }
        if (p == MAP_FAILED) {
            logf("[guest] mmap of guest page %#llx failed: %s", (unsigned long long)h, strerror(errno));
            return false;
        }
    }
    for (uint64_t a = begin; a < end && a < (static_cast<uint64_t>(1) << 32); a += PAGE) g_page_map[a >> 12] = OURS;
    return true;
}

bool is_mapped(addr_t addr) { return g_page_map[addr >> 12] != 0; }

// Like the heaps, the arena only wishes for ARENA_BASE: on a Huawei phone that
// address was taken and the loader died in "cannot map guest arena at
// 0x8000000". Nothing in the guest cares where the arena is -- it only sees the
// pointers it is handed -- so it goes wherever there is room. The s3e image is
// mapped before the first allocation from here, so the arena cannot take its
// place.
static void ensure_arena() {
    if (g_arena) return;
    constexpr size_t kArenaLeast = 32u << 20;
    size_t size = 0;
    addr_t base = find_free_span(layout::ARENA_BASE, layout::ARENA_SIZE, kArenaLeast, &size);
    if (!base || !map_fixed(base, size)) {
        logf("[guest] no room for the arena, mappings below 4 GB:");
        dump_low_mappings();
        fatal("cannot map guest arena (wanted %#x)", layout::ARENA_BASE);
    }
    if (base != layout::ARENA_BASE || size != layout::ARENA_SIZE)
        logf("[guest] arena moved to %#x + %zu MB (wanted %#x + %zu MB)", base, size >> 20, layout::ARENA_BASE,
             layout::ARENA_SIZE >> 20);
    g_arena = create_mspace_with_base(gptr(base), size, 0);
}

addr_t alloc(size_t size) {
    std::lock_guard<std::mutex> lock(g_arena_mutex);
    ensure_arena();
    void *p = mspace_memalign(g_arena, 16, size ? size : 1);
    if (!p) fatal("guest arena exhausted (%zu bytes)", size);
    memset(p, 0, size);
    return gaddr(p);
}

void release(addr_t addr) {
    if (!addr) return;
    std::lock_guard<std::mutex> lock(g_arena_mutex);
    mspace_free(g_arena, gptr(addr));
}

addr_t strdup(const char *s) {
    size_t n = strlen(s) + 1;
    addr_t a = alloc(n);
    memcpy(gptr(a), s, n);
    return a;
}

addr_t intern(const std::string &s) {
    {
        std::lock_guard<std::mutex> lock(g_arena_mutex);
        auto it = g_interned.find(s);
        if (it != g_interned.end()) return it->second;
    }
    addr_t a = strdup(s.c_str());
    std::lock_guard<std::mutex> lock(g_arena_mutex);
    return g_interned.emplace(s, a).first->second;
}

uint32_t read32(addr_t a) {
    uint32_t v;
    memcpy(&v, gptr(a), 4);
    return v;
}

void write32(addr_t a, uint32_t v) { memcpy(gptr(a), &v, 4); }
}  // namespace guest
