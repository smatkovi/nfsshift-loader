// Guest (32-bit ARM) address space helpers.
//
// Guest memory is identity mapped into the low 4 GB of the host process, so a
// guest address can be used directly as a host pointer and vice versa.
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
constexpr addr_t HEAP_BASE = 0x10000000;   // s3e heaps (MemSize0..)
constexpr size_t HEAP_SPAN = 0x30000000;
constexpr addr_t STACK_BASE = 0x60000000;  // per-thread guest stacks
constexpr size_t STACK_SIZE = 0x00400000;
}  // namespace layout

[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

inline void *gptr(addr_t a) { return reinterpret_cast<void *>(static_cast<uintptr_t>(a)); }
template <class T> inline T *gptr_t(addr_t a) { return reinterpret_cast<T *>(static_cast<uintptr_t>(a)); }

inline addr_t gaddr(const void *p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v >> 32) fatal("host pointer %p is not guest addressable", p);
    return static_cast<addr_t>(v);
}

namespace guest {
// Reserve and map [start, start+size) read/write at exactly that address.
bool map_fixed(addr_t start, size_t size);
// Print every existing mapping below 4 GB (diagnostic for map_fixed failures).
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
}  // namespace guest
