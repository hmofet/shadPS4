# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Cross-compiles for ARM64 Linux with clang against a target root file system,
# e.g. an Arch Linux ARM or Debian arm64 root copied from the device.
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-sysroot.cmake \
#         -DAARCH64_SYSROOT=/path/to/root ...
# The compilers default to clang/clang++; set AARCH64_CLANG_SUFFIX (e.g. -21)
# to pick a versioned install. AARCH64_TRIPLE defaults to the triple of the
# GCC install found in the sysroot.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(AARCH64_SYSROOT "$ENV{AARCH64_SYSROOT}" CACHE PATH "Target root file system")
set(AARCH64_CLANG_SUFFIX "$ENV{AARCH64_CLANG_SUFFIX}" CACHE STRING "Suffix of the clang binaries")
if (NOT AARCH64_SYSROOT)
    message(FATAL_ERROR "Set AARCH64_SYSROOT to the target root file system")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES AARCH64_SYSROOT AARCH64_CLANG_SUFFIX AARCH64_TRIPLE)

if (NOT AARCH64_TRIPLE)
    file(GLOB _gcc_triples LIST_DIRECTORIES true RELATIVE "${AARCH64_SYSROOT}/usr/lib/gcc"
         "${AARCH64_SYSROOT}/usr/lib/gcc/aarch64*")
    list(GET _gcc_triples 0 _triple)
    set(AARCH64_TRIPLE "${_triple}" CACHE STRING "Target triple")
endif()

set(CMAKE_SYSROOT "${AARCH64_SYSROOT}")
set(CMAKE_C_COMPILER clang${AARCH64_CLANG_SUFFIX})
set(CMAKE_CXX_COMPILER clang++${AARCH64_CLANG_SUFFIX})
set(CMAKE_ASM_COMPILER clang${AARCH64_CLANG_SUFFIX})
set(CMAKE_C_COMPILER_TARGET ${AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${AARCH64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${AARCH64_TRIPLE})
set(CMAKE_AR llvm-ar${AARCH64_CLANG_SUFFIX})
set(CMAKE_RANLIB llvm-ranlib${AARCH64_CLANG_SUFFIX})
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH "${AARCH64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${AARCH64_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${AARCH64_SYSROOT}/usr/lib/pkgconfig:${AARCH64_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
