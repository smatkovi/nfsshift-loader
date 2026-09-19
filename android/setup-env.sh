#!/bin/bash
# =====================================================================
#  Android build environment for the NFS Shift loader (arm64-v8a),
#  without Gradle, without Qt, without Android Studio.
#
#  RUNS ON THE BUILD HOST (sebastian@192.168.1.21), not on the phone.
#  android/build.sh calls it over ssh when something is missing.
#
#  Everything lives under /tmp/android -- which is tmpfs on that machine
#  and therefore gone after a reboot; the root partition is full, so do
#  not put it anywhere else.  Running this script again rebuilds it
#  (~730 MB of downloads, a few minutes on 16 cores).
#
#  Derived from scratchpad/wf1/android-env/build-test-apk.sh (the proven
#  version, including the Boost workaround); the test app was dropped and
#  liblzma was added.
#
#     bash setup-env.sh            # everything that is missing
#     bash setup-env.sh fetch      # only the downloads
#     bash setup-env.sh sdl        # only SDL2
#     bash setup-env.sh dynarmic   # only dynarmic
#     bash setup-env.sh lzma       # only liblzma
#     bash setup-env.sh check      # only report what is present
# =====================================================================
set -euo pipefail

ROOT=${ANDROID_ROOT:-/tmp/android}
NDK_VER=r27c                      # = Pkg.Revision 27.2.12479018
SDL_VER=2.30.12
XZ_VER=5.8.1
API=${ANDROID_API:-26}            # minSdk 26 (Android 8.0)
ABI=${ANDROID_ABI:-arm64-v8a}
# Parallelism; the build host is a shared workstation, so JOBS=6 is polite.
JOBS=${JOBS:-$(nproc)}

NDK=$ROOT/android-ndk-$NDK_VER
SDK=$ROOT/sdk
ANDROID_JAR=$SDK/platforms/android-35/android.jar
PREFIX=$ROOT/prefix/$ABI
SDLSRC=$ROOT/SDL2-$SDL_VER
XZSRC=$ROOT/xz-$XZ_VER
DYNARMIC_SRC=${DYNARMIC_SRC:-/tmp/dynarmic}
DYNARMIC_REV=e77b1ba             # azahar-emu/dynarmic, the revision in use

STEP=${1:-all}

# ---------------------------------------------------------------------
# 1. Downloads (official sources only, checksums verified)
# ---------------------------------------------------------------------
fetch() {
  mkdir -p "$ROOT"
  cd "$ROOT"

  # --- NDK r27c -----------------------------------------------------
  # sha1 from https://dl.google.com/android/repository/repository2-3.xml
  if [ ! -d "$NDK" ]; then
    curl -sSL -o ndk.zip \
      "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip"
    echo "090e8083a715fdb1a3e402d0763c388abb03fb4e  ndk.zip" | sha1sum -c -
    unzip -q ndk.zip && rm -f ndk.zip
  fi

  # --- Platform android-35 (android.jar) ----------------------------
  if [ ! -f "$ANDROID_JAR" ]; then
    curl -sSL -o platform-35.zip \
      "https://dl.google.com/android/repository/platform-35_r01.zip"
    mkdir -p "$SDK/platforms" tmp-plat
    unzip -q platform-35.zip -d tmp-plat
    mv tmp-plat/android-35 "$SDK/platforms/"
    rmdir tmp-plat; rm -f platform-35.zip
  fi
  # build-tools/platform-tools from the system installation
  [ -e "$SDK/build-tools"    ] || ln -sfn /opt/android-sdk/build-tools    "$SDK/build-tools"
  [ -e "$SDK/platform-tools" ] || ln -sfn /opt/android-sdk/platform-tools "$SDK/platform-tools"

  # --- SDL2 ---------------------------------------------------------
  if [ ! -d "$SDLSRC" ]; then
    curl -sSL -o sdl2.tar.gz \
      "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
    echo "ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea  sdl2.tar.gz" | sha256sum -c -
    tar xzf sdl2.tar.gz && rm -f sdl2.tar.gz
  fi

  # --- xz / liblzma -------------------------------------------------
  # The NDK ships zlib but no liblzma; hle_compression.cpp and
  # s3e_image.cpp need lzma_alone_decoder().  The checksum below was
  # computed here and cross-checked against the second mirror
  # (tukaani.org and github.com deliver byte-identical tarballs).
  if [ ! -d "$XZSRC" ]; then
    curl -sSL -o xz.tar.gz \
      "https://github.com/tukaani-project/xz/releases/download/v$XZ_VER/xz-$XZ_VER.tar.gz"
    echo "507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543  xz.tar.gz" | sha256sum -c -
    tar xzf xz.tar.gz && rm -f xz.tar.gz
  fi

  # --- dynarmic -----------------------------------------------------
  if [ ! -d "$DYNARMIC_SRC" ]; then
    git clone https://github.com/azahar-emu/dynarmic.git "$DYNARMIC_SRC"
    git -C "$DYNARMIC_SRC" checkout "$DYNARMIC_REV"
  fi
}

