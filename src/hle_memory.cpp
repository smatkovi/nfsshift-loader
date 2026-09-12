// s3eMemory: the game's allocator (IwCRT malloc) sits on top of s3eMallocBase.
// Each configured heap gets its own dlmalloc space inside the guest range.
#include <algorithm>
#include <cstring>
#include <mutex>

#include "config.h"
#include "dlmalloc.h"
#include "hle.h"
#include "memory.h"
#include "runtime.h"

namespace {
using namespace runtime;

constexpr int kHeaps = 8;
constexpr uint32_t FLAG_OS_DIRECT = 8;
constexpr uint32_t FLAG_INACTIVE = 4;

struct Heap {
    uint32_t size = 0;      // configured size reported to the game
    uint32_t flags = 0;
    addr_t base = 0;
    size_t capacity = 0;    // actually reserved
    mspace space = nullptr;
    int64_t used = 0;
};

std::recursive_mutex g_mutex;
Heap g_heaps[kHeaps];
int32_t g_current;
// Where the heaps really end up: layout::HEAP_BASE is only the wish, an
// Android process has ART sitting in the middle of that window (see
// place_heaps_window()).
addr_t g_heap_base = layout::HEAP_BASE;
size_t g_heap_span = layout::HEAP_SPAN;
addr_t g_next_base = layout::HEAP_BASE;
// Heap 6 alone reserves 256 MB (OS_DIRECT), the configured ones came to 54 MB
// in the MeeGo build: below this a start is not worth trying.
constexpr size_t kHeapLeast = 320u << 20;
bool g_initialized;
uint32_t g_user_mgr[3];  // guest {malloc, realloc, free}

uint32_t parse_flags(const std::string &s) {
    uint32_t f = 0;
    if (s.find("NO_MSG_ON_FAIL") != std::string::npos) f |= 1;
    if (s.find("USE_STACK_ALLOCATOR") != std::string::npos) f |= 2;
    if (s.find("INACTIVE") != std::string::npos) f |= FLAG_INACTIVE;
    if (s.find("OS_DIRECT") != std::string::npos) f |= FLAG_OS_DIRECT;
    return f;
}

bool create(int i) {
    Heap &h = g_heaps[i];
    if (h.space) return false;
    // Reserve generously: the GLES2 renderer needs more than the N9 did.
    size_t want = (h.flags & FLAG_OS_DIRECT) ? 256u << 20 : std::max<size_t>(size_t(h.size) * 4, 1u << 20);
    want = (want + 0xfffff) & ~size_t(0xfffff);
    if (static_cast<uint64_t>(g_next_base) + want > static_cast<uint64_t>(g_heap_base) + g_heap_span)
        fatal("guest heap span exhausted");
    if (!guest::map_fixed(g_next_base, want)) fatal("cannot map heap %d", i);
    h.base = g_next_base;
    h.capacity = want;
    h.space = create_mspace_with_base(gptr(h.base), want, 0);
    g_next_base += want + 0x100000;
    logf("[mem] heap %d: %u bytes configured, %zu MB reserved at %#x", i, h.size, want >> 20, h.base);
    return true;
}

// The heaps sit at a fixed address in the guest's low 4 GB. On Sailfish and on
// the N9 that address is free; in an Android process it is not -- ART puts the
// Java heap at 0x14000000 and its large object space around 0x3fb00000, both
// inside the window, so the first heap past 0x13600000 died in map_fixed()
// ("cannot map heap 6") and the app went down before the first frame. Ask the
// kernel what is actually free instead of insisting on the wish.
void place_heaps_window() {
    size_t got = 0;
    addr_t base = guest::find_free_span(layout::HEAP_BASE, layout::HEAP_SPAN, kHeapLeast, &got);
    if (!base) {
        logf("[mem] no free window of %zu MB below 4 GB, mappings:", kHeapLeast >> 20);
        guest::dump_low_mappings();
        fatal("no room for the guest heaps below 4 GB");
    }
    g_heap_base = base;
    g_heap_span = got;
    g_next_base = base;
    if (base != layout::HEAP_BASE || got != layout::HEAP_SPAN)
        logf("[mem] heap window moved to %#x + %zu MB (wanted %#x + %zu MB)", base, got >> 20,
             layout::HEAP_BASE, layout::HEAP_SPAN >> 20);
}

void init_heaps() {
    if (g_initialized) return;
    g_initialized = true;
    place_heaps_window();
    for (int i = 0; i < kHeaps; ++i) {
        Heap &h = g_heaps[i];
        int size = config::get_int("s3e", "MemSize" + std::to_string(i), -1);
        if (i == 0 && size <= 0) size = config::get_int("s3e", "MemSize", 0x300000);
        if (i == 7 && size <= 0) size = 0x1400;
        h.size = size > 0 ? (static_cast<uint32_t>(size) + 3) & ~3u : 0;
        h.flags = parse_flags(config::get("s3e", "MemFlags" + std::to_string(i)).value_or(""));
        if ((h.size > 0 || (h.flags & FLAG_OS_DIRECT)) && !(h.flags & FLAG_INACTIVE)) create(i);
    }
    if (!g_heaps[6].space) {
        g_heaps[6].flags = FLAG_OS_DIRECT;
        create(6);
    }
}

Heap *heap_of(addr_t p) {
    for (Heap &h : g_heaps)
        if (h.space && p >= h.base && p < h.base + h.capacity) return &h;
    return nullptr;
}

addr_t heap_alloc(Heap &h, uint32_t size) {
    void *p = mspace_malloc(h.space, size ? size : 1);
    if (!p) return 0;
    h.used += mspace_usable_size(p);
    return gaddr(p);
}

addr_t s3eMallocBase(int32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    init_heaps();
    Heap &h = g_heaps[g_current];
    if (!h.space) {
        set_error(DEV_MEMORY, 1002);
        logf("[mem] malloc on heap %d which is not created", g_current);
        return 0;
    }
    addr_t a = heap_alloc(h, static_cast<uint32_t>(size));
    if (!a) {
        set_error(DEV_MEMORY, 1001);
        logf("[mem] heap %d out of memory allocating %d bytes", g_current, size);
    }
    return a;
}
HLE_REGISTER(s3eMallocBase);

void s3eFreeBase(addr_t ptr) {
    if (!ptr) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Heap *h = heap_of(ptr);
    if (!h) {
        logf("[mem] free of unknown pointer %#x ignored", ptr);
        return;
    }
    h->used -= mspace_usable_size(gptr(ptr));
    mspace_free(h->space, gptr(ptr));
}
HLE_REGISTER(s3eFreeBase);

addr_t s3eReallocBase(addr_t ptr, int32_t size) {
    if (!ptr) return s3eMallocBase(size);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Heap *h = heap_of(ptr);
    if (!h) {
        set_error(DEV_MEMORY, 1000);
        logf("[mem] realloc of unknown pointer %#x", ptr);
        return 0;
    }
    size_t old = mspace_usable_size(gptr(ptr));
    void *p = mspace_realloc(h->space, gptr(ptr), size > 0 ? size : 1);
    if (!p) {
        set_error(DEV_MEMORY, 1001);
        logf("[mem] realloc to %d bytes failed", size);
        return 0;
    }
    h->used += static_cast<int64_t>(mspace_usable_size(p)) - static_cast<int64_t>(old);
    return gaddr(p);
}
HLE_REGISTER(s3eReallocBase);

int32_t s3eMemoryGetInt(int32_t prop) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    init_heaps();
    Heap &h = g_heaps[g_current];
    int64_t free_bytes = 0;
    if (h.space && !(h.flags & FLAG_OS_DIRECT)) free_bytes = std::max<int64_t>(int64_t(h.size) - h.used, 0);
    switch (prop) {
    case 0: return g_current;
    case 1: return static_cast<int32_t>(h.size);
    case 2: return static_cast<int32_t>(free_bytes);
    case 3: return static_cast<int32_t>(int64_t(h.size) - free_bytes);
    case 4: return static_cast<int32_t>(free_bytes);
    case 5: return static_cast<int32_t>(h.flags);
    default: set_error(DEV_MEMORY, 1); return -1;
    }
}
HLE_REGISTER(s3eMemoryGetInt);

