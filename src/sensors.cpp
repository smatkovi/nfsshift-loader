// Accelerometer in s3e units.  Two backends with identical output:
//
//   * default (Sailfish OS, desktop): QtSensors / sensorfw,
//   * __ANDROID__: SDL_Sensor -- Qt5Sensors does not exist on Android.
//
// Both report milli-g in the landscape game frame, X inverted (the game steers
// the other way round otherwise) and Z = -1000 with the screen facing up.
#include "sensors.h"

#include <cstdlib>

#include "config.h"
#include "display.h"
#include "guest.h"

#ifdef __ANDROID__
// ---------------------------------------------------------------------------
// Android: SDL sensor subsystem (ASensorManager underneath).
#include <SDL.h>

namespace sensors {
namespace {
SDL_Sensor *g_acc;
bool g_active;
bool g_complained;
int32_t g_x = 0, g_y = 0, g_z = -1000;

bool init_subsystem() {
    if (SDL_WasInit(SDL_INIT_SENSOR) & SDL_INIT_SENSOR) return true;
    if (SDL_InitSubSystem(SDL_INIT_SENSOR) != 0) {
        logf("[sensors] SDL_InitSubSystem(SENSOR): %s", SDL_GetError());
        return false;
    }
    return true;
}

int find_accel() {
    int n = SDL_NumSensors();
    for (int i = 0; i < n; ++i)
        if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) return i;
    if (!g_complained) {
        logf("[sensors] no accelerometer among %d SDL sensors", n);
        g_complained = true;
    }
    return -1;
}

// SDL's Android backend binds the sensor to the ALooper of the thread that
// opens it and polls that looper in SDL_SensorUpdate(), so opening and polling
// must happen on the same thread -- here the loader's main thread, which also
// runs the guest CPU and the event pump.
bool open_accel() {
    if (g_acc) return true;
    if (!init_subsystem()) return false;
    int index = find_accel();
    if (index < 0) return false;
    g_acc = SDL_SensorOpen(index);
    if (!g_acc) {
        logf("[sensors] SDL_SensorOpen(%d): %s", index, SDL_GetError());
        return false;
    }
    logf("[sensors] accelerometer: %s", SDL_SensorGetDeviceName(index));
    return true;
}

// Android reports sensor axes in the device's natural (portrait on a phone)
// frame and never turns them with the screen, so we rotate by the angle the
// picture is rotated by.  SDLActivity.getCurrentOrientation() passes
// Display.getRotation() straight through (ROTATION_90 ->
// SDL_ORIENTATION_LANDSCAPE), which is exactly that angle -- and therefore
// also right on devices whose natural orientation is landscape.
int screen_rotation() {
    switch (SDL_GetDisplayOrientation(0)) {
    case SDL_ORIENTATION_PORTRAIT: return 0;
    case SDL_ORIENTATION_LANDSCAPE: return 90;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED: return 180;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return 270;
    default: return 90;  // sensorLandscape, orientation not reported (yet)
    }
}
}  // namespace

bool disabled() { return getenv("NFS_NO_SENSORS") != nullptr; }

bool available() {
    if (disabled()) return false;
    if (g_acc) return true;
    if (!init_subsystem()) return false;
    return find_accel() >= 0;
}

bool start() {
    if (disabled()) return false;
    g_active = open_accel();
    logf("[sensors] accelerometer %s", g_active ? "started" : "failed to start");
    return g_active;
}

void stop() {
    if (g_acc) {
        SDL_SensorClose(g_acc);
        g_acc = nullptr;
    }
    g_active = false;
}

