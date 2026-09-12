// Shared state of the reimplemented Marmalade runtime.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "guest.h"

union SDL_Event;

namespace runtime {
inline constexpr const char *kOsName = "MEEGO";

constexpr int32_t S3E_RESULT_SUCCESS = 0;
constexpr int32_t S3E_RESULT_ERROR = 1;

// Error/callback device ids used by the original loader.
enum Dev : int {
    DEV_DEVICE = 0,
    DEV_FILE = 1,
    DEV_SURFACE = 2,
    DEV_AUDIO = 3,
    DEV_POINTER = 6,
    DEV_DEBUG = 7,
    DEV_MEMORY = 9,
    DEV_SOUND = 0xb,
    DEV_SOCKET = 0xc,
    DEV_KEYBOARD = 0xd,
    DEV_TIMER = 0xe,
    DEV_CONFIG = 0x12,
    DEV_GL = 0x14,
    DEV_COMPRESSION = 0x15,
    DEV_EXT = 0x18,
    DEV_ACCEL = 0x19,
    DEV_COUNT = 0x20,
};

struct Paths {
    std::string data;  // rom://, read-only game data
    std::string user;  // ram://, writable
};

void set_paths(const Paths &p);
const Paths &paths();

void init();
void shutdown();

// Per-subsystem error codes (s3eXxxGetError reads and clears them).
void set_error(Dev dev, int32_t code);
int32_t take_error(Dev dev);

// Callback registry shared by device/keyboard/pointer/surface/GL/debug/audio.
int32_t register_callback(Dev dev, int max_cbid, int32_t cbid, addr_t fn, addr_t user);
int32_t unregister_callback(Dev dev, int max_cbid, int32_t cbid, addr_t fn);
bool has_callback(Dev dev, int32_t cbid);
// Queue an event; `data` is copied and passed as systemData during the next yield.
void queue_event(Dev dev, int32_t cbid, const void *data, size_t size);
// Fire immediately on the calling guest CPU.
void fire_event(Dev dev, int32_t cbid, const void *data, size_t size);

// Called from s3eDeviceYield.
void yield(int32_t ms, bool until_event);
bool quit_requested();

uint64_t now_ms();

// Run once per yield; the LAN multiplayer uses it as its frame pump.
extern void (*pump_hook)();

// Added to s3eTimerGetUTC, which is the only clock the game's multiplayer
// timing is derived from. The LAN client shifts it onto the host's clock.
extern int64_t utc_offset_ms;
}  // namespace runtime

// Implemented by hle_input.cpp / hle_sound.cpp, driven by the runtime.
namespace input {
void init();
void handle_event(const SDL_Event &ev);
void update();  // once per yield, after events
}  // namespace input

namespace sound {
void init();
void shutdown();
void set_paused(bool paused);
void pump();  // main-thread housekeeping in yield
}  // namespace sound

namespace files {
// Resolve an s3e path to a host path for reading (rom/ram/raw only).
bool host_path_for_read(const char *path, std::string &out);
}  // namespace files
