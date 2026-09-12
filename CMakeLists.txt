cmake_minimum_required(VERSION 3.16)
project(harbour-nfsshift CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# PIE keeps the executable and its brk heap out of the low 4 GB, which are
# reserved for the identity-mapped guest address space.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DYNARMIC_DIR "${CMAKE_SOURCE_DIR}/../dynarmic" CACHE PATH "dynarmic source tree")
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DYNARMIC_DIR} dynarmic EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(GLES REQUIRED glesv2 egl)
find_package(ZLIB REQUIRED)
pkg_check_modules(LZMA REQUIRED liblzma)
pkg_check_modules(QT REQUIRED Qt5Core Qt5Sensors)

add_library(dlmalloc STATIC third_party/dlmalloc.c)
target_compile_definitions(dlmalloc PRIVATE ONLY_MSPACES=1 MSPACES=1 HAVE_MMAP=0 HAVE_MORECORE=0 USE_LOCKS=1)
target_compile_options(dlmalloc PRIVATE -w)

add_executable(harbour-nfsshift
  src/main.cpp
  src/guest.cpp
  src/cpu.cpp
  src/s3e_image.cpp
  src/config.cpp
  src/hle_device.cpp
  src/hle_memory.cpp
  src/hle_file.cpp
  src/hle_compression.cpp
  src/hle_input.cpp
  src/hle_sound.cpp
  src/hle_gl.cpp
  src/hle_egl.cpp
  src/hle_misc.cpp
  src/hle_mp.cpp
  src/display.cpp
  src/sensors.cpp
  src/mp/nfsmp.c
  src/mp/glue.c
  src/mp/patches.c
  third_party/PVRTDecompress.cpp
)
set_source_files_properties(src/mp/nfsmp.c src/mp/glue.c src/mp/patches.c PROPERTIES COMPILE_OPTIONS "-std=c11")
target_include_directories(harbour-nfsshift PRIVATE src third_party ${SDL2_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS} ${LZMA_INCLUDE_DIRS} ${QT_INCLUDE_DIRS})
target_link_libraries(harbour-nfsshift PRIVATE dynarmic dlmalloc ${SDL2_LIBRARIES} ${GLES_LIBRARIES} ZLIB::ZLIB ${LZMA_LIBRARIES} ${QT_LIBRARIES} pthread dl)
target_compile_options(harbour-nfsshift PRIVATE -Wall -Wno-unused-parameter)
target_link_options(harbour-nfsshift PRIVATE -pie)
