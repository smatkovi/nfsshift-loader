// harbour-nfsshift: runs the Marmalade (s3e) build of Need for Speed Shift on
// Sailfish OS by executing its 32-bit ARM code in a JIT and reimplementing the
// Marmalade runtime on top of SDL2 and OpenGL ES 2.
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "config.h"
#include "cpu.h"
#include "display.h"
#include "mp_host.h"
#include "runtime.h"
#include "s3e_image.h"

// Keep the start of the static TLS block free: bionic-based GL drivers loaded
// through libhybris use the first thread pointer slots for themselves.
[[maybe_unused]] static thread_local char tls_reserved_for_bionic[256] = {1};

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--data DIR] [--user DIR] [--height N] [--flip] [--no-rotate] [--trace]\n"
            "  --data DIR   directory containing NFSShift.s3e and res.dz\n"
            "  --user DIR   writable directory for save games\n"
            "  --height N   render height of the landscape surface (default: screen)\n",
            argv0);
}

// Where the game data lives, in order of preference. Keeping the copy in the
// user's home first means the package never has to ship EA's files.
static std::string find_data_dir(const std::string &home) {
    const std::string candidates[] = {
        home + "/.local/share/harbour-nfsshift/data",
        "/usr/share/harbour-nfsshift/data",
    };
    for (const std::string &d : candidates)
        if (access((d + "/NFSShift.s3e").c_str(), R_OK) == 0) return d;
    return candidates[1];
}

int main(int argc, char **argv) {
    std::string data_dir;
    const char *home = getenv("HOME");
    std::string user_dir = std::string(home ? home : "/tmp") + "/.local/share/harbour-nfsshift/save";
    display::Options dopt;
    dopt.surface_height = 540;
    bool trace = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(argv[0]);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--data") data_dir = next();
        else if (a == "--user") user_dir = next();
        else if (a == "--height") dopt.surface_height = atoi(next().c_str());
        else if (a == "--flip") dopt.flip = true;
        else if (a == "--no-rotate") dopt.rotate = false;
        else if (a == "--trace") trace = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (data_dir.empty()) data_dir = find_data_dir(home ? home : "/tmp");
    mkdir((std::string(home ? home : "/tmp") + "/.local/share").c_str(), 0755);
    mkdir((std::string(home ? home : "/tmp") + "/.local/share/harbour-nfsshift").c_str(), 0755);
    mkdir(user_dir.c_str(), 0755);

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
    // Patch before the first guest instruction runs: the JIT cache is still
    // empty, so no invalidation is needed.
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