void poll() {
    if (!g_active || !g_acc) return;
    // SDL_PollEvent() pumps the sensors too, but the loader also calls poll()
    // from yields that do not drain the event queue.
    SDL_SensorUpdate();
    float v[3] = {0, 0, 0};
    if (SDL_SensorGetData(g_acc, v, 3) != 0) return;
    // SDL/ASensor, like Qt: m/s^2 including gravity, device axes, +z out of
    // the screen, +9.8 lying face up.  s3e: milli-g, Z = -1000 face up, X/Y in
    // the landscape surface orientation.
    const double k = -1000.0 / 9.80665;
    double ax = v[0], ay = v[1], az = v[2];
    double sx, sy;
    switch ((screen_rotation() + (display::flipped() ? 180 : 0)) % 360) {
    case 90:  // same mapping as the Sailfish (rotated, not flipped) case
        sx = ay;
        sy = -ax;
        break;
    case 180:
        sx = -ax;
        sy = -ay;
        break;
    case 270:
        sx = -ay;
        sy = ax;
        break;
    default:
        sx = ax;
        sy = ay;
        break;
    }
    // X is negated relative to the plain rotation: verified on device that
    // steering was mirrored otherwise.
    g_x = -static_cast<int32_t>(sx * k) * (config::get_int("s3e", "AccelFlipX", 0) ? -1 : 1);
    g_y = static_cast<int32_t>(sy * k) * (config::get_int("s3e", "AccelFlipY", 0) ? -1 : 1);
    g_z = static_cast<int32_t>(az * k);
}

int32_t x() { return g_x; }
int32_t y() { return g_y; }
int32_t z() { return g_z; }
}  // namespace sensors

#else
// ---------------------------------------------------------------------------
// Sailfish OS / desktop: QtSensors (sensorfw).  Unchanged.
#include <QAccelerometer>
#include <QCoreApplication>

namespace sensors {
namespace {
QCoreApplication *g_app;
QAccelerometer *g_acc;
bool g_active;
int32_t g_x = 0, g_y = 0, g_z = -1000;

void ensure() {
    if (g_acc) return;
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "harbour-nfsshift";
        static char *argv[] = {name, nullptr};
        g_app = new QCoreApplication(argc, argv);
    }
    g_acc = new QAccelerometer();
    g_acc->setAccelerationMode(QAccelerometer::Combined);
}
}  // namespace

bool disabled() { return getenv("NFS_NO_SENSORS") != nullptr; }

bool available() {
    if (disabled()) return false;
    ensure();
    return g_acc->connectToBackend();
}

bool start() {
    if (disabled()) return false;
    ensure();
    if (!g_acc->connectToBackend()) {
        logf("[sensors] no accelerometer backend");
        return false;
    }
    g_acc->setDataRate(50);
    g_active = g_acc->start();
    logf("[sensors] accelerometer %s", g_active ? "started" : "failed to start");
    return g_active;
}

void stop() {
    if (g_acc) g_acc->stop();
    g_active = false;
}

void poll() {
    if (!g_active) return;
    QCoreApplication::processEvents();
    QAccelerometerReading *r = g_acc->reading();
    if (!r) return;
    // Qt: device (portrait) axes in m/s^2, +z out of the screen, +9.8 when
    // lying face up. s3e: milli-g with Z = -1000 face up, X/Y in the
    // landscape surface orientation.
    const double k = -1000.0 / 9.80665;
    double qx = r->x(), qy = r->y(), qz = r->z();
    double sx, sy;
    if (!display::rotated()) {
        sx = qx;
        sy = qy;
    } else if (!display::flipped()) {
        sx = qy;
        sy = -qx;
    } else {
        sx = -qy;
        sy = qx;
    }
    // X is negated relative to the plain rotation: verified on device that
    // steering was mirrored otherwise.
    g_x = -static_cast<int32_t>(sx * k) * (config::get_int("s3e", "AccelFlipX", 0) ? -1 : 1);
    g_y = static_cast<int32_t>(sy * k) * (config::get_int("s3e", "AccelFlipY", 0) ? -1 : 1);
    g_z = static_cast<int32_t>(qz * k);
}

int32_t x() { return g_x; }
int32_t y() { return g_y; }
int32_t z() { return g_z; }
}  // namespace sensors
#endif
