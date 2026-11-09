# ---------------------------------------------------------------------------
# CMake toolchain for classic AmigaOS 68k, cross-compiled with the Bebbo
# m68k-amigaos-gcc + libnix toolchain (https://github.com/bebbo/amiga-gcc).
#
# Usage:
#   cmake -B build/amigaos/build \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/amigaos.toolchain.cmake \
#         -DAMIGAOS_PISTORM3D=/path/to/PiStorm3D/SDK \
#         -DLIBRW_PLATFORM=MINI
#
# Prefer build/amigaos/build.sh, which drives the Makefile inside the proven
# Docker image; this toolchain is for hosts that have CMake plus the cross
# compiler available locally.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Consumed by CMakeLists.txt to select the AmigaOS profile.
set(AMIGAOS ON CACHE BOOL "Target classic AmigaOS 68k" FORCE)

# Toolchain location: env override first, then PATH.
if(DEFINED ENV{M68K_AMIGAOS_GCC})
    set(_amiga_prefix "$ENV{M68K_AMIGAOS_GCC}")
    set(CMAKE_C_COMPILER   "${_amiga_prefix}/bin/m68k-amigaos-gcc")
    set(CMAKE_CXX_COMPILER "${_amiga_prefix}/bin/m68k-amigaos-g++")
    set(CMAKE_FIND_ROOT_PATH "${_amiga_prefix}")
else()
    set(CMAKE_C_COMPILER   m68k-amigaos-gcc)
    set(CMAKE_CXX_COMPILER m68k-amigaos-g++)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CPU / FPU. FPU is mandatory (float-heavy game); 020/030-without-FPU is out of scope.
# Bebbo GCC 6.5 spells these -m68881 / -mhard-float (not -mfpu=68881).
set(AMIGAOS_CPU "68040" CACHE STRING "Target 68k CPU (68040 or 68060)")
set(AMIGAOS_FPU "68881" CACHE STRING "Target FPU option suffix, e.g. 68881")

set(_amiga_common
    "-m${AMIGAOS_CPU}"
    "-m${AMIGAOS_FPU}"
    "-mhard-float"
    "-mcrt=nix20"
    # No -malign-int: it breaks AmigaOS exec/NDK struct offsets (2-byte ABI).
    "-fno-omit-frame-pointer"
    "-fno-builtin-sin"
    "-fno-builtin-cos")

set(CMAKE_C_FLAGS_INIT   "${_amiga_common} -std=gnu11")
set(CMAKE_CXX_FLAGS_INIT
    "${_amiga_common} -std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics")

# MiniGL: GL types/enums and the dispatch API (<proto/minigl.h>) come from the
# PiStorm3D shared minigl.library 29.x SDK (minigl-shared-library/include); the
# import lib is lib/libminigl.a.
set(AMIGAOS_PISTORM3D "" CACHE PATH "PiStorm3D SDK dir (minigl-shared-library/include + lib/)")
if(AMIGAOS_PISTORM3D)
    include_directories("${AMIGAOS_PISTORM3D}/minigl-shared-library/include")
    link_directories("${AMIGAOS_PISTORM3D}/lib")
endif()