int32_t s3eMemorySetInt(int32_t prop, int32_t value) {
    if (prop != 0 || static_cast<uint32_t>(value) >= kHeaps) {
        set_error(DEV_MEMORY, 1);
        return S3E_RESULT_ERROR;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_current = value;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eMemorySetInt);

int32_t s3eMemoryHeapCreate(uint32_t index) {
    if (index >= kHeaps) {
        set_error(DEV_MEMORY, 1);
        return S3E_RESULT_ERROR;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    init_heaps();
    if (g_heaps[index].space) {
        set_error(DEV_MEMORY, 1002);
        return S3E_RESULT_ERROR;
    }
    g_heaps[index].flags &= ~FLAG_INACTIVE;
    create(static_cast<int>(index));
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eMemoryHeapCreate);

int32_t s3eMemorySetUserMemMgr(const uint32_t *mgr) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!mgr) {
        g_user_mgr[0] = hle::stub_for("s3eMallocBase");
        g_user_mgr[1] = hle::stub_for("s3eReallocBase");
        g_user_mgr[2] = hle::stub_for("s3eFreeBase");
        return S3E_RESULT_SUCCESS;
    }
    if (!mgr[0] || !mgr[1] || !mgr[2]) {
        set_error(DEV_MEMORY, 1);
        return S3E_RESULT_ERROR;
    }
    memcpy(g_user_mgr, mgr, sizeof(g_user_mgr));
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eMemorySetUserMemMgr);
}  // namespace

namespace memory {
addr_t user_realloc(addr_t ptr, uint32_t size) {
    if (!g_user_mgr[1]) return s3eReallocBase(ptr, static_cast<int32_t>(size));
    return Cpu::current().call(g_user_mgr[1], {ptr, size});
}

void user_free(addr_t ptr) {
    if (!ptr) return;
    if (!g_user_mgr[2]) return s3eFreeBase(ptr);
    Cpu::current().call(g_user_mgr[2], {ptr});
}
}  // namespace memory
