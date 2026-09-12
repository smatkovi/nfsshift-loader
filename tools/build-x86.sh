#!/bin/sh
# Native x86_64 build on the build host, used for automated LAN tests
# against the phone (headless: SDL_VIDEODRIVER=offscreen).
set -e
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
HERE=$(cd "$(dirname "$0")/.." && pwd)
tar -C "$HERE/.." -cf - --exclude=build --exclude=.git "$(basename "$HERE")" |
    ssh "$HOST" "mkdir -p /tmp/nfsx86 && tar -xf - -C /tmp/nfsx86"
ssh "$HOST" "cd /tmp/nfsx86 && mkdir -p build && cd build &&
    { [ -f build.ninja ] || cmake ../$(basename "$HERE") -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDYNARMIC_DIR=/tmp/dynarmic > cmake.log 2>&1; } &&
    nice ninja -j12 harbour-nfsshift > ninja.log 2>&1 || { grep -E 'error|FAILED' -A4 ninja.log | head -40; exit 1; }
    ls -la harbour-nfsshift"
