// JIT backend: dynarmic A32 on a 64-bit host.
#include "cpu.h"

#include <cstring>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

using Dynarmic::HaltReason;


class Cpu::Callbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Callbacks(Cpu &cpu) : cpu_(cpu) {}

    uint32_t svc = 0;
    Dynarmic::A32::Jit *jit = nullptr;

    template <class T> T read(uint32_t va) {
        if (!guest::is_mapped(va) || !guest::is_mapped(va + sizeof(T) - 1)) {
            bad_access("read", va);
            return 0;
        }
        T v;
        memcpy(&v, gptr(va), sizeof(T));
        return v;
    }
    template <class T> void write(uint32_t va, T v) {
        if (!guest::is_mapped(va) || !guest::is_mapped(va + sizeof(T) - 1)) {
            bad_access("write", va);
            return;
        }
        memcpy(gptr(va), &v, sizeof(T));
    }

    std::optional<uint32_t> MemoryReadCode(uint32_t va) override {
        if (!guest::is_mapped(va) || !guest::is_mapped(va + 3)) {
            logf("[%s] jump to unmapped address %#x (lr=%#x)", cpu_.name(), va, jit->Regs()[14]);
            return std::nullopt;
        }
        return guest::read32(va);
    }

    uint8_t MemoryRead8(uint32_t va) override { return read<uint8_t>(va); }
    uint16_t MemoryRead16(uint32_t va) override { return read<uint16_t>(va); }
    uint32_t MemoryRead32(uint32_t va) override { return read<uint32_t>(va); }
    uint64_t MemoryRead64(uint32_t va) override { return read<uint64_t>(va); }
    void MemoryWrite8(uint32_t va, uint8_t v) override { write(va, v); }
    void MemoryWrite16(uint32_t va, uint16_t v) override { write(va, v); }
    void MemoryWrite32(uint32_t va, uint32_t v) override { write(va, v); }
    void MemoryWrite64(uint32_t va, uint64_t v) override { write(va, v); }

    void InterpreterFallback(uint32_t pc, size_t) override {
        fatal("[%s] interpreter fallback requested at %#x", cpu_.name(), pc);
    }

    void CallSVC(uint32_t swi) override {
        svc = swi;
        jit->HaltExecution(HaltReason::UserDefined1);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        switch (e) {
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        default:
            break;
        }
        uint32_t insn = guest::is_mapped(pc) ? guest::read32(pc) : 0;
        logf("[%s] guest exception %d at pc=%#x insn=%08x lr=%#x", cpu_.name(), static_cast<int>(e), pc, insn,
             jit->Regs()[14]);
        jit->HaltExecution(HaltReason::UserDefined2);
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1000000; }

private:
    void bad_access(const char *what, uint32_t va) {
        static int reported = 0;
        if (reported < 64) {
            ++reported;
            logf("[%s] invalid %s at %#x (pc=%#x lr=%#x)", cpu_.name(), what, va, jit->Regs()[15],
                 jit->Regs()[14]);
        }
    }

    Cpu &cpu_;
};

Cpu::Cpu(const char *name) : name_(name), callbacks_(std::make_unique<Callbacks>(*this)) {
    static std::mutex stack_mutex;
    static addr_t next_stack = layout::STACK_BASE;
    {
        std::lock_guard<std::mutex> lock(stack_mutex);
        // STACK_BASE is a wish as well: a stack goes to the next free 4 MB
        // window when the planned one is taken (see guest::find_free_span).
        size_t got = 0;
        addr_t base = guest::find_free_span(next_stack, layout::STACK_SIZE, layout::STACK_SIZE, &got);
        if (!base || !guest::map_fixed(base, layout::STACK_SIZE))
            fatal("cannot map a guest stack (wanted %#x)", next_stack);
        if (base != next_stack) logf("[cpu] stack for %s moved to %#x (wanted %#x)", name, base, next_stack);
        stack_top_ = base + layout::STACK_SIZE - 16;
        next_stack = base + layout::STACK_SIZE + 0x10000;
    }

    Dynarmic::A32::UserConfig config;
    config.callbacks = callbacks_.get();
    // The JIT reads and writes guest memory directly at base + address; a
    // fault (an unmapped page in the reservation) recompiles that access to go
    // through the callbacks above. With the old identity mapping the base was 0.
    config.fastmem_pointer = guest::g_base;
    config.recompile_on_fastmem_failure = true;
    config.enable_cycle_counting = false;
    config.code_cache_size = 128 * 1024 * 1024;
    jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
    callbacks_->jit = jit_.get();
    jit_->Regs()[13] = stack_top_;
    jit_->SetCpsr(0x10);  // user mode, ARM
}

