#!/bin/sh
# Build the Android (arm64-v8a) APK of the loader.
#
# Runs on the phone / workstation, does the work on the build host over ssh --
# same shape as tools/build.sh: copy the source tree over, build there, fetch
# the result back into build/android/.
#
#   android/build.sh            # environment check, native code, APK, checks
#   android/build.sh native     # only libmain.so (fast syntax/link check)
#   android/build.sh apk        # only repack the APK
#   android/build.sh verify     # only aapt2 dump badging + apksigner verify
#
# Environment:
#   BUILD_HOST   ssh target                 (default sebastian@192.168.1.21)
#   REMOTE_DIR   source copy on the host    (default /tmp/nfsandroid)
#   ANDROID_ROOT toolchains/dependencies    (default /tmp/android, tmpfs!)
#   NFS_MP_SOURCES  multiplayer sources to compile in, ";" separated, relative
#                   to the tree root, e.g.
#                   NFS_MP_SOURCES="src/mp/nfsmp.c;src/mp/glue.c;src/mp/patches.c;src/hle_mp.cpp"
#   NFS_MP_AUTO     ON = glob src/mp/*.c + src/hle_mp.cpp instead (default OFF)
#   CMAKE_EXTRA     further cmake arguments
#   GAMEDATA     directory ON THE BUILD HOST whose game data goes into the APK
#                (private full build, e.g. /tmp/nfsx86/data)
#   VERSION_NAME / VERSION_CODE   what aapt2 stamps into the manifest
#   APK_NAME     name of the fetched file under build/android (default
#                nfsshift.apk)
#   JOBS         parallel compiler jobs on the host (default: all its cores)
#   NICE         niceness of everything run on the host (default 0).  The host
#                is somebody's workstation -- JOBS=6 NICE=15 leaves it usable.
set -e
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
REMOTE=${REMOTE_DIR:-/tmp/nfsandroid}
ANDROID_ROOT=${ANDROID_ROOT:-/tmp/android}
HERE=$(cd "$(dirname "$0")/.." && pwd)
NAME=$(basename "$HERE")
STEP=${1:-all}
RNICE=${NICE:+nice -n $NICE}
RJOBS=${JOBS:+JOBS=$JOBS}

# rsync statt einer tar-Pipe: ueber den cloudflared-Tunnel laeuft die
# Uebertragung mit rund 1 MB/s, und rsync schickt nur die Aenderungen.
echo "[1/4] copying $NAME to $HOST:$REMOTE (rsync)"
rsync -a --delete --exclude=build --exclude=.git \
      --rsync-path="mkdir -p '$REMOTE/$NAME' && rsync" \
      "$HERE/" "$HOST:$REMOTE/$NAME/"

# /tmp is tmpfs on the build host: after a reboot the NDK, SDL2, liblzma and
# dynarmic are gone.  setup-env.sh restores whatever is missing.
echo "[2/4] checking the build environment"
ssh "$HOST" "ANDROID_ROOT=$ANDROID_ROOT bash $REMOTE/$NAME/android/setup-env.sh check" ||
    ssh "$HOST" "ANDROID_ROOT=$ANDROID_ROOT $RJOBS \
                 $RNICE bash $REMOTE/$NAME/android/setup-env.sh all"

echo "[3/4] building ($STEP)"
ssh "$HOST" "ANDROID_ROOT=$ANDROID_ROOT $RJOBS \
             ${GAMEDATA:+GAMEDATA='$GAMEDATA'} \
             ${VERSION_NAME:+VERSION_NAME='$VERSION_NAME'} \
             ${VERSION_CODE:+VERSION_CODE='$VERSION_CODE'} \
             ${NFS_MP_SOURCES:+NFS_MP_SOURCES='$NFS_MP_SOURCES'} \
             ${NFS_MP_AUTO:+NFS_MP_AUTO='$NFS_MP_AUTO'} \
             ${CMAKE_EXTRA:+CMAKE_EXTRA='$CMAKE_EXTRA'} \
             $RNICE bash $REMOTE/$NAME/android/make-apk.sh $STEP"

[ "$STEP" = native ] && exit 0

echo "[4/4] fetching the APK"
mkdir -p "$HERE/build/android"
OUT_APK=$HERE/build/android/${APK_NAME:-nfsshift.apk}
# --partial: die Vollpaket-APK ist ueber 100 MB gross; bricht der Tunnel ab,
# setzt der naechste Lauf fort, statt wieder bei null anzufangen.
[ -t 2 ] && RSYNC_PROGRESS=--info=progress2 || RSYNC_PROGRESS=
rsync -a --partial $RSYNC_PROGRESS \
      "$HOST:$ANDROID_ROOT/out-loader/nfsshift.apk" "$OUT_APK"
ls -la "$OUT_APK"
echo
echo "install:   adb install -r $OUT_APK"
echo "game data: adb push <data>/* /sdcard/Android/data/org.nfsshift.loader/files/"
echo "log:       adb logcat -s nfsshift SDL"
