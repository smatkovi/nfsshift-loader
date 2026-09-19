// Android entry point of the loader.
//
// It replaces src/main.cpp (which stays the Sailfish/x86 entry point) because
// three things differ on Android:
//
//   1. The process entry point is Java.  libSDL2.so's
//      Java_org_libsdl_app_SDLActivity_nativeRunMain dlopen()s libmain.so and
//      dlsym()s "SDL_main", so the loader has to export SDL_main instead of
//      main.  (libSDL2main.a is empty on Android -- SDL_android_main.c.o
//      contains no symbols.)
//   2. There are no command line arguments and no HOME.  The game data
//      directory is searched for (getExternalFilesDir, then
//      /sdcard/Download/nfsshift) and the save directory is getFilesDir().
//   3. stdout/stderr of an Android app go to /dev/null, so the loader's logf()
//      output is piped into logcat (tag "nfsshift").
//
// Optional arguments can still be passed for testing; ShiftActivity forwards the
// intent extra "args", e.g.
//   adb shell am start -n org.nfsshift.loader/.ShiftActivity --es args "--height 720"

#include <android/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "config.h"
#include "cpu.h"
#include "display.h"
#include "guest.h"
#include "mp_host.h"
#include "runtime.h"
#include "s3e_image.h"

// guest::dump_low_mappings() (every mapping below 4 GB, i.e. the address space
// the guest is identity mapped into) used to be declared here because guest.h
// did not export it; it does now, so the local copy is gone.

namespace {

constexpr const char *kLogTag = "nfsshift";

// --- logcat bridge ------------------------------------------------------------
// guest.cpp's logf()/fatal() and everything else in the loader write to stderr.
// Android throws that away, so both descriptors are replaced by a pipe that a
// helper thread forwards to logcat, line by line.
int g_log_pipe[2] = {-1, -1};
// Second sink for the same lines: a file in the app's own external directory.
// On a device without adb (and there is no adb on the phone this is developed
// from) logcat is out of reach, and a crash during startup then says nothing at
// all. The file survives the crash and can be fetched over USB.
FILE *g_log_file = nullptr;

void *log_pump(void *) {
    char buf[512];
    size_t fill = 0;
    for (;;) {
        ssize_t n = read(g_log_pipe[0], buf + fill, sizeof(buf) - 1 - fill);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        fill += static_cast<size_t>(n);
        buf[fill] = 0;
        char *line = buf;
        for (;;) {
            char *nl = strchr(line, '\n');
            if (!nl) break;
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, kLogTag, line);
            if (g_log_file) {
                fprintf(g_log_file, "%s\n", line);
                fflush(g_log_file);
            }
            line = nl + 1;
        }
        fill = strlen(line);
        // A line longer than the buffer is flushed as it is.
        if (fill >= sizeof(buf) - 1) {
            __android_log_write(ANDROID_LOG_INFO, kLogTag, line);
            fill = 0;
        } else {
            memmove(buf, line, fill + 1);
        }
    }
    return nullptr;
}

// The external directory is only known once SDL is up, so the file joins the
// bridge a moment later; the lines until then are in logcat only.
void open_log_file() {
    const char *ext = SDL_AndroidGetExternalStoragePath();
    if (!ext || g_log_file) return;
    std::string path = std::string(ext) + "/nfsshift.log";
    g_log_file = fopen(path.c_str(), "w");
    if (g_log_file) logf("[main] log file: %s", path.c_str());
}

void start_log_bridge() {
    if (pipe(g_log_pipe) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(g_log_pipe[1], STDOUT_FILENO);
    dup2(g_log_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, nullptr, log_pump, nullptr) == 0) pthread_detach(t);
}

// --- helpers ------------------------------------------------------------------
bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The loader needs NFSShift.s3e (guest code) next to res.dz and the MP3s.
bool has_game_data(const std::string &dir) {
    if (dir.empty()) return false;
    std::string probe = dir + "/NFSShift.s3e";
    struct stat st;
    if (stat(probe.c_str(), &st) == 0) return S_ISREG(st.st_mode);
    // Scoped storage: with targetSdk >= 29 shared directories such as
    // /sdcard/Download are not readable through the file system without a
    // storage permission; say so instead of silently reporting "not found".
    if (errno == EACCES || errno == EPERM)
        logf("[main] %s: no permission (scoped storage) -- use the app's own "
             "external directory instead",
             probe.c_str());
    return false;
}

void mkdir_p(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        mkdir(path.substr(0, i).c_str(), 0770);
    }
}

