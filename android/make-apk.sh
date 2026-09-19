#!/bin/bash
# =====================================================================
#  Builds the signed NFS Shift loader APK (arm64-v8a) without Gradle:
#  CMake/NDK -> libmain.so, javac -> d8 -> aapt2 -> zip -> zipalign ->
#  apksigner.
#
#  RUNS ON THE BUILD HOST.  android/build.sh copies the source tree over
#  and calls this script; call it directly only if the tree is already
#  there.  The script expects to sit in <tree>/android/.
#
#     bash make-apk.sh           # native code + APK
#
#  GAMEDATA=<dir> puts the game data from that directory into the APK
#  (assets/data/ plus a filelist.txt); android_main.cpp unpacks it on the first
#  start.  That is the private full build -- it contains material of Electronic
#  Arts and must not be passed on.
#     bash make-apk.sh native    # only libmain.so
#     bash make-apk.sh apk       # only the APK (uses the last libmain.so)
#     bash make-apk.sh verify    # only aapt2 dump badging + apksigner verify
# =====================================================================
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)                 # the nfsshift-sfos tree
ROOT=${ANDROID_ROOT:-/tmp/android}
NDK_VER=r27c
SDL_VER=2.30.12
API=${ANDROID_API:-26}
TARGET_API=${ANDROID_TARGET_API:-35}
ABI=${ANDROID_ABI:-arm64-v8a}

NDK=$ROOT/android-ndk-$NDK_VER
SDK=$ROOT/sdk
PLATFORM=$SDK/platforms/android-35
ANDROID_JAR=$PLATFORM/android.jar
BT=${BUILD_TOOLS:-/opt/android-sdk/build-tools/36.1.0}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
PREFIX=$ROOT/prefix/$ABI
SDLSRC=$ROOT/SDL2-$SDL_VER
APP=$HERE/app
BUILD=${BUILD_DIR:-$ROOT/build-loader-$ABI}
OUT=${OUT_DIR:-$ROOT/out-loader}
APK=$OUT/nfsshift.apk
# Signing: without KEYSTORE the throwaway debug key under $ROOT is used (and
# created if missing). packaging/android/sign-apk.sh keeps the real key in
# ~/.config/nfsshift/nfsshift-release.keystore -- pass that as KEYSTORE plus
# KS_ALIAS/NFSSHIFT_KS_PASS to build a release-signed APK straight away.
# Careful: a different key means uninstall + install on every device, an
# upgrade over the old signature is impossible.
KS=${KEYSTORE:-$ROOT/debug.keystore}
KS_ALIAS=${KS_ALIAS:-androiddebugkey}
export NFSSHIFT_KS_PASS=${NFSSHIFT_KS_PASS:-android}
VERSION_CODE=${VERSION_CODE:-1}
VERSION_NAME=${VERSION_NAME:-1.0}
# Parallelism of the native build.  The build host is a workstation somebody
# else is using; JOBS=6 (and nice, see build.sh) keeps it usable.
JOBS=${JOBS:-$(nproc)}

STEP=${1:-all}

# ---------------------------------------------------------------------
# 1. native: libmain.so via android/CMakeLists.txt
# ---------------------------------------------------------------------
build_native() {
  cmake -S "$HERE" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release} \
    -DANDROID_DEPS_ROOT="$ROOT" \
    -DDYNARMIC_DIR="${DYNARMIC_SRC:-/tmp/dynarmic}" \
    -DNFS_MP_AUTO=${NFS_MP_AUTO:-ON} \
    -DNFS_MP_SOURCES="${NFS_MP_SOURCES:-}" \
    ${CMAKE_EXTRA:-}
  cmake --build "$BUILD" -j"$JOBS"
  ls -la "$BUILD/libmain.so"
}

# ---------------------------------------------------------------------
# 2. Java -> classes.dex
# ---------------------------------------------------------------------
build_dex() {
  rm -rf "$OUT/classes" "$OUT/dex"
  mkdir -p "$OUT/classes" "$OUT/dex"
  # SDLActivity & Co. from the SDL2 sources plus our own subclass.
  find "$SDLSRC/android-project/app/src/main/java" -name '*.java' > "$OUT/java.list"
  find "$APP/java" -name '*.java' >> "$OUT/java.list"
  # android.jar alone is not enough as -bootclasspath: it contains no
  # java/lang/invoke/LambdaMetafactory, which javac needs for the lambdas in
  # SDLAudioManager.java ("Unable to find method metafactory").  The Android
  # Gradle Plugin builds a JDK system image from it; appending
  # core-for-system-modules.jar from the platform does the same job.
  javac -source 8 -target 8 \
        -bootclasspath "$ANDROID_JAR:$PLATFORM/core-for-system-modules.jar" \
        -classpath "$ANDROID_JAR" \
        -encoding UTF-8 -nowarn \
        -d "$OUT/classes" @"$OUT/java.list"
  "$BT/d8" --lib "$ANDROID_JAR" --min-api $API --release \
           --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
  ls -la "$OUT/dex/classes.dex"
}

