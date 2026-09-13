#include "cpu.h"

#include <pthread.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

using Dynarmic::HaltReason;

namespace {
// No thread_local here: on libhybris the Android GL driver writes bionic TLS
// slots that overlap the executable's static TLS block.
struct ThreadCpus {
    pthread_t thread;
    std::vector<Cpu *> stack;
};
std::mutex g_threads_mutex;
std::vector<ThreadCpus *> g_threads;

std::vector<Cpu *> &cpu_stack() {
    pthread_t self = pthread_self();
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (ThreadCpus *t : g_threads)
        if (pthread_equal(t->thread, self)) return t->stack;
    g_threads.push_back(new ThreadCpus{self, {}});
    return g_threads.back()->stack;
}
}

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
    config.fastmem_pointer = 0;
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

Cpu &Cpu::current() {
    std::vector<Cpu *> &stack = cpu_stack();
    if (stack.empty()) fatal("no guest CPU on this thread");
    return *stack.back();
}

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

    std::vector<Cpu *> &stack = cpu_stack();
    stack.push_back(this);
    ++depth_;
    run_until_return();
    --depth_;
    stack.pop_back();

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
// HLE stub table

namespace hle {
namespace {
std::unordered_map<std::string, HleFn> &registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
std::vector<std::string> g_stub_names;
std::vector<HleFn> g_stub_fns;
std::vector<uint32_t> g_stub_calls;
bool g_trace = false;
bool g_stub_page_mapped = false;

void unimplemented(Cpu &cpu) {}
}  // namespace

void register_fn(const char *name, HleFn fn) { registry()[name] = fn; }

HleFn lookup(const char *name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

void map_stub_page() {
    if (g_stub_page_mapped) return;
    if (!guest::map_fixed(STUB_BASE - 0x1000, 0x10000)) fatal("cannot map HLE stub page");
    guest::write32(RETURN_ADDR, 0xef000000 | RETURN_SVC);
    guest::write32(RETURN_ADDR + 4, 0xeafffffd);  // b RETURN_ADDR
    g_stub_page_mapped = true;
}

addr_t stub_for(const char *name) {
    map_stub_page();
    for (size_t i = 0; i < g_stub_names.size(); ++i)
        if (g_stub_names[i] == name) return STUB_BASE + 8 * i;
    size_t idx = g_stub_names.size();
    if (idx >= 0x1ff0) fatal("too many HLE stubs");
    g_stub_names.emplace_back(name);
    HleFn fn = lookup(name);
    if (!fn) logf("[hle] no implementation for %s", name);
    g_stub_fns.push_back(fn ? fn : unimplemented);
    g_stub_calls.push_back(0);
    addr_t at = STUB_BASE + 8 * idx;
    guest::write32(at, 0xef000000 | static_cast<uint32_t>(idx));  // svc #idx
    guest::write32(at + 4, 0xe12fff1e);                           // bx lr
    return at;
}

void set_trace(bool on) { g_trace = on; }

bool patch_arm_function(addr_t site, uint32_t expect, const char *name) {
    uint32_t insn = guest::read32(site);
    if (insn != expect) {
        logf("[patch] %s at %#x: expected %08x, found %08x", name, site, expect, insn);
        return false;
    }
    addr_t target = stub_for(name);
    int64_t delta = (static_cast<int64_t>(target) - (static_cast<int64_t>(site) + 8)) >> 2;
    if (delta < -(1 << 23) || delta >= (1 << 23)) {
        logf("[patch] %s at %#x: stub out of branch range", name, site);
        return false;
    }
    guest::write32(site, 0xea000000 | (static_cast<uint32_t>(delta) & 0xffffff));  // b stub
    return true;
}

bool patch_bytes(addr_t site, const uint8_t *expect, const uint8_t *replacement, size_t size) {
    if (memcmp(gptr(site), expect, size) != 0) {
        logf("[patch] bytes at %#x do not match", site);
        return false;
    }
    memcpy(gptr(site), replacement, size);
    return true;
}
void (*post_call_hook)(Cpu &cpu, uint32_t svc) = nullptr;

const char *name_of(uint32_t svc) { return svc < g_stub_names.size() ? g_stub_names[svc].c_str() : "?"; }

void dispatch(Cpu &cpu, uint32_t svc) {
    if (svc >= g_stub_fns.size()) fatal("[%s] unknown svc %#x at pc=%#x", cpu.name(), svc, cpu.reg(15));
    uint32_t n = ++g_stub_calls[svc];
    if (g_stub_fns[svc] == unimplemented) {
        if (n <= 3) logf("[hle] unimplemented %s(%#x, %#x, %#x, %#x) lr=%#x", g_stub_names[svc].c_str(), cpu.reg(0),
                         cpu.reg(1), cpu.reg(2), cpu.reg(3), cpu.reg(14));
        cpu.reg(0) = 0;
        return;
    }
    if (g_trace && n <= 20)
        logf("[trace] %s(%#x, %#x, %#x, %#x) lr=%#x", g_stub_names[svc].c_str(), cpu.reg(0), cpu.reg(1), cpu.reg(2),
             cpu.reg(3), cpu.reg(14));
    g_stub_fns[svc](cpu);
    if (post_call_hook) post_call_hook(cpu, svc);
}
}  // namespace hle
