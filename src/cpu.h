// Guest CPU: a dynarmic A32 JIT plus the host<->guest call bridge.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "guest.h"

namespace Dynarmic::A32 {
class Jit;
}

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

    class Callbacks;

private:
    void run_until_return();

    const char *name_;
    std::unique_ptr<Callbacks> callbacks_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    addr_t stack_top_;
    int depth_ = 0;
};

namespace hle {
constexpr addr_t STUB_BASE = 0x4a400000;
constexpr uint32_t RETURN_SVC = 0xfffff;
constexpr addr_t RETURN_ADDR = STUB_BASE - 8;

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
