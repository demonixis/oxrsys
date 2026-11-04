#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the native-arm64 HEVC encoder helper as a single self-contained
# executable. This is the bulletproof path (one translation unit, system
# frameworks only) — no CMake, no FetchContent, no arch contention with the
# runtime's build/x86 tree. Use the CMakeLists in this directory if you prefer.
#
# Output: build/helper/oxrsys-encoder-helper (arm64), ad-hoc codesigned.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build/helper"
OUT_BIN="$OUT_DIR/oxrsys-encoder-helper"

mkdir -p "$OUT_DIR"

echo "Compiling native-arm64 encoder helper -> $OUT_BIN"
xcrun --sdk macosx clang++ \
    -arch arm64 \
    -std=c++17 \
    -O2 \
    -Wall -Wextra \
    -o "$OUT_BIN" \
    "$HERE/main.mm" \
    -framework Foundation \
    -framework CoreFoundation \
    -framework CoreMedia \
    -framework CoreVideo \
    -framework VideoToolbox \
    -framework IOSurface

echo "Ad-hoc codesigning"
codesign --force --sign - "$OUT_BIN"

echo "Result:"
lipo -info "$OUT_BIN"
file "$OUT_BIN"
echo "OK: $OUT_BIN"
