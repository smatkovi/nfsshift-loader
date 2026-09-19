// HLE stub table and dispatch, shared by the JIT and the native backend.
// Only the encoding of a stub differs (hle::write_stub in the backend).
#include "cpu.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

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
    // one page in front for RETURN_ADDR, then MAX_STUBS stubs
    size_t size = (0x1000 + MAX_STUBS * STUB_STRIDE + 0xfff) & ~static_cast<size_t>(0xfff);
    if (!guest::map_fixed(STUB_BASE - 0x1000, size)) fatal("cannot map HLE stub page");
    backend_init_stub_page();
    g_stub_page_mapped = true;
}

addr_t stub_for(const char *name) {
    map_stub_page();
    for (size_t i = 0; i < g_stub_names.size(); ++i)
        if (g_stub_names[i] == name) return STUB_BASE + STUB_STRIDE * i;
    size_t idx = g_stub_names.size();
    if (idx >= MAX_STUBS) fatal("too many HLE stubs");
    g_stub_names.emplace_back(name);
    HleFn fn = lookup(name);
    if (!fn) logf("[hle] no implementation for %s", name);
    g_stub_fns.push_back(fn ? fn : unimplemented);
    g_stub_calls.push_back(0);
    addr_t at = STUB_BASE + STUB_STRIDE * idx;
    write_stub(at, static_cast<uint32_t>(idx));
    guest::flush_code(at, STUB_STRIDE);
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
    guest::flush_code(site, 4);
    return true;
}

bool patch_bytes(addr_t site, const uint8_t *expect, const uint8_t *replacement, size_t size) {
    if (memcmp(gptr(site), expect, size) != 0) {
        logf("[patch] bytes at %#x do not match", site);
        return false;
    }
    memcpy(gptr(site), replacement, size);
    guest::flush_code(site, size);
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
