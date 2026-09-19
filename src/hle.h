// Automatic marshalling between the guest AAPCS (softfp) calling convention
// and plain host C/C++ functions.
//
//   HLE_REGISTER(s3eTimerGetMs);          // host function with the same name
//   hle::reg("glClear", HLE_WRAP(::glClear));
#pragma once

#include <bit>
#include <cstring>
#include <tuple>
#include <type_traits>

#include "cpu.h"

namespace hle {

struct ArgCursor {
    Cpu &cpu;
    int ncrn = 0;
    uint32_t nsaa = 0;

    uint32_t word() {
        if (ncrn < 4) return cpu.reg(ncrn++);
        uint32_t v = guest::read32(cpu.reg(13) + nsaa);
        nsaa += 4;
        return v;
    }
    uint64_t dword() {
        if (ncrn < 4) {
            if (ncrn & 1) ++ncrn;
            if (ncrn <= 2) {
                uint64_t lo = cpu.reg(ncrn), hi = cpu.reg(ncrn + 1);
                ncrn += 2;
                return lo | (hi << 32);
            }
            ncrn = 4;
        }
        nsaa = (nsaa + 7) & ~7u;
        uint64_t lo = guest::read32(cpu.reg(13) + nsaa), hi = guest::read32(cpu.reg(13) + nsaa + 4);
        nsaa += 8;
        return lo | (hi << 32);
    }
};

template <class T> T get_arg(ArgCursor &c) {
    // Guest pointers become host pointers into the guest window -- except 0,
    // which has to stay nullptr: GL and s3e functions test for NULL.
    if constexpr (std::is_pointer_v<T>) {
        uint32_t w = c.word();
        return w ? reinterpret_cast<T>(gptr(w)) : nullptr;
    }
    else if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(c.word());
    else if constexpr (std::is_same_v<T, double>) return std::bit_cast<double>(c.dword());
    // Only long long is a 64-bit guest argument; host `long` (GLsizeiptr,
    // GLintptr) is 32 bits wide on the guest.
    else if constexpr (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>)
        return static_cast<T>(c.dword());
    else if constexpr (std::is_signed_v<T>) return static_cast<T>(static_cast<int32_t>(c.word()));
    else return static_cast<T>(c.word());
}

template <class R> void set_ret(Cpu &cpu, R r) {
    if constexpr (std::is_pointer_v<R>) {
        cpu.reg(0) = gaddr(r);
    } else if constexpr (std::is_same_v<R, float>) {
        cpu.reg(0) = std::bit_cast<uint32_t>(r);
    } else if constexpr (std::is_same_v<R, double>) {
        uint64_t v = std::bit_cast<uint64_t>(r);
        cpu.reg(0) = static_cast<uint32_t>(v);
        cpu.reg(1) = static_cast<uint32_t>(v >> 32);
    } else if constexpr (sizeof(R) == 8) {
        uint64_t v = static_cast<uint64_t>(r);
        cpu.reg(0) = static_cast<uint32_t>(v);
        cpu.reg(1) = static_cast<uint32_t>(v >> 32);
    } else if constexpr (std::is_same_v<R, bool>) {
        cpu.reg(0) = r ? 1 : 0;
    } else {
        cpu.reg(0) = static_cast<uint32_t>(static_cast<int64_t>(r));
    }
}

template <auto F, class R, class... A> void thunk_impl(Cpu &cpu, R (*)(A...)) {
    [[maybe_unused]] ArgCursor cur{cpu};
    std::tuple<A...> args{get_arg<A>(cur)...};  // braced init: evaluated left to right
    if constexpr (std::is_void_v<R>) {
        std::apply(F, args);
    } else {
        set_ret<R>(cpu, std::apply(F, args));
    }
}

template <auto F> void thunk(Cpu &cpu) { thunk_impl<F>(cpu, F); }

inline void reg(const char *name, HleFn fn) { register_fn(name, fn); }

struct AutoRegister {
    AutoRegister(const char *name, HleFn fn) { register_fn(name, fn); }
};

}  // namespace hle

#define HLE_WRAP(f) (&hle::thunk<static_cast<std::decay_t<decltype(f)>>(f)>)
#define HLE_REGISTER(f) static hle::AutoRegister hle_reg_##f(#f, HLE_WRAP(f))
