# Android (arm64-v8a) build of the NFS Shift loader.
#
# This is a *standalone* CMake project; it is never pulled in by the top level
# CMakeLists.txt (which targets Sailfish OS and needs Qt5Sensors, pkg-config and
# a plain executable).  It compiles the very same sources from ../src and
# ../third_party, but
#
#   * produces a shared library `libmain.so` exporting `SDL_main`, because on
#     Android the process entry point lives in libSDL2.so
#     (Java_org_libsdl_app_SDLActivity_nativeRunMain -> dlopen/dlsym),
#   * replaces src/main.cpp with android/src/android_main.cpp (data directory
#     discovery, logcat bridge -- see that file),
#   * takes SDL2, dynarmic, zlib and liblzma from the NDK / the prebuilt tree
#     under /tmp/android instead of pkg-config,
#   * needs no Qt at all: the accelerometer comes from SDL (see ../src/sensors.cpp,
#     #ifdef __ANDROID__).
#
# Configure (the NDK toolchain file is mandatory):
#
#   cmake -S android -B build-android -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
#     -DCMAKE_BUILD_TYPE=Release
#
# android/make-apk.sh does this and packs the result into an APK.

cmake_minimum_required(VERSION 3.20)
project(nfsshift-android CXX C)

if(NOT ANDROID)
  message(FATAL_ERROR "android/CMakeLists.txt needs the NDK toolchain file "
                      "(-DCMAKE_TOOLCHAIN_FILE=.../build/cmake/android.toolchain.cmake)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(NFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "loader source tree (nfsshift-sfos)")

# --- prebuilt dependencies ----------------------------------------------------
# Built by android/setup-env.sh on the build host; all under /tmp/android.
set(ANDROID_DEPS_ROOT "/tmp/android" CACHE PATH "root of the prebuilt Android dependencies")
set(SDL2_PREFIX "${ANDROID_DEPS_ROOT}/prefix/${ANDROID_ABI}" CACHE PATH "SDL2 install prefix (also holds liblzma)")
set(LZMA_PREFIX "${SDL2_PREFIX}" CACHE PATH "liblzma install prefix")
set(DYNARMIC_DIR "/tmp/dynarmic" CACHE PATH "dynarmic source tree (headers)")
set(DYNARMIC_BUILD_DIR "${ANDROID_DEPS_ROOT}/build-dynarmic-${ANDROID_ABI}" CACHE PATH "dynarmic build tree (libraries)")

find_library(SDL2_LIBRARY NAMES SDL2 PATHS "${SDL2_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATHS "${SDL2_PREFIX}/include/SDL2" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(LZMA_LIBRARY NAMES lzma PATHS "${LZMA_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_path(LZMA_INCLUDE_DIR NAMES lzma.h PATHS "${LZMA_PREFIX}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# dynarmic is linked from its build tree.  Its exported package config cannot be
# used: dynarmicTargets.cmake is not next to dynarmicConfig.cmake in a build
# tree, and the config re-runs find_package(Boost/fmt/mcl/...).  The static
# libraries plus ${DYNARMIC_DIR}/src as include path are all we need -- the
# public headers (dynarmic/interface/**) pull in nothing from the externals.
find_library(DYNARMIC_dynarmic_LIBRARY NAMES dynarmic
             PATHS "${DYNARMIC_BUILD_DIR}/src/dynarmic" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(DYNARMIC_mcl_LIBRARY NAMES mcl
             PATHS "${DYNARMIC_BUILD_DIR}/externals/mcl/src" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(DYNARMIC_fmt_LIBRARY NAMES fmt
             PATHS "${DYNARMIC_BUILD_DIR}/externals/fmt" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# --- optional multiplayer sources --------------------------------------------
# src/mp/*.c and src/hle_mp.cpp are written in parallel and may not exist yet
# (or only partly), so they are OFF by default -- an incomplete set would break
# the link.  Switch them on either explicitly
#   -DNFS_MP_SOURCES="src/mp/nfsmp.c;src/mp/glue.c;src/mp/patches.c;src/hle_mp.cpp"
# (paths relative to NFS_ROOT or absolute) or, once the set is complete, with
#   -DNFS_MP_AUTO=ON
# which globs src/mp/*.c plus src/hle_mp.cpp.
option(NFS_MP_AUTO "pick up src/mp/*.c and src/hle_mp.cpp when they exist" ON)
set(NFS_MP_SOURCES "" CACHE STRING "extra multiplayer sources, relative to NFS_ROOT")

set(_mp_sources "")
if(NFS_MP_SOURCES)
  foreach(f ${NFS_MP_SOURCES})
    if(IS_ABSOLUTE "${f}")
      list(APPEND _mp_sources "${f}")
    else()
      list(APPEND _mp_sources "${NFS_ROOT}/${f}")
    endif()
  endforeach()
elseif(NFS_MP_AUTO)
  file(GLOB _mp_glob "${NFS_ROOT}/src/mp/*.c" "${NFS_ROOT}/src/mp/*.cpp")
  list(APPEND _mp_sources ${_mp_glob})
  if(EXISTS "${NFS_ROOT}/src/hle_mp.cpp")
    list(APPEND _mp_sources "${NFS_ROOT}/src/hle_mp.cpp")
  endif()
endif()
if(_mp_sources)
  message(STATUS "multiplayer sources: ${_mp_sources}")
else()
  message(STATUS "multiplayer sources: none (src/mp/* not present)")
endif()

# --- dlmalloc (guest arena allocator) ----------------------------------------
add_library(dlmalloc STATIC "${NFS_ROOT}/third_party/dlmalloc.c")
target_compile_definitions(dlmalloc PRIVATE ONLY_MSPACES=1 MSPACES=1 HAVE_MMAP=0 HAVE_MORECORE=0 USE_LOCKS=1)
target_compile_options(dlmalloc PRIVATE -w)

# --- the loader ---------------------------------------------------------------
# Same list as the top level CMakeLists.txt, but src/main.cpp is replaced by
# android/src/android_main.cpp.
add_library(main SHARED
  src/android_main.cpp
  "${NFS_ROOT}/src/guest.cpp"
  "${NFS_ROOT}/src/cpu_common.cpp"
  "${NFS_ROOT}/src/hle_stubs.cpp"
  "${NFS_ROOT}/src/cpu.cpp"
  "${NFS_ROOT}/src/s3e_image.cpp"
  "${NFS_ROOT}/src/config.cpp"
  "${NFS_ROOT}/src/hle_device.cpp"
  "${NFS_ROOT}/src/hle_memory.cpp"
  "${NFS_ROOT}/src/hle_file.cpp"
  "${NFS_ROOT}/src/hle_compression.cpp"
  "${NFS_ROOT}/src/hle_input.cpp"
  "${NFS_ROOT}/src/hle_sound.cpp"
  "${NFS_ROOT}/src/hle_gl.cpp"
  "${NFS_ROOT}/src/hle_egl.cpp"
  "${NFS_ROOT}/src/hle_misc.cpp"
  "${NFS_ROOT}/src/display.cpp"
  "${NFS_ROOT}/src/sensors.cpp"
  "${NFS_ROOT}/third_party/PVRTDecompress.cpp"
  ${_mp_sources}
)
# libmain.so: SDLActivity dlopen()s it and dlsym()s "SDL_main".
set_target_properties(main PROPERTIES PREFIX "lib" OUTPUT_NAME "main")

target_include_directories(main PRIVATE
  "${NFS_ROOT}/src"
  "${NFS_ROOT}/third_party"
  "${SDL2_INCLUDE_DIR}"
  "${LZMA_INCLUDE_DIR}"
  "${DYNARMIC_DIR}/src"
)
target_compile_options(main PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(main PRIVATE
  "${DYNARMIC_dynarmic_LIBRARY}"
  "${DYNARMIC_mcl_LIBRARY}"
  "${DYNARMIC_fmt_LIBRARY}"
  dlmalloc
  "${SDL2_LIBRARY}"
  "${LZMA_LIBRARY}"
  z          # NDK sysroot
  GLESv2
  EGL
  android    # AConfiguration/ANativeWindow, used by SDL and by us
  log        # __android_log_write for the logcat bridge
  dl
  m
)
# -Wl,--no-undefined: everything must resolve at link time, a missing symbol
#   would otherwise only show up when SDLActivity dlopen()s the library.
# -Wl,-z,max-page-size=16384: devices from Android 15 on may use 16 KB pages
#   and refuse to load a library whose LOAD segments are only 4 KB aligned.
#   NDK r27 only adds this with ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON, so set
#   it here explicitly (r28 makes it the default).
target_link_options(main PRIVATE
  -Wl,--no-undefined
  -Wl,-soname,libmain.so
  -Wl,-z,max-page-size=16384
)

message(STATUS "SDL2:     ${SDL2_LIBRARY}")
message(STATUS "liblzma:  ${LZMA_LIBRARY}")
message(STATUS "dynarmic: ${DYNARMIC_dynarmic_LIBRARY}")
