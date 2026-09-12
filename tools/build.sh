#!/bin/sh
# Sync the source tree into the Sailfish SDK container on the build host,
# cross-compile for aarch64 and copy the binary back.
set -e
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
CONTAINER=${SDK_CONTAINER:-sfossdk52}
TARGET=${SDK_TARGET:-SailfishOS-5.2.0.15-aarch64}
HERE=$(cd "$(dirname "$0")/.." && pwd)

tar -C "$HERE/.." -cf - --exclude=build --exclude=.git "$(basename "$HERE")" |
    ssh "$HOST" "docker exec -i $CONTAINER tar -xf - -C /home/mersdk/nfsloader"

ssh "$HOST" "docker exec $CONTAINER bash -c '
    set -e
    cd ~/nfsloader/$(basename "$HERE")
    mkdir -p build && cd build
    [ -f Makefile ] || sb2 -t $TARGET cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDYNARMIC_DIR=/home/mersdk/nfsloader/dynarmic > cmake.log 2>&1 || { tail -30 cmake.log; exit 1; }
    sb2 -t $TARGET make -j16 harbour-nfsshift 2>&1 | grep -E -A3 \"error|Error|undefined\" | head -60
    test \${PIPESTATUS[0]} -eq 0
'"
mkdir -p "$HERE/build"
ssh "$HOST" "docker exec $CONTAINER cat /home/mersdk/nfsloader/$(basename "$HERE")/build/harbour-nfsshift" > "$HERE/build/harbour-nfsshift.new"
mv "$HERE/build/harbour-nfsshift.new" "$HERE/build/harbour-nfsshift"
chmod +x "$HERE/build/harbour-nfsshift"
ls -la "$HERE/build/harbour-nfsshift"