# ---------------------------------------------------------------------
# 2. SDL2
# ---------------------------------------------------------------------
build_sdl() {
  # max-page-size=16384: Android 15 devices may use 16 KB pages and then
  # refuse to load a 4 KB aligned .so.  NDK r27 only adds the flag with
  # ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON, which also changes compile flags,
  # so pass just the linker flag.
  cmake -S "$SDLSRC" -B "$ROOT/build-sdl2-$ABI" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    -DSDL_STATIC=OFF -DSDL_SHARED=ON
  cmake --build "$ROOT/build-sdl2-$ABI" -j"$JOBS"
  cmake --install "$ROOT/build-sdl2-$ABI"
  file "$PREFIX/lib/libSDL2.so"
}

# ---------------------------------------------------------------------
# 3. liblzma (static)
# ---------------------------------------------------------------------
build_lzma() {
  cmake -S "$XZSRC" -B "$ROOT/build-lzma-$ABI" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_NLS=OFF -DXZ_NLS=OFF -DXZ_DOC=OFF \
    -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
    -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF \
    -DXZ_TOOL_SYMLINKS=OFF -DXZ_TOOL_SYMLINKS_LZMA=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
  cmake --build "$ROOT/build-lzma-$ABI" -j"$JOBS"
  cmake --install "$ROOT/build-lzma-$ABI" > /dev/null
  ls -la "$PREFIX/lib/liblzma.a" "$PREFIX/include/lzma.h"
}

# ---------------------------------------------------------------------
# 4. dynarmic
# ---------------------------------------------------------------------
# WORKAROUND Boost:
#   dynarmic does find_package(Boost 1.57 REQUIRED) and links Boost::boost
#   (header only: boost/variant.hpp in src/dynarmic/ir/terminal.h).
#   (a) CMake >= 4.0 deprecated FindBoost (CMP0167).  The FindBoost.cmake that
#       still ships sets INTERFACE_INCLUDE_DIRECTORIES only on Boost::headers,
#       not on Boost::boost -> boost/variant.hpp not found.
#   (b) The real /usr/lib/cmake/Boost-*/BoostConfig.cmake fails when cross
#       compiling (boost_find_component does not find the components because
#       the NDK toolchain restricts CMAKE_FIND_ROOT_PATH_MODE_*).
#   (c) CMake always strips "/usr/include" from the -I/-isystem flags, so a
#       shim pointing at /usr/include is not enough; it needs its own include
#       root with a symlink.
boost_shim() {
  mkdir -p "$ROOT/boost-inc" "$ROOT/cmake-shims/Boost"
  ln -sfn /usr/include/boost "$ROOT/boost-inc/boost"
  cat > "$ROOT/cmake-shims/Boost/BoostConfig.cmake" <<EOF
set(Boost_VERSION 1.91.0)
set(Boost_INCLUDE_DIR  "$ROOT/boost-inc")
set(Boost_INCLUDE_DIRS "$ROOT/boost-inc")
set(Boost_FOUND TRUE)
foreach(t Boost::headers Boost::boost)
  if(NOT TARGET \${t})
    add_library(\${t} INTERFACE IMPORTED)
    set_target_properties(\${t} PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "$ROOT/boost-inc")
  endif()
endforeach()
EOF
  cat > "$ROOT/cmake-shims/Boost/BoostConfigVersion.cmake" <<'EOF'
set(PACKAGE_VERSION 1.91.0)
if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
EOF
}

build_dynarmic() {
  boost_shim
  cmake -S "$DYNARMIC_SRC" -B "$ROOT/build-dynarmic-$ABI" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
    -DBoost_DIR="$ROOT/cmake-shims/Boost" \
    -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON \
    -DDYNARMIC_TESTS=OFF -DBUILD_TESTING=OFF \
    -DDYNARMIC_FRONTENDS=A32 \
    -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF
  cmake --build "$ROOT/build-dynarmic-$ABI" -j"$JOBS"
  ls -la "$ROOT/build-dynarmic-$ABI/src/dynarmic/libdynarmic.a"
}

# ---------------------------------------------------------------------
check() {
  local missing=0
  report() { if [ -e "$2" ]; then echo "ok      $1"; else echo "MISSING $1 ($2)"; missing=1; fi; }
  report "NDK $NDK_VER"        "$NDK/build/cmake/android.toolchain.cmake"
  report "android.jar"         "$ANDROID_JAR"
  report "build-tools"         "$SDK/build-tools"
  report "SDL2 $SDL_VER"       "$PREFIX/lib/libSDL2.so"
  report "liblzma $XZ_VER"     "$PREFIX/lib/liblzma.a"
  report "dynarmic"            "$ROOT/build-dynarmic-$ABI/src/dynarmic/libdynarmic.a"
  return $missing
}

case "$STEP" in
  all)
    fetch
    [ -f "$PREFIX/lib/libSDL2.so" ] || build_sdl
    [ -f "$PREFIX/lib/liblzma.a" ] || build_lzma
    [ -f "$ROOT/build-dynarmic-$ABI/src/dynarmic/libdynarmic.a" ] || build_dynarmic
    check
    ;;
  fetch)    fetch ;;
  sdl)      build_sdl ;;
  lzma)     build_lzma ;;
  dynarmic) build_dynarmic ;;
  check)    check ;;
  *) echo "unknown step: $STEP" >&2; exit 1 ;;
esac
