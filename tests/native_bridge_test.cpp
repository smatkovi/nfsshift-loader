// Unit test of the native backend's call bridge (GUEST_NATIVE, 32-bit ARM):
// a "game" compiled as plain C functions with word arguments -- which is what
// the soft-float ABI looks like from the outside -- calls import stubs whose
// host implementations take real floats, doubles, 64-bit values and pointers,
// and the host calls back into the "game" through Cpu::call, nested.
//
// Build inside the SDK container (armv7hl target, runs under qemu-arm):
//   sb2 -t SailfishOS-5.2.0.15-armv7hl g++ -std=c++20 -fPIC -pie -DGUEST_NATIVE=1 -Isrc -Ithird_party \
//       tests/native_bridge_test.cpp src/cpu_native.cpp src/cpu_native.S src/cpu_common.cpp \
//       src/hle_stubs.cpp src/guest.cpp third_party/dlmalloc.c -DONLY_MSPACES=1 -DMSPACES=1 \
//       -DHAVE_MMAP=0 -DHAVE_MORECORE=0 -DUSE_LOCKS=1 $(pkg-config --cflags --libs sdl2) -o build/native_bridge_test
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cpu.h"
#include "hle.h"

// ---- host side ("s3e") -----------------------------------------------------
static int g_calls;

static float host_mix(float a, int b, double c, const char *s, long long big, float last) {
    ++g_calls;
    // a=1.5 b=7 c=2.25 s="hi" big=0x1122334455667788 last=-3.0
    if (a != 1.5f || b != 7 || c != 2.25 || strcmp(s, "hi") != 0 || big != 0x1122334455667788LL || last != -3.0f) {
        fprintf(stderr, "host_mix: bad arguments a=%g b=%d c=%g s=%s big=%llx last=%g\n", a, b, c, s, big, last);
        return -1.0f;
    }
    return a + static_cast<float>(c) + last;  // 0.75
}

static unsigned long long host_wide(unsigned int lo, unsigned int hi) { return (static_cast<unsigned long long>(hi) << 32) | lo; }

static const char *host_name() { return "native"; }

// Calls back into the game while a guest call is on the stack.
static uint32_t g_guest_double_fn;
static int host_twice(int v) {
    uint32_t r = Cpu::current().call(g_guest_double_fn, {static_cast<uint32_t>(v)});
    return static_cast<int>(r) + 1;
}

HLE_REGISTER(host_mix);
HLE_REGISTER(host_wide);
HLE_REGISTER(host_name);
HLE_REGISTER(host_twice);

// ---- "game" side: word arguments only, like soft-float code ----------------
// 64-bit values travel as uint64_t: integer alignment (even register pair,
// 8-byte stack slot) is the same in the soft-float and the hard-float ABI.
typedef uint32_t (*fn_mix)(uint32_t a, uint32_t b, uint64_t c, uint32_t s, uint64_t big, uint32_t last);
typedef uint64_t (*fn_wide)(uint32_t lo, uint32_t hi);
typedef uint32_t (*fn_name)();
typedef uint32_t (*fn_twice)(uint32_t v);

static struct {
    fn_mix mix;
    fn_wide wide;
    fn_name name;
    fn_twice twice;
} imports;

extern "C" __attribute__((noinline)) uint32_t game_double(uint32_t v) { return v * 2; }

extern "C" __attribute__((noinline)) uint32_t game_main(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                                       uint32_t arg4, uint32_t arg5) {
    if (arg0 != 10 || arg1 != 11 || arg2 != 12 || arg3 != 13 || arg4 != 14 || arg5 != 15) {
        fprintf(stderr, "game_main: bad arguments %u %u %u %u %u %u\n", arg0, arg1, arg2, arg3, arg4, arg5);
        return 1;
    }
    uint64_t c = std::bit_cast<uint64_t>(2.25);
    uint64_t big = 0x1122334455667788ull;
    uint32_t r = imports.mix(std::bit_cast<uint32_t>(1.5f), 7, c, reinterpret_cast<uint32_t>("hi"), big,
                             std::bit_cast<uint32_t>(-3.0f));
    if (std::bit_cast<float>(r) != 0.75f) {
        fprintf(stderr, "game_main: mix returned %g\n", std::bit_cast<float>(r));
        return 2;
    }
    uint64_t w = imports.wide(0xdeadbeef, 0x0badf00d);
    if (w != 0x0badf00ddeadbeefull) {
        fprintf(stderr, "game_main: wide returned %llx\n", (unsigned long long)w);
        return 3;
    }
    if (strcmp(reinterpret_cast<const char *>(imports.name()), "native") != 0) return 4;
    if (imports.twice(20) != 41) {
        fprintf(stderr, "game_main: twice returned %u\n", imports.twice(20));
        return 5;
    }
    return 0;
}

int main() {
    guest::init_address_space();
    hle::map_stub_page();
    imports.mix = reinterpret_cast<fn_mix>(hle::stub_for("host_mix"));
    imports.wide = reinterpret_cast<fn_wide>(hle::stub_for("host_wide"));
    imports.name = reinterpret_cast<fn_name>(hle::stub_for("host_name"));
    imports.twice = reinterpret_cast<fn_twice>(hle::stub_for("host_twice"));
    g_guest_double_fn = reinterpret_cast<uint32_t>(&game_double);

    Cpu cpu("test");
    uint32_t rc = cpu.call(reinterpret_cast<uint32_t>(&game_main), {10, 11, 12, 13, 14, 15});
    // a 64-bit result through call(): r1 via reg(1)
    uint32_t lo = cpu.call(reinterpret_cast<uint32_t>(imports.wide), {1, 2});
    uint32_t hi = cpu.reg(1);
    if (lo != 1 || hi != 2) {
        fprintf(stderr, "call(): 64-bit result lo=%u hi=%u\n", lo, hi);
        rc = 6;
    }
    printf("native bridge test: %s (rc=%u, %d host calls, stub stride %zu)\n", rc == 0 ? "OK" : "FAILED", rc, g_calls,
           hle::STUB_STRIDE);
    return rc == 0 ? 0 : 1;
}
