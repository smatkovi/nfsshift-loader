// Runtime core: event queue, callbacks, timers, yield, and the s3eDevice,
// s3eSurface and s3eGL APIs.
#include <algorithm>
#include <SDL.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <list>
#include <mutex>
#include <vector>

#include "config.h"
#include "display.h"
#include "hle.h"
#include "runtime.h"

namespace runtime {
namespace {
Paths g_paths;
std::atomic<int32_t> g_errors[DEV_COUNT];

struct Callback {
    Dev dev;
    int32_t cbid;
    addr_t fn;
    addr_t user;
};
std::list<Callback> g_callbacks;

struct Event {
    Dev dev;
    int32_t cbid;
    std::vector<uint8_t> data;
};
std::mutex g_queue_mutex;
std::deque<Event> g_queue;
std::atomic<bool> g_wake{false};

struct Timer {
    uint64_t due;
    addr_t fn;
    addr_t user;
};
std::list<Timer> g_timers;

bool g_quit = false;
bool g_paused = false;
int32_t g_device_state = 5;  // RUNNING
uint64_t g_start_ms;
}  // namespace

void set_paths(const Paths &p) { g_paths = p; }
const Paths &paths() { return g_paths; }

uint64_t now_ms() { return SDL_GetTicks64() - g_start_ms; }

void set_error(Dev dev, int32_t code) { g_errors[dev] = code; }
int32_t take_error(Dev dev) { return g_errors[dev].exchange(0); }

int32_t register_callback(Dev dev, int max_cbid, int32_t cbid, addr_t fn, addr_t user) {
    if (cbid < 0 || cbid >= max_cbid || !fn) {
        set_error(dev, 1);
        return S3E_RESULT_ERROR;
    }
    for (const Callback &c : g_callbacks)
        if (c.dev == dev && c.cbid == cbid && c.fn == fn) {
            set_error(dev, 3);
            return S3E_RESULT_ERROR;
        }
    g_callbacks.push_back({dev, cbid, fn, user});
    return S3E_RESULT_SUCCESS;
}

int32_t unregister_callback(Dev dev, int max_cbid, int32_t cbid, addr_t fn) {
    if (cbid < 0 || cbid >= max_cbid) {
        set_error(dev, 1);
        return S3E_RESULT_ERROR;
    }
    bool found = false;
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        if (it->dev == dev && it->cbid == cbid && (!fn || it->fn == fn)) {
            it = g_callbacks.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (!found) {
        set_error(dev, 4);
        return S3E_RESULT_ERROR;
    }
    return S3E_RESULT_SUCCESS;
}

bool has_callback(Dev dev, int32_t cbid) {
    for (const Callback &c : g_callbacks)
        if (c.dev == dev && c.cbid == cbid) return true;
    return false;
}

void queue_event(Dev dev, int32_t cbid, const void *data, size_t size) {
    Event e{dev, cbid, {}};
    if (size) e.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(e));
    g_wake = true;
}

void fire_event(Dev dev, int32_t cbid, const void *data, size_t size) {
    // Copy the list first: callbacks may (un)register callbacks.
    std::vector<Callback> targets;
    for (const Callback &c : g_callbacks)
        if (c.dev == dev && c.cbid == cbid) targets.push_back(c);
    if (targets.empty()) return;
    addr_t sys = 0;
    if (size) {
        sys = guest::alloc(size);
        memcpy(gptr(sys), data, size);
    }
    for (const Callback &c : targets) Cpu::current().call(c.fn, {sys, c.user});
    guest::release(sys);
}

namespace {
void deliver_queue() {
    for (;;) {
        Event e;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            if (g_queue.empty()) return;
            e = std::move(g_queue.front());
            g_queue.pop_front();
        }
        fire_event(e.dev, e.cbid, e.data.data(), e.data.size());
    }
}

void fire_timers() {
    uint64_t now = now_ms();
    for (;;) {
        auto due = g_timers.end();
        for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
            if (it->due <= now && (due == g_timers.end() || it->due < due->due)) due = it;
        if (due == g_timers.end()) return;
        Timer t = *due;
        g_timers.erase(due);
        g_wake = true;
        Cpu::current().call(t.fn, {0, t.user});
    }
}

void set_paused(bool paused) {
    static const bool ignore = getenv("NFS_IGNORE_PAUSE") != nullptr;
    if (ignore || paused == g_paused) return;
    g_paused = paused;
    g_device_state = paused ? 3 : 5;
    logf("[device] %s", paused ? "pause" : "unpause");
    sound::set_paused(paused);
    queue_event(DEV_DEVICE, paused ? 0 : 1, nullptr, 0);
}

void pump_host_events() {
    display::pump([](const SDL_Event &ev) {
        switch (ev.type) {
        case SDL_QUIT:
            if (!g_quit) {
                g_quit = true;
                queue_event(DEV_DEVICE, 2, nullptr, 0);
            }
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST || ev.window.event == SDL_WINDOWEVENT_MINIMIZED ||
                ev.window.event == SDL_WINDOWEVENT_HIDDEN)
                set_paused(true);
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || ev.window.event == SDL_WINDOWEVENT_RESTORED ||
                     ev.window.event == SDL_WINDOWEVENT_SHOWN)
                set_paused(false);
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            set_paused(true);
            break;
        case SDL_APP_DIDENTERFOREGROUND:
            set_paused(false);
            break;
        default:
            input::handle_event(ev);
            break;
        }
    });
    input::update();
    sound::pump();
}
}  // namespace

