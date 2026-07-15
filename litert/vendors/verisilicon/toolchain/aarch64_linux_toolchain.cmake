# CMake toolchain file for cross-compiling LiteRT VeriSilicon dispatch
# for Linux aarch64 (Orange Pi Zero 3W / Allwinner A733).
#
# Target: Debian 12 (bookworm), glibc 2.36, aarch64
# Host:   x86_64 Ubuntu 24.04 with gcc-12-aarch64-linux-gnu package
#
# The Pi's sysroot is rsynced to /pi-sysroot so the binaries link against
# the exact glibc 2.36 and libstdc++ on the Pi. This avoids GLIBC_2.38
# symbol errors that occur when using the cross-compiler's newer sysroot.
#
# To create the sysroot, rsync from the Pi:
#   rsync -aL opi:/usr/include/ /pi-sysroot/usr/include/
#   rsync -aL opi:/lib/         /pi-sysroot/lib/
#   rsync -aL opi:/usr/lib/     /pi-sysroot/usr/lib/
#
# Usage:
#   cmake .. \
#     -DCMAKE_TOOLCHAIN_FILE=litert/vendors/verisilicon/toolchain/aarch64_linux_toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DLITERT_ENABLE_VERISILICON=ON \
#     -DLITERT_ENABLE_QUALCOMM=OFF \
#     -DLITERT_ENABLE_SAMSUNG=OFF

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-12)

# Point at the Pi's exact sysroot (glibc 2.36, Debian 12 bookworm).
set(CMAKE_SYSROOT /pi-sysroot)
set(CMAKE_FIND_ROOT_PATH /pi-sysroot)

# CRITICAL: The cross-compiler ships glibc 2.39 headers at
# /usr/aarch64-linux-gnu/include which shadow our Pi sysroot.
# Those headers define __isoc23_* redirects requiring GLIBC_2.38.
# Using -isystem puts the Pi's headers BEFORE the default cross-sysroot
# headers, ensuring we compile against glibc 2.36 declarations.
set(CMAKE_C_FLAGS_INIT "-O2 -isystem /pi-sysroot/usr/include/aarch64-linux-gnu -isystem /pi-sysroot/usr/include")
set(CMAKE_CXX_FLAGS_INIT "-O2 -isystem /pi-sysroot/usr/include/aarch64-linux-gnu -isystem /pi-sysroot/usr/include")

# Search paths: target sysroot only for libs/includes, host for programs
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
