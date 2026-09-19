// Guest CPU: runs the game's 32-bit ARM code and bridges host<->guest calls.
//
// Two backends share this interface:
//   cpu.cpp         a dynarmic A32 JIT (64-bit hosts: aarch64, x86_64)
//   cpu_native.cpp  direct execution (32-bit ARM hosts, GUEST_NATIVE): the
//                   game code is ARM code, so the CPU runs it as it is. Only
//                   the calling convention needs a bridge -- the game is
//                   soft-float (armel), the host hard-float -- and that is
//                   what the HLE stubs and Cpu::call do for both backends.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "guest.h"

#ifndef GUEST_NATIVE
namespace Dynarmic::A32 {
class Jit;
}
#endif

class Cpu;
using HleFn = void (*)(Cpu &);

class Cpu {
public:
    explicit Cpu(const char *name);
    ~Cpu();

    uint32_t &reg(int i);
    uint32_t cpsr() const;
    void set_cpsr(uint32_t v);

    // Call guest function `fn` (bit 0 selects Thumb) and return r0 (r1 is
    // available via reg(1) for 64-bit results). Arguments beyond the first
    // four are passed on the guest stack, so structs by value work too.
    uint32_t call(addr_t fn, const uint32_t *args, size_t nargs);
    uint32_t call(addr_t fn, std::initializer_list<uint32_t> args = {}) {
        return call(fn, args.begin(), args.size());
    }

    // Currently executing CPU of this host thread.
    static Cpu &current();

    const char *name() const { return name_; }

#ifdef GUEST_NATIVE
    // Used by the native HLE entry: `regs` holds r0-r3, sp and lr of the guest
    // call that just arrived; reg() reads and writes that block until leave.
    uint32_t *native_enter(uint32_t *regs) {
        uint32_t *prev = frame_;
        frame_ = regs;
        return prev;
    }
    void native_leave(uint32_t *prev) { frame_ = prev; }
#else
    class Callbacks;
#endif

private:
    const char *name_;
    int depth_ = 0;
#ifdef GUEST_NATIVE
    uint32_t *frame_;
    uint32_t top_regs_[16] = {};
#else
    void run_until_return();
    std::unique_ptr<Callbacks> callbacks_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    addr_t stack_top_;
#endif
};

// Per-thread stack of active CPUs (Cpu::current() is its top). Shared by both
// backends, implemented in cpu_common.cpp.
namespace cpu_thread {
void push(Cpu *cpu);
void pop();
Cpu *top();
}  // namespace cpu_thread

namespace hle {
constexpr addr_t STUB_BASE = 0x4a400000;
constexpr uint32_t RETURN_SVC = 0xfffff;
constexpr addr_t RETURN_ADDR = STUB_BASE - 8;
// Bytes per import stub: "svc #idx; bx lr" for the JIT, a 16-byte trampoline
// into hle_native_entry for the native backend.
#ifdef GUEST_NATIVE
constexpr size_t STUB_STRIDE = 16;
#else
constexpr size_t STUB_STRIDE = 8;
#endif
constexpr size_t MAX_STUBS = 0x1ff0;

// Backend hooks (cpu.cpp / cpu_native.cpp), used by hle_stubs.cpp.
void backend_init_stub_page();
void write_stub(addr_t at, uint32_t idx);

void register_fn(const char *name, HleFn fn);
HleFn lookup(const char *name);
// Map the stub page and return the guest address of the stub for `name`.
addr_t stub_for(const char *name);
void map_stub_page();
void dispatch(Cpu &cpu, uint32_t svc);
const char *name_of(uint32_t svc);
void set_trace(bool on);
// Called after every HLE call when set (debugging aid).
extern void (*post_call_hook)(Cpu &cpu, uint32_t svc);

// Replace the ARM function at `site` with a branch to the HLE stub `name`.
// `expect` is the original first instruction, checked before patching.
bool patch_arm_function(addr_t site, uint32_t expect, const char *name);
// Overwrite guest code bytes after checking the original content.
bool patch_bytes(addr_t site, const uint8_t *expect, const uint8_t *replacement, size_t size);
}  // namespace hle
