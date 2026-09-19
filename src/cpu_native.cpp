// Native backend: the host is a 32-bit ARM, so the game's code runs directly.
//
// Guest addresses are host addresses (guest::g_base == 0). An import stub
// (hle::write_stub) is a 16-byte trampoline that loads the import index into
// r12 and jumps to hle_native_entry (cpu_native.S), which spills r0-r3, the
// caller's sp and lr into a 16-word register block and calls
// hle_native_dispatch(). From there on everything is the same code as in the
// JIT build: hle::dispatch() reads arguments through Cpu::reg() and
// ArgCursor, and writes the result to reg(0)/reg(1), which the entry stub
// hands back in r0/r1.
//
// The game is compiled for the soft-float ABI, the host for the hard-float
// one. That never meets: the HLE thunks (hle.h) take floats as raw words from
// the integer registers, and Cpu::call passes raw words as well.
#include "cpu.h"

#include <cstring>

extern "C" {
void hle_native_entry();
uint64_t native_call(uint32_t fn, const uint32_t r[4], const uint32_t *extra, uint32_t nextra);

void hle_native_dispatch(uint32_t *regs, uint32_t idx) {
    Cpu &cpu = Cpu::current();
    uint32_t *prev = cpu.native_enter(regs);
    regs[15] = hle::STUB_BASE + hle::STUB_STRIDE * idx;  // "pc" for diagnostics
    hle::dispatch(cpu, idx);
    cpu.native_leave(prev);
}
}

Cpu::Cpu(const char *name) : name_(name), frame_(top_regs_) {}

Cpu::~Cpu() = default;

uint32_t &Cpu::reg(int i) { return frame_[i]; }
uint32_t Cpu::cpsr() const { return 0x10; }
void Cpu::set_cpsr(uint32_t) {}

uint32_t Cpu::call(addr_t fn, const uint32_t *args, size_t nargs) {
    if (!fn) fatal("[%s] call to null guest function", name_);
    uint32_t r[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < nargs && i < 4; ++i) r[i] = args[i];
    cpu_thread::push(this);
    ++depth_;
    uint64_t res = native_call(fn, r, nargs > 4 ? args + 4 : nullptr, nargs > 4 ? static_cast<uint32_t>(nargs - 4) : 0);
    --depth_;
    cpu_thread::pop();
    // r1 of a 64-bit result stays readable through reg(1), as with the JIT.
    frame_[1] = static_cast<uint32_t>(res >> 32);
    return static_cast<uint32_t>(res);
}

namespace hle {
void backend_init_stub_page() {}

void write_stub(addr_t at, uint32_t idx) {
    // ldr r12, [pc, #0]  ; pc = at+8  -> r12 = idx
    // ldr pc,  [pc, #0]  ; pc = at+12 -> jump to hle_native_entry (ARM code)
    guest::write32(at, 0xe59fc000);
    guest::write32(at + 4, 0xe59ff000);
    guest::write32(at + 8, idx);
    guest::write32(at + 12, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hle_native_entry)));
}
}  // namespace hle
