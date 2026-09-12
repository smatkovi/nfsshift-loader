// s3ePointer, s3eKeyboard and s3eAccelerometer on top of SDL events.
#include <algorithm>
#include <SDL.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "display.h"
#include "hle.h"
#include "runtime.h"
#include "sensors.h"

namespace {
using namespace runtime;

// --- keyboard -----------------------------------------------------------------
constexpr int kKeys = 211;
constexpr uint8_t DOWN = 1, PRESSED = 2, RELEASED = 4;
uint8_t g_live[kKeys], g_published[kKeys];
bool g_char_events = false;

// Abstract keys (200..210) and their default real keys.
const std::pair<int, int> kAbstract[] = {{200, 14}, {201, 15}, {202, 16}, {203, 17}, {204, 10}, {205, 12},
                                         {206, 9},  {207, 11}, {208, 78}, {209, 72}, {210, 73}};

int s3e_key(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 23 + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return 13 + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F9) return 49 + (k - SDLK_F1);
    switch (k) {
    case SDLK_ESCAPE: return 126;  // Back
    case SDLK_AC_BACK: return 126;
    case SDLK_TAB: return 2;
    case SDLK_BACKSPACE: return 3;
    case SDLK_RETURN: return 4;
    case SDLK_LSHIFT: return 5;
    case SDLK_LCTRL: return 6;
    case SDLK_SPACE: return 8;
    case SDLK_LEFT: return 9;
    case SDLK_UP: return 10;
    case SDLK_RIGHT: return 11;
    case SDLK_DOWN: return 12;
    case SDLK_F10: return 58;
    case SDLK_F11: return 97;
    case SDLK_F12: return 98;
    case SDLK_VOLUMEUP: return 80;
    case SDLK_VOLUMEDOWN: return 81;
    case SDLK_MENU: return 127;
    case SDLK_LALT: return 99;
    case SDLK_RCTRL: return 100;
    case SDLK_RALT: return 101;
    case SDLK_RSHIFT: return 102;
    case SDLK_COMMA: return 104;
    case SDLK_PERIOD: return 105;
    case SDLK_SLASH: return 106;
    case SDLK_MINUS: return 113;
    case SDLK_EQUALS: return 112;
    default: return 0;
    }
}

void key_event(int key, bool pressed) {
    if (key <= 0 || key >= kKeys) return;
    if (pressed) {
        if (g_live[key] & DOWN) return;  // no auto-repeat
        g_live[key] |= DOWN | PRESSED;
    } else {
        if (!(g_live[key] & DOWN)) return;
        g_live[key] = (g_live[key] & ~DOWN) | RELEASED;
    }
    int32_t ev[2] = {key, pressed ? 1 : 0};
    queue_event(DEV_KEYBOARD, 0, ev, sizeof(ev));
    for (auto &a : kAbstract)
        if (a.second == key) {
            int32_t aev[2] = {a.first, pressed ? 1 : 0};
            queue_event(DEV_KEYBOARD, 0, aev, sizeof(aev));
        }
}

// --- pointer ------------------------------------------------------------------
constexpr int kSlots = 10;
struct Slot {
    bool used = false;
    SDL_FingerID finger = 0;
    int x = 0, y = 0;
} g_slots[kSlots];

void touch(int slot, bool pressed, int x, int y) {
    int32_t tev[4] = {slot, pressed ? 1 : 0, x, y};
    queue_event(DEV_POINTER, 2, tev, sizeof(tev));
    if (slot == 0) {
        int32_t bev[4] = {0, pressed ? 1 : 0, x, y};
        queue_event(DEV_POINTER, 0, bev, sizeof(bev));
    }
}

void touch_motion(int slot, int x, int y) {
    int32_t tev[3] = {slot, x, y};
    queue_event(DEV_POINTER, 3, tev, sizeof(tev));
    if (slot == 0) {
        int32_t mev[2] = {x, y};
        queue_event(DEV_POINTER, 1, mev, sizeof(mev));
    }
}

int slot_for(SDL_FingerID id, bool allocate) {
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].used && g_slots[i].finger == id) return i;
    if (!allocate) return -1;
    for (int i = 0; i < kSlots; ++i)
        if (!g_slots[i].used) {
            g_slots[i].used = true;
            g_slots[i].finger = id;
            return i;
        }
    return -1;
}

// --- API ----------------------------------------------------------------------
int32_t s3eKeyboardGetInt(int32_t prop) {
    switch (prop) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 1;
    case 3: return 0;
    case 4: return g_char_events ? 1 : 0;
    default: set_error(DEV_KEYBOARD, 1); return -1;
    }
}
HLE_REGISTER(s3eKeyboardGetInt);

