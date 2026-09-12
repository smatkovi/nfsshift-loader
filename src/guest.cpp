#include "guest.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "dlmalloc.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[fatal] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
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