void (*pump_hook)() = nullptr;
int64_t utc_offset_ms = 0;

void yield(int32_t ms, bool until_event) {
    if (pump_hook) pump_hook();
    if (until_event) {
        if (g_quit) ms = 0;
        else if (ms == 0) ms = 0x7fffffff;
    }
    g_wake = false;
    deliver_queue();
    pump_host_events();
    fire_timers();
    deliver_queue();
    if (ms <= 0) return;
    uint64_t end = now_ms() + static_cast<uint64_t>(ms);
    while (!(until_event && g_wake)) {
        uint64_t now = now_ms();
        if (now >= end) break;
        uint64_t step = std::min<uint64_t>(end - now, 5);
        for (const Timer &t : g_timers)
            if (t.due > now) step = std::min<uint64_t>(step, t.due - now);
        SDL_Delay(static_cast<Uint32>(std::max<uint64_t>(step, 1)));
        pump_host_events();
        fire_timers();
        deliver_queue();
    }
}

bool quit_requested() { return g_quit; }

void init() {
    g_start_ms = SDL_GetTicks64();
    SDL_DisableScreenSaver();
    input::init();
    sound::init();
}

void shutdown() { sound::shutdown(); }
}  // namespace runtime

// ---------------------------------------------------------------------------
namespace {
using namespace runtime;

int32_t s3eTimerSetTimer(uint32_t ms, addr_t fn, addr_t user) {
    if (!fn) {
        set_error(DEV_TIMER, 1);
        return S3E_RESULT_ERROR;
    }
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
        if (it->fn == fn && it->user == user) {
            g_timers.erase(it);
            break;
        }
    if (g_timers.size() >= 32) {
        set_error(DEV_TIMER, 2);
        return S3E_RESULT_ERROR;
    }
    g_timers.push_back({now_ms() + ms, fn, user});
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eTimerSetTimer);

uint64_t s3eDeviceYield(int32_t ms) {
    static const bool trace = getenv("NFS_TRACE_YIELD") != nullptr;
    static int traced = 0;
    if (trace && traced < 200) {
        ++traced;
        logf("[yield] %d ms at t=%llu", ms, static_cast<unsigned long long>(now_ms()));
    }
    yield(ms, false);
    return now_ms();
}
HLE_REGISTER(s3eDeviceYield);

void s3eDeviceYieldUntilEvent(int32_t ms) { yield(ms, true); }
HLE_REGISTER(s3eDeviceYieldUntilEvent);

uint8_t s3eDeviceCheckQuitRequest() { return g_quit ? 1 : 0; }
HLE_REGISTER(s3eDeviceCheckQuitRequest);

void s3eDeviceRequestQuit() {
    if (g_paused) queue_event(DEV_DEVICE, 1, nullptr, 0);
    g_device_state = 4;
    if (!g_quit) {
        g_quit = true;
        queue_event(DEV_DEVICE, 2, nullptr, 0);
    }
}
HLE_REGISTER(s3eDeviceRequestQuit);

void s3eDeviceExit(int32_t code) {
    logf("[device] s3eDeviceExit(%d)", code);
    sound::shutdown();
    display::shutdown();
    _exit(code);
}
HLE_REGISTER(s3eDeviceExit);

void s3eDeviceBacklightOn() {}
HLE_REGISTER(s3eDeviceBacklightOn);

int64_t meminfo_kb(const char *key) {
    std::ifstream f("/proc/meminfo");
    std::string k;
    int64_t v;
    std::string unit;
    while (f >> k >> v >> unit)
        if (k == key) return v;
    return 0;
}

int32_t language_id() {
    const char *lang = getenv("LANG");
    std::string l = lang ? lang : "en";
    static const std::pair<const char *, int32_t> table[] = {
        {"en", 1},  {"fr", 2},  {"de", 3},  {"es", 4},  {"it", 5},  {"pt", 6},  {"nl", 7},  {"tr", 8},
        {"hr", 9},  {"cs", 10}, {"da", 11}, {"fi", 12}, {"hu", 13}, {"no", 14}, {"nb", 14}, {"pl", 15},
        {"ru", 16}, {"sr", 17}, {"sk", 18}, {"sl", 19}, {"sv", 20}, {"uk", 21}, {"el", 22}, {"ja", 23},
        {"ko", 26}, {"is", 27}, {"th", 29}};
    if (l.compare(0, 2, "zh") == 0) return l.find("TW") != std::string::npos ? 25 : 24;
    for (auto &e : table)
        if (l.compare(0, 2, e.first) == 0) return e.second;
    return 1;
}

int32_t s3eDeviceGetInt(int32_t prop) {
    switch (prop) {
    case 0: return 23;       // S3E_OS_ID_MEEGO
    case 1: return 5002;     // S3E_DEVICE_CLASS_LINUX_EMBED
    case 2: return 1;        // unknown device
    case 4: return language_id();
    case 5: return g_device_state;
    case 6: return static_cast<int32_t>(std::min<int64_t>(meminfo_kb("MemAvailable:") << 10, 0x7fffffff));
    case 7: return static_cast<int32_t>(std::min<int64_t>(meminfo_kb("MemTotal:") << 10, 0x7fffffff));
    case 8: return 12;       // ARM7
    case 9: return 0x41e01;  // s3e 4.30.1
    case 10: return 100;
    case 11: return 1;
    case 12: return 1;
    case 13: return 0x20006;  // kernel 2.6 as on the N9
    case 15: return 0;
    case 16: return 1;  // VFP
    case 17: return 0;
    case 18: return 1;
    case 19: return 0x50001;
    case 22: return 0;
    case 23: return 0;
    case 27: return 0;
    case 28: return 1;
    case 29: return static_cast<int32_t>(meminfo_kb("MemTotal:"));
    case 30: return static_cast<int32_t>(meminfo_kb("MemAvailable:"));
    case 25: set_error(DEV_DEVICE, 7); return -1;
    default: set_error(DEV_DEVICE, 1); return -1;
    }
}
HLE_REGISTER(s3eDeviceGetInt);

int32_t s3eDeviceRegister(int32_t cbid, addr_t fn, addr_t user) {
    return register_callback(DEV_DEVICE, 24, cbid, fn, user);
}
HLE_REGISTER(s3eDeviceRegister);

int32_t s3eDeviceUnRegister(int32_t cbid, addr_t fn) { return unregister_callback(DEV_DEVICE, 24, cbid, fn); }
HLE_REGISTER(s3eDeviceUnRegister);

// --- surface ------------------------------------------------------------------
constexpr int32_t kPixelType = 0x422;  // RGB565
addr_t g_surface_buffer;

int32_t quantise(int32_t v) {
    static const int32_t steps[] = {128, 176, 208, 220, 240, 288, 320, 352, 416, 480, 512, 540,
                                    600, 640, 768, 800, 960, 1024, 1152, 1280, 1600, 2048, 4096};
    for (int32_t s : steps)
        if (s >= v) return s;
    return v;
}

int32_t s3eSurfaceGetInt(int32_t prop) {
    int32_t w = display::width(), h = display::height();
    switch (prop) {
    case 0: case 4: return w;
    case 1: case 5: return h;
    case 2: case 7: return w * 2;
    case 3: case 6: return kPixelType;
    case 8: return 0;
    case 9: return 0;
    case 10: return 1;
    case 11: return 0;
    case 12: return quantise(w);
    case 13: return quantise(h);
    default: set_error(DEV_SURFACE, 1); return -1;
    }
}
HLE_REGISTER(s3eSurfaceGetInt);

addr_t s3eSurfacePtr() {
    if (!g_surface_buffer) g_surface_buffer = guest::alloc(display::width() * display::height() * 2);
    return g_surface_buffer;
}
HLE_REGISTER(s3eSurfacePtr);

int32_t s3eSurfaceSetup(int32_t type, int32_t pitch, addr_t surface, int32_t dir) { return S3E_RESULT_SUCCESS; }
HLE_REGISTER(s3eSurfaceSetup);

void s3eSurfaceShow() {}
HLE_REGISTER(s3eSurfaceShow);

int32_t s3eSurfaceRegister(int32_t cbid, addr_t fn, addr_t user) {
    return register_callback(DEV_SURFACE, 2, cbid, fn, user);
}
HLE_REGISTER(s3eSurfaceRegister);

int32_t s3eSurfaceUnRegister(int32_t cbid, addr_t fn) { return unregister_callback(DEV_SURFACE, 2, cbid, fn); }
HLE_REGISTER(s3eSurfaceUnRegister);

// --- GL -----------------------------------------------------------------------
int32_t s3eGLGetInt(int32_t prop) {
    switch (prop) {
    case 0: return 0x200;  // GLES 2.0 (the device has no GLES 1 driver)
    case 1: return 0x103;
    case 2: return 0;      // context survives suspend
    default: set_error(DEV_GL, 1); return -1;
    }
}
HLE_REGISTER(s3eGLGetInt);

addr_t s3eGLGetNativeWindow() { return 0x4e57; }
HLE_REGISTER(s3eGLGetNativeWindow);

int32_t s3eGLRegister(int32_t cbid, addr_t fn, addr_t user) { return register_callback(DEV_GL, 2, cbid, fn, user); }
HLE_REGISTER(s3eGLRegister);

int32_t s3eGLUnRegister(int32_t cbid, addr_t fn) { return unregister_callback(DEV_GL, 2, cbid, fn); }
HLE_REGISTER(s3eGLUnRegister);
}  // namespace