int32_t s3eKeyboardSetInt(int32_t prop, int32_t value) {
    if (prop == 4 && (value == 0 || value == 1)) {
        g_char_events = value;
        if (value) SDL_StartTextInput();
        else SDL_StopTextInput();
        return S3E_RESULT_SUCCESS;
    }
    set_error(DEV_KEYBOARD, 1);
    return S3E_RESULT_ERROR;
}
HLE_REGISTER(s3eKeyboardSetInt);

int32_t s3eKeyboardGetState(int32_t key) {
    if (key < 0 || key >= kKeys) {
        set_error(DEV_KEYBOARD, 1);
        return 0;
    }
    if (key >= 200) {
        int32_t state = 0;
        for (auto &a : kAbstract)
            if (a.first == key) state |= g_published[a.second];
        return state;
    }
    return g_published[key];
}
HLE_REGISTER(s3eKeyboardGetState);

int32_t s3eKeyboardUpdate() {
    memcpy(g_published, g_live, sizeof(g_live));
    for (uint8_t &s : g_live) s &= ~(PRESSED | RELEASED);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eKeyboardUpdate);

int32_t s3eKeyboardRegister(int32_t cbid, addr_t fn, addr_t user) {
    return register_callback(DEV_KEYBOARD, 2, cbid, fn, user);
}
HLE_REGISTER(s3eKeyboardRegister);

int32_t s3eKeyboardGetDisplayName(char *dst, int32_t key, uint8_t terminate) {
    std::string name;
    if (key >= 23 && key <= 48) name = std::string(1, static_cast<char>('A' + key - 23));
    else if (key >= 13 && key <= 22) name = std::string(1, static_cast<char>('0' + key - 13));
    else {
        static const std::map<int, const char *> names = {
            {1, "Esc"}, {2, "Tab"}, {3, "Bspc"}, {4, "CR"}, {5, "Shift"}, {6, "Ctrl"}, {8, "Spc"}, {9, "Left"},
            {10, "Up"}, {11, "Right"}, {12, "Down"}, {72, "RSK"}, {73, "LSK"}, {78, "Ok"}, {126, "Back"},
            {127, "Menu"}};
        auto it = names.find(key);
        name = it != names.end() ? it->second : "###";
    }
    if (dst) {
        memcpy(dst, name.data(), name.size());
        if (terminate) dst[name.size()] = 0;
    }
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eKeyboardGetDisplayName);

int32_t s3ePointerGetInt(int32_t prop) {
    switch (prop) {
    case 0: return 1;
    case 1: return 0;
    case 2: return 2;
    case 3: return 2;
    case 4: return 1;
    default: set_error(DEV_POINTER, 1); return -1;
    }
}
HLE_REGISTER(s3ePointerGetInt);

int32_t s3ePointerRegister(int32_t cbid, addr_t fn, addr_t user) {
    return register_callback(DEV_POINTER, 4, cbid, fn, user);
}
HLE_REGISTER(s3ePointerRegister);

int32_t s3ePointerUnRegister(int32_t cbid, addr_t fn) { return unregister_callback(DEV_POINTER, 4, cbid, fn); }
HLE_REGISTER(s3ePointerUnRegister);

// --- accelerometer ------------------------------------------------------------
bool g_accel_started = false;

int32_t s3eAccelerometerStart() {
    if (!g_accel_started) g_accel_started = sensors::start();
    return g_accel_started ? S3E_RESULT_SUCCESS : S3E_RESULT_ERROR;
}
HLE_REGISTER(s3eAccelerometerStart);

void s3eAccelerometerStop() {
    g_accel_started = false;
    sensors::stop();
}
HLE_REGISTER(s3eAccelerometerStop);

int32_t s3eAccelerometerGetX() { return g_accel_started ? sensors::x() : 0; }
int32_t s3eAccelerometerGetY() { return g_accel_started ? sensors::y() : 0; }
int32_t s3eAccelerometerGetZ() { return g_accel_started ? sensors::z() : 0; }
HLE_REGISTER(s3eAccelerometerGetX);
HLE_REGISTER(s3eAccelerometerGetY);
HLE_REGISTER(s3eAccelerometerGetZ);

int32_t s3eAccelerometerGetInt(int32_t prop) {
    if (prop == 0) return sensors::available() ? 1 : 0;
    set_error(DEV_ACCEL, 1);
    return -1;
}
HLE_REGISTER(s3eAccelerometerGetInt);
}  // namespace