// Candidates in the order of the design: the app's own external directory
// first (adb push .../Android/data/org.nfsshift.loader/files/), then the
// shared download folder.
std::vector<std::string> data_candidates() {
    std::vector<std::string> out;
    if (const char *env = SDL_getenv("NFS_DATA_DIR")) out.push_back(env);
    if (const char *ext = SDL_AndroidGetExternalStoragePath()) {
        out.push_back(ext);                             // .../files
        out.push_back(std::string(ext) + "/nfsshift");  // .../files/nfsshift
    }
    if (const char *in = SDL_AndroidGetInternalStoragePath()) {
        out.push_back(std::string(in) + "/data");
    }
    out.push_back("/sdcard/Download/nfsshift");
    out.push_back("/storage/emulated/0/Download/nfsshift");
    return out;
}

void report_missing_data(const std::vector<std::string> &tried) {
    std::string list;
    for (const std::string &d : tried) list += "  " + d + "\n";
    std::string msg =
        "NFSShift.s3e was not found.\n\nCopy NFSShift.s3e, res.dz and the "
        "bgm_*.mp3 files into one of these directories:\n\n" + list +
        "\nadb push <dir>/* /sdcard/Android/data/org.nfsshift.loader/files/";
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", msg.c_str());
    // Needs the video subsystem (SDL's Android message box goes through the
    // activity); ignore the failure, the log line is the real report.
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NFS Shift: no game data", msg.c_str(), nullptr);
}


// --- game data bundled into the APK -------------------------------------------
// The private full build carries the game data in the APK (assets/data/), so
// the loader and its ~99 MB of data travel in one file and nothing has to be
// pushed with adb.  SDL_RWFromFile() reads a relative path straight out of the
// assets, so this needs no JNI of its own.  android/make-apk.sh writes
// assets/data/filelist.txt with one "<size> <name>" per line; a file is only
// unpacked when it is missing or has a different size, so a second start costs
// one stat() per entry and nothing else.

bool copy_asset(const std::string &name, const std::string &dest) {
    SDL_RWops *in = SDL_RWFromFile(("data/" + name).c_str(), "rb");
    if (!in) {
        logf("[data] asset data/%s: %s", name.c_str(), SDL_GetError());
        return false;
    }
    // Unpack under a temporary name: an extraction that is cut short (battery,
    // task killer) must not leave a truncated file that the size check on the
    // next start would have to catch by luck.
    std::string tmp = dest + ".part";
    FILE *out = fopen(tmp.c_str(), "wb");
    if (!out) {
        logf("[data] %s: %s", tmp.c_str(), strerror(errno));
        SDL_RWclose(in);
        return false;
    }
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        size_t n = SDL_RWread(in, buf.data(), 1, buf.size());
        if (n == 0) break;
        if (fwrite(buf.data(), 1, n, out) != n) {
            logf("[data] %s: %s", tmp.c_str(), strerror(errno));
            ok = false;
            break;
        }
    }
    if (fclose(out) != 0) ok = false;
    SDL_RWclose(in);
    if (ok && rename(tmp.c_str(), dest.c_str()) == 0) return true;
    unlink(tmp.c_str());
    return false;
}

// Returns the directory with the unpacked data, or "" when this APK carries
// none (the ordinary build) or the unpacking failed.
std::string extract_bundled_data() {
    SDL_RWops *list = SDL_RWFromFile("data/filelist.txt", "rb");
    if (!list) return std::string();

    std::string text;
    char chunk[4096];
    for (size_t n; (n = SDL_RWread(list, chunk, 1, sizeof chunk)) > 0;) text.append(chunk, n);
    SDL_RWclose(list);

    std::vector<std::pair<long long, std::string>> entries;
    long long total = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t sep = line.find(' ');
        if (line.empty() || sep == std::string::npos) continue;
        long long size = atoll(line.substr(0, sep).c_str());
        std::string name = line.substr(sep + 1);
        // The list is generated, but it ends up in the APK next to the data it
        // describes: a name that walks out of the directory is not unpacked.
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) continue;
        entries.push_back({size, name});
        total += size;
    }
    if (entries.empty()) return std::string();

    const char *ext = SDL_AndroidGetExternalStoragePath();
    if (!ext) {
        logf("[data] no external storage path -- cannot unpack the game data");
        return std::string();
    }
    std::string dir = std::string(ext) + "/nfsshift";
    mkdir_p(dir);
    if (!is_dir(dir)) {
        logf("[data] cannot create %s", dir.c_str());
        return std::string();
    }

    uint32_t t0 = SDL_GetTicks();
    int unpacked = 0;
    long long written = 0;
    for (const auto &e : entries) {
        std::string dest = dir + "/" + e.second;
        struct stat st;
        if (stat(dest.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == e.first) continue;
        if (unpacked == 0)
            logf("[data] unpacking the game data from the APK to %s (%lld MB, "
                 "once, this takes a moment)",
                 dir.c_str(), total / (1024 * 1024));
        if (!copy_asset(e.second, dest)) return std::string();
        ++unpacked;
        written += e.first;
    }
    if (unpacked > 0)
        logf("[data] unpacked %d of %d files (%lld MB) in %.1f s", unpacked,
             static_cast<int>(entries.size()), written / (1024 * 1024),
             (SDL_GetTicks() - t0) / 1000.0);
    else
        logf("[data] game data from the APK already unpacked in %s", dir.c_str());
    return dir;
}

}  // namespace

