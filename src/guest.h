// Guest (32-bit ARM) address space helpers.
//
// The guest's 4 GB address space is one private host reservation, made once at
// startup by guest::init_address_space(): guest address a lives at host address
// guest::g_base + a. Inside that window there is nothing but our own mappings,
// so the fixed layout below always fits.
//
// On a 32-bit ARM host (GUEST_NATIVE) the guest IS the process: g_base is 0,
// the game code runs natively, and init_address_space() reserves the free
// gaps of the low address space instead so that nothing else moves in later.
//
// Until 1.0.4 the guest was identity mapped into the low 4 GB of the host
// process instead. That meant sharing those 4 GB with whatever the process
// already had there, and an Android process has plenty: ART's heaps sat in the
// heap window, a Huawei phone had something at the arena, and a Sony Xperia 10 V
// at 0x4a000000 -- the game image's link address, which cannot move.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using addr_t = uint32_t;

// Fixed layout of the guest address space.
namespace layout {
constexpr addr_t ARENA_BASE = 0x08000000;  // loader-owned objects visible to the guest
constexpr size_t ARENA_SIZE = 0x08000000;
// The heap window is a wish, not a law: in an Android process ART already
// owns parts of it, so hle_memory.cpp asks find_free_span() what is free.
constexpr addr_t HEAP_BASE = 0x10000000;   // s3e heaps (MemSize0..)
constexpr size_t HEAP_SPAN = 0x30000000;
constexpr addr_t STACK_BASE = 0x60000000;  // per-thread guest stacks
constexpr size_t STACK_SIZE = 0x00400000;
}  // namespace layout

[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

namespace guest {
extern uintptr_t g_base;  // host address of guest address 0
}

inline void *gptr(addr_t a) { return reinterpret_cast<void *>(guest::g_base + a); }
template <class T> inline T *gptr_t(addr_t a) { return reinterpret_cast<T *>(guest::g_base + a); }

// Host pointer -> guest address. nullptr stays 0: host code hands NULL to the
// guest all the time, and with a base it would otherwise turn into -g_base.
inline addr_t gaddr(const void *p) {
    if (!p) return 0;
    uintptr_t v = reinterpret_cast<uintptr_t>(p) - guest::g_base;
    if constexpr (sizeof(uintptr_t) > 4) {
        if (v >> 32) fatal("host pointer %p is not guest addressable", p);
    }
    return static_cast<addr_t>(v);
}

namespace guest {
// Reserve the guest's 4 GB in the host address space. Must run before the
// first map_fixed(); a second call does nothing.
void init_address_space();
// Is this host pointer inside the guest window?
bool in_guest_space(const void *p);
// Map [start, start+size) read/write at exactly that guest address. Fails when
// part of it is already mapped -- by us, since nothing else lives there.
bool map_fixed(addr_t start, size_t size);
// Pick a guest window of `want` bytes that none of our mappings occupies:
// `preferred` when that is free, otherwise the smallest gap that holds all of
// `want`, otherwise the largest one (1 MB aligned). *got is the usable size, at
// most `want`; the return value is 0 when not even `least` bytes are free.
addr_t find_free_span(addr_t preferred, size_t want, size_t least, size_t *got);
// Print the guest's mapped ranges (diagnostic for map_fixed failures).
void dump_low_mappings();
bool is_mapped(addr_t addr);

// Allocator for loader-owned guest-visible memory (strings, handles, ...).
addr_t alloc(size_t size);
void release(addr_t addr);
addr_t strdup(const char *s);

// Stable guest copy of a host string, interned by content.
addr_t intern(const std::string &s);

uint32_t read32(addr_t a);
void write32(addr_t a, uint32_t v);

// After writing code the CPU is going to execute: no-op for the JIT (it reads
// memory when it translates), an instruction cache flush on the native host.
inline void flush_code(addr_t a, size_t size) {
#ifdef GUEST_NATIVE
    __builtin___clear_cache(reinterpret_cast<char *>(g_base + a), reinterpret_cast<char *>(g_base + a + size));
#else
    (void)a;
    (void)size;
#endif
}
}  // namespace guest