namespace input {
void init() {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_StopTextInput();
}

void handle_event(const SDL_Event &ev) {
    switch (ev.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (!ev.key.repeat) key_event(s3e_key(ev.key.keysym.sym), ev.type == SDL_KEYDOWN);
        break;
    case SDL_TEXTINPUT:
        if (g_char_events && ev.text.text[0] && !(ev.text.text[0] & 0x80)) {
            uint16_t ch = static_cast<uint8_t>(ev.text.text[0]);
            queue_event(DEV_KEYBOARD, 1, &ch, sizeof(ch));
        }
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION: {
        int x, y;
        display::window_to_surface(ev.tfinger.x, ev.tfinger.y, x, y);
        if (ev.type == SDL_FINGERDOWN) {
            int s = slot_for(ev.tfinger.fingerId, true);
            if (s < 0) break;
            g_slots[s].x = x;
            g_slots[s].y = y;
            touch(s, true, x, y);
        } else if (ev.type == SDL_FINGERUP) {
            int s = slot_for(ev.tfinger.fingerId, false);
            if (s < 0) break;
            touch(s, false, x, y);
            g_slots[s].used = false;
        } else {
            int s = slot_for(ev.tfinger.fingerId, false);
            if (s < 0 || (g_slots[s].x == x && g_slots[s].y == y)) break;
            g_slots[s].x = x;
            g_slots[s].y = y;
            touch_motion(s, x, y);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (ev.button.which != SDL_TOUCH_MOUSEID && ev.button.button == SDL_BUTTON_LEFT) {
            int w, h;
            SDL_GetWindowSize(SDL_GetWindowFromID(ev.button.windowID), &w, &h);
            int x, y;
            display::window_to_surface(float(ev.button.x) / w, float(ev.button.y) / h, x, y);
            touch(0, ev.type == SDL_MOUSEBUTTONDOWN, x, y);
        }
        break;
    case SDL_MOUSEMOTION:
        if (ev.motion.which != SDL_TOUCH_MOUSEID && (ev.motion.state & SDL_BUTTON_LMASK)) {
            int w, h;
            SDL_GetWindowSize(SDL_GetWindowFromID(ev.motion.windowID), &w, &h);
            int x, y;
            display::window_to_surface(float(ev.motion.x) / w, float(ev.motion.y) / h, x, y);
            touch_motion(0, x, y);
        }
        break;
    default:
        break;
    }
}

// Debug aid: NFS_INPUT_SCRIPT="ms:tap:x,y;ms:key:code;ms:hold:x,y,duration"
// injects input at the given times (surface coordinates).
void run_script() {
    static const char *script = getenv("NFS_INPUT_SCRIPT");
    if (!script) return;
    struct Step {
        uint64_t at;
        int kind;  // 0 down, 1 up, 2 key down, 3 key up
        int a, b;
    };
    static std::vector<Step> steps;
    static bool parsed = false;
    static size_t next = 0;
    if (!parsed) {
        parsed = true;
        std::string all = script;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(pos, end - pos);
            pos = end + 1;
            unsigned long at;
            char kind[16];
            int a = 0, b = 0, c = 120;
            if (sscanf(item.c_str(), "%lu:%15[a-z]:%d,%d,%d", &at, kind, &a, &b, &c) < 3) continue;
            if (!strcmp(kind, "tap") || !strcmp(kind, "hold")) {
                steps.push_back({at, 0, a, b});
                steps.push_back({at + c, 1, a, b});
            } else if (!strcmp(kind, "key")) {
                steps.push_back({at, 2, a, 0});
                steps.push_back({at + 150, 3, a, 0});
            }
        }
        std::sort(steps.begin(), steps.end(), [](const Step &l, const Step &r) { return l.at < r.at; });
    }
    uint64_t now = now_ms();
    while (next < steps.size() && steps[next].at <= now) {
        const Step &st = steps[next++];
        logf("[script] t=%llu %s %d,%d", static_cast<unsigned long long>(now),
             st.kind == 0 ? "down" : st.kind == 1 ? "up" : st.kind == 2 ? "keydown" : "keyup", st.a, st.b);
        if (st.kind == 0) touch(0, true, st.a, st.b);
        else if (st.kind == 1) touch(0, false, st.a, st.b);
        else key_event(st.a, st.kind == 2);
    }
}

void update() {
    sensors::poll();
    run_script();
}
}  // namespace input
