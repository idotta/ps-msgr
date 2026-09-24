# SPDX-License-Identifier: Apache-2.0
#
# Cross toolchain for the BeagleBone Black (Debian trixie armhf), using
# Debian's crossbuild-essential-armhf. Keep Debian's default code generation
# (ARMv7-A, Thumb-2, VFPv3-D16): no -mcpu/-mfpu flags.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)   # header test only

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ctest runs armhf test binaries through qemu user-mode emulation.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L /usr/arm-linux-gnueabihf)

set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE armhf)
