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

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

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

void dump_low_mappings() {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo = strtoul(line, nullptr, 16);
        if (lo < 0x100000000ul) fprintf(stderr, "  %s", line);
    }
    fclose(f);
}

namespace {
constexpr uint64_t kLowLimit = 0x100000000ull;  // guest addresses are 32 bit
// Every mapping below 4 GB as [begin, end), sorted.
std::vector<std::pair<uint64_t, uint64_t>> low_mappings() {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *dash = nullptr;
        uint64_t lo = strtoull(line, &dash, 16);
        if (!dash || *dash != '-') continue;
        uint64_t hi = strtoull(dash + 1, nullptr, 16);
        if (hi <= lo || lo >= kLowLimit) continue;
        out.push_back({lo, std::min(hi, kLowLimit)});
    }
    fclose(f);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

addr_t find_free_span(addr_t preferred, size_t want, size_t least, size_t *got) {
    // The lowest 16 MB stay untouched: mmap_min_addr, the ELF of the process
    // itself and its brk live down there, and nothing is gained by squeezing in.
    constexpr uint64_t kFloor = 0x01000000ull;
    constexpr uint64_t kAlign = 0x100000ull;  // 1 MB, the step the heaps use
    *got = 0;

    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    uint64_t at = kFloor;
    for (const auto &m : low_mappings()) {
        if (m.second <= at) continue;
        if (m.first > at) gaps.push_back({at, m.first});
        at = m.second;
    }
    if (at < kLowLimit) gaps.push_back({at, kLowLimit});

    // The wish first -- on Sailfish and the N9 it is simply free, and then the
    // layout stays the one that has been tested there.
    for (const auto &g : gaps)
        if (preferred >= g.first && static_cast<uint64_t>(preferred) + want <= g.second) {
            *got = want;
            return preferred;
        }

    uint64_t best_base = 0, best_size = 0;
    for (const auto &g : gaps) {
        uint64_t b = (g.first + kAlign - 1) & ~(kAlign - 1);
        if (b >= g.second) continue;
        if (g.second - b > best_size) {
            best_size = g.second - b;
            best_base = b;
        }
    }
    if (best_size < least) return 0;
    *got = static_cast<size_t>(std::min<uint64_t>(want, best_size));
    return static_cast<addr_t>(best_base);
}

bool map_fixed(addr_t start, size_t size) {
    const size_t hp = host_page();
    uint64_t begin = start & ~(uint64_t)(hp - 1);
    uint64_t end = (static_cast<uint64_t>(start) + size + hp - 1) & ~(uint64_t)(hp - 1);
    void *p = mmap(gptr(static_cast<addr_t>(begin)), end - begin, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if ((p == MAP_FAILED && errno == EINVAL) || (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) != begin)) {
        // MAP_FIXED_NOREPLACE needs Linux 4.17; older kernels ignore the flag
        // and hand back some other address. Check the range ourselves instead.
        if (p != MAP_FAILED) munmap(p, end - begin);
        bool clear = true;
        for (uint64_t a = begin; a < end && clear; a += hp)
            if (msync(gptr(static_cast<addr_t>(a)), hp, MS_ASYNC) == 0) clear = false;
        if (clear)
            p = mmap(gptr(static_cast<addr_t>(begin)), end - begin, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    if (p == MAP_FAILED || reinterpret_cast<uintptr_t>(p) != begin) {
        if (p != MAP_FAILED) munmap(p, end - begin);
        logf("[guest] mmap %#llx-%#llx failed, low mappings:", (unsigned long long)begin, (unsigned long long)end);
        dump_low_mappings();
        return false;
    }
    for (uint64_t a = begin; a < end; a += PAGE) g_page_map[a >> 12] = 1;
    return true;
}

bool is_mapped(addr_t addr) { return g_page_map[addr >> 12] != 0; }

static void ensure_arena() {
    if (g_arena) return;
    if (!map_fixed(layout::ARENA_BASE, layout::ARENA_SIZE))
        fatal("cannot map guest arena at %#x", layout::ARENA_BASE);
    g_arena = create_mspace_with_base(gptr(layout::ARENA_BASE), layout::ARENA_SIZE, 0);
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