# ---------------------------------------------------------------------
# 3. resources + manifest -> APK skeleton
# ---------------------------------------------------------------------
build_resources() {
  # --replace-version: ohne das traegt aapt2 --version-code/--version-name nur
  # ein, wenn im Manifest keins steht -- dort stehen aber 1 und "1.0" fest, und
  # das Vollpaket soll sich davon unterscheiden.
  rm -f "$OUT/res.zip" "$OUT/base.apk"
  "$BT/aapt2" compile --dir "$APP/res" -o "$OUT/res.zip"
  "$BT/aapt2" link \
    -I "$ANDROID_JAR" \
    --manifest "$APP/AndroidManifest.xml" \
    --min-sdk-version $API --target-sdk-version $TARGET_API \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" --replace-version \
    -o "$OUT/base.apk" \
    "$OUT/res.zip"
}

# ---------------------------------------------------------------------
# 4. pack, align, sign
# ---------------------------------------------------------------------
build_apk() {
  mkdir -p "$OUT/stage/lib/$ABI" "$OUT/symbols"
  build_dex
  build_resources

  cp "$BUILD/libmain.so" "$OUT/symbols/libmain.so"       # keep the symbols
  cp "$BUILD/libmain.so" "$OUT/stage/lib/$ABI/libmain.so"
  cp "$PREFIX/lib/libSDL2.so" "$OUT/stage/lib/$ABI/libSDL2.so"
  "$TC/bin/llvm-strip" "$OUT/stage/lib/$ABI/libmain.so" "$OUT/stage/lib/$ABI/libSDL2.so"
  # SDLActivity dlopen()s libmain.so and dlsym()s "SDL_main" -- if the symbol
  # is missing the app dies with "Failed to find SDL_main".
  "$TC/bin/llvm-nm" -D --defined-only "$OUT/stage/lib/$ABI/libmain.so" | grep ' SDL_main$'

  cp "$OUT/dex/classes.dex" "$OUT/stage/"

  # Full build: the game data rides along in assets/data/.  The data directory
  # is flat (26 files), so a flat list is enough -- the unpacker in
  # android_main.cpp creates no subdirectories.
  rm -rf "$OUT/stage/assets"
  if [ -n "${GAMEDATA:-}" ]; then
    [ -f "$GAMEDATA/NFSShift.s3e" ] || { echo "no NFSShift.s3e in $GAMEDATA" >&2; exit 1; }
    mkdir -p "$OUT/stage/assets/data"
    cp -a "$GAMEDATA"/. "$OUT/stage/assets/data/"
    ( cd "$OUT/stage/assets/data" &&
      find . -maxdepth 1 -type f ! -name filelist.txt -printf '%s %P\n' |
        LC_ALL=C sort -k2 > filelist.txt )
    echo "game data: $(du -sh "$OUT/stage/assets/data" | cut -f1) in \
$(grep -c '' "$OUT/stage/assets/data/filelist.txt") files"
  fi

  rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk" "$APK"
  cp "$OUT/base.apk" "$OUT/unsigned.apk"
  ( cd "$OUT/stage" && zip -q -r "$OUT/unsigned.apk" classes.dex lib )
  if [ -d "$OUT/stage/assets" ]; then
    # -0: stored, not deflated.  res.dz and the MP3s are compressed already,
    # deflating them again only costs build time, and a stored asset can be
    # read straight out of the mapped APK.
    ( cd "$OUT/stage" && zip -q -0 -r "$OUT/unsigned.apk" assets )
  fi
  # -p 4: page align the .so entries (harmless, extractNativeLibs is true).
  "$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

  if [ ! -f "$KS" ]; then
    # A keystore that was asked for by name is never invented here: that would
    # silently sign with a key nobody has, and the next update would not install.
    [ -n "${KEYSTORE:-}" ] && { echo "keystore $KS does not exist" >&2; exit 1; }
    keytool -genkeypair -v -keystore "$KS" -storepass "$NFSSHIFT_KS_PASS" \
      -alias "$KS_ALIAS" -keypass "$NFSSHIFT_KS_PASS" \
      -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=Android Debug,O=Android,C=US"
  fi
  echo "signing with $KS (alias $KS_ALIAS)"
  "$BT/apksigner" sign --ks "$KS" --ks-pass env:NFSSHIFT_KS_PASS \
    --ks-key-alias "$KS_ALIAS" --key-pass env:NFSSHIFT_KS_PASS \
    --min-sdk-version $API \
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
    --out "$APK" "$OUT/aligned.apk"
  ls -la "$APK"
}

# ---------------------------------------------------------------------
# 5. static checks (there is no Android device on this machine)
# ---------------------------------------------------------------------
verify() {
  echo "=== aapt2 dump badging ==="
  "$BT/aapt2" dump badging "$APK"
  echo "=== contents ==="
  unzip -l "$APK"
  echo "=== apksigner verify ==="
  # With the APK's own minSdk 26 apksigner does not evaluate v1 any more;
  # --min-sdk-version 21 shows that the JAR signature is there as well.
  "$BT/apksigner" verify --min-sdk-version 21 -v "$APK"
  echo "=== sha256 ==="
  sha256sum "$APK"
}

case "$STEP" in
  all)    build_native; build_apk; verify ;;
  native) build_native ;;
  apk)    build_apk; verify ;;
  verify) verify ;;
  *) echo "unknown step: $STEP" >&2; exit 1 ;;
esac