Cpu::~Cpu() = default;

uint32_t &Cpu::reg(int i) { return jit_->Regs()[i]; }
uint32_t Cpu::cpsr() const { return jit_->Cpsr(); }
void Cpu::set_cpsr(uint32_t v) { jit_->SetCpsr(v); }

uint32_t Cpu::call(addr_t fn, const uint32_t *args, size_t nargs) {
    if (!fn) fatal("[%s] call to null guest function", name_);
    std::array<uint32_t, 16> saved = jit_->Regs();
    uint32_t saved_cpsr = jit_->Cpsr();
    std::array<uint32_t, 64> saved_ext = jit_->ExtRegs();
    uint32_t saved_fpscr = jit_->Fpscr();

    auto &r = jit_->Regs();
    uint32_t sp = depth_ == 0 ? stack_top_ : r[13];
    sp = (sp - 64) & ~7u;
    for (size_t i = 0; i < nargs && i < 4; ++i) r[i] = args[i];
    if (nargs > 4) {
        size_t n = nargs - 4;
        sp = (sp - 4 * n) & ~7u;
        for (size_t k = 0; k < n; ++k) guest::write32(sp + 4 * k, args[4 + k]);
    }
    r[13] = sp;
    r[14] = hle::RETURN_ADDR;
    r[15] = fn & ~1u;
    jit_->SetCpsr((saved_cpsr & ~0x20u) | (fn & 1 ? 0x20u : 0u));

    cpu_thread::push(this);
    ++depth_;
    run_until_return();
    --depth_;
    cpu_thread::pop();

    uint32_t result = r[0];
    uint32_t result_hi = r[1];
    r = saved;
    jit_->SetCpsr(saved_cpsr);
    jit_->ExtRegs() = saved_ext;
    jit_->SetFpscr(saved_fpscr);
    r[1] = result_hi;
    return result;
}

void Cpu::run_until_return() {
    for (;;) {
        HaltReason hr = jit_->Run();
        if (Dynarmic::Has(hr, HaltReason::UserDefined1)) {
            uint32_t svc = callbacks_->svc;
            if (svc == hle::RETURN_SVC) return;
            hle::dispatch(*this, svc);
            continue;
        }
        auto &r = jit_->Regs();
        fatal("[%s] guest halted (reason %#x) pc=%#x lr=%#x sp=%#x r0=%#x", name_, static_cast<unsigned>(hr), r[15],
              r[14], r[13], r[0]);
    }
}

// ---------------------------------------------------------------------------
// Stub encoding for the JIT: the stub raises an SVC with the import index,
// dispatch() runs the host function, then `bx lr` returns to the game.

namespace hle {
void backend_init_stub_page() {
    guest::write32(RETURN_ADDR, 0xef000000 | RETURN_SVC);
    guest::write32(RETURN_ADDR + 4, 0xeafffffd);  // b RETURN_ADDR
}

void write_stub(addr_t at, uint32_t idx) {
    guest::write32(at, 0xef000000 | idx);  // svc #idx
    guest::write32(at + 4, 0xe12fff1e);    // bx lr
}
}  // namespace hle
