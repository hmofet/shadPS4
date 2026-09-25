#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the FEXCore static libraries that ENABLE_FEX_GUEST_CPU links, at a
# pinned FEX revision. Run on (or cross-compile for) an ARM64 Linux host:
#
#   scripts/build-fexcore.sh <build-dir> [extra cmake arguments...]
#
# then configure shadPS4 with
#   -DENABLE_FEX_GUEST_CPU=ON -DFEXCORE_BUILD_DIR=<build-dir>
#
# FEX_SOURCE_DIR (default externals/fex, not a submodule) holds the checkout.
# Pass the same -DCMAKE_TOOLCHAIN_FILE=... as for shadPS4 when cross-compiling.
set -euo pipefail

# The revision Bachata S4 shipped with (FEX-2607-179). Moving it is a
# one-line change; rebuild and rerun the guest harness when you do.
FEX_URL=https://github.com/FEX-Emu/FEX.git
FEX_REVISION=f2b679f6028ce1c38875233aecfcf5d3f8ebecec

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir=${1:?usage: $0 <build-dir> [cmake args...]}
shift
src=${FEX_SOURCE_DIR:-$root/externals/fex}

if [[ ! -d $src/.git ]]; then
    git clone --filter=blob:none "$FEX_URL" "$src"
fi
if [[ $(git -C "$src" rev-parse HEAD) != "$FEX_REVISION" ]]; then
    git -C "$src" fetch --quiet origin "$FEX_REVISION" || true
    git -C "$src" checkout --quiet "$FEX_REVISION"
fi
# Only the submodules FEXCore itself needs; the test binaries are gigabytes.
git -C "$src" submodule update --init --depth 1 -- \
    External/unordered_dense External/rpmalloc External/xxhash External/fmt \
    External/range-v3 External/vixl Source/Common/cpp-optparse

cmake -S "$src" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DTUNE_CPU=none \
    -DBUILD_TESTING=OFF \
    -DBUILD_FEX_LINUX_TESTS=OFF \
    -DBUILD_THUNKS=OFF \
    -DBUILD_FEXCONFIG=OFF \
    -DENABLE_GDB_SYMBOLS=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF \
    -DENABLE_VIXL_DISASSEMBLER=OFF \
    -DENABLE_VIXL_SIMULATOR=OFF \
    -DENABLE_ZYDIS=OFF \
    -DENABLE_FEXCORE_PROFILER=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "$@"

cmake --build "$build_dir" --target \
    FEXCore FEXCore_Base Common CommonTools JemallocLibs cpp-optparse tiny-json \
    fmt xxhash cephes_128bit softfloat_3e rpmalloc