extern "C" int SDL_main(int argc, char *argv[]) {
    start_log_bridge();
    // The guest's 4 GB, before anything maps guest memory. Reserved as early
    // as possible, although nothing in the process can take guest addresses
    // any more -- the window is ours alone.
    guest::init_address_space();

    std::string data_dir;
    std::string user_dir;
    display::Options dopt;
    // The Android window is already landscape (screenOrientation is
    // sensorLandscape), so the loader must not rotate the blit as it does
    // inside a portrait Sailfish window.
    dopt.rotate = false;
    dopt.surface_height = 540;
    bool trace = false;
    bool dump_maps = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i] ? argv[i] : "";
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--data") data_dir = next();
        else if (a == "--user") user_dir = next();
        else if (a == "--height") dopt.surface_height = atoi(next().c_str());
        else if (a == "--flip") dopt.flip = true;
        else if (a == "--rotate") dopt.rotate = true;
        else if (a == "--no-rotate") dopt.rotate = false;
        else if (a == "--trace") trace = true;
        else if (a == "--dump-maps") dump_maps = true;
        else logf("[main] ignoring unknown argument '%s'", a.c_str());
    }
    if (const char *h = SDL_getenv("NFS_HEIGHT")) dopt.surface_height = atoi(h);
    if (dump_maps) {
        // The guest is identity mapped into the low 4 GB; this shows what an
        // Android app process already has there (ART boot image, Java heap).
        logf("[main] mappings below 4 GB before the first guest mapping:");
        guest::dump_low_mappings();
    }

    // The accelerometer is an SDL sensor here (no Qt/sensorfw); without this
    // hint SDL would additionally expose it as a three-axis phantom gamepad.
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    // Needed before SDL_ShowSimpleMessageBox and before display::init() can
    // query the display mode; display::init() re-uses this subsystem.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        logf("[main] SDL_Init(video): %s", SDL_GetError());
        return 1;
    }

    open_log_file();

    std::vector<std::string> tried = data_candidates();
    if (data_dir.empty()) {
        for (const std::string &d : tried) {
            if (has_game_data(d)) {
                data_dir = d;
                break;
            }
        }
    }
    // Nothing on the device: the full build has the data in the APK itself.
    // (Not when --data was given -- then the wrong directory is the report.)
    if (data_dir.empty()) data_dir = extract_bundled_data();
    if (!has_game_data(data_dir)) {
        report_missing_data(tried);
        return 1;
    }
    logf("[main] data directory: %s", data_dir.c_str());

    if (user_dir.empty()) {
        const char *internal = SDL_AndroidGetInternalStoragePath();
        user_dir = std::string(internal ? internal : "/data/local/tmp") + "/save";
    }
    mkdir_p(user_dir);
    if (!is_dir(user_dir)) {
        logf("[main] cannot create save directory %s", user_dir.c_str());
        return 1;
    }
    logf("[main] save directory: %s", user_dir.c_str());

    runtime::Paths paths{data_dir, user_dir};
    runtime::set_paths(paths);
    hle::set_trace(trace);

    S3eImage image;
    if (!s3e_load(data_dir + "/NFSShift.s3e", image)) return 1;

    config::set_os_name(runtime::kOsName);
    config::add_text(image.config, "embedded");
    config::add_file(data_dir + "/app.icf");
    config::add_file(user_dir + "/app.icf");
    // Only OpenGL ES 2 exists on the device.
    config::set("s3e", "SysGlesVersion", "2");

    if (!display::init(dopt)) return 1;
    runtime::init();
    // Same place as in src/main.cpp: patch before the first guest instruction
    // runs, while the JIT code cache is still empty, so nothing has to be
    // invalidated.  ShiftActivity holds the MulticastLock the UDP discovery
    // needs (see android/app/java/.../ShiftActivity.java).
    mp::init();

    Cpu cpu("main");
    logf("[main] entering game at %#x", image.entry);
    uint32_t rc = cpu.call(image.entry);
    logf("[main] game returned %d", rc);
    mp::shutdown();
    runtime::shutdown();
    display::shutdown();
    return static_cast<int>(rc);
}
