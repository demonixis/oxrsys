#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command cmake
require_command ctest
require_command ninja

BUILD_DIR="${BUILD_ROOT}/runtime-heavy"

log "Configuring heavy macOS runtime verification build"
cd "${REPO_ROOT}"
run cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Debug

log "Building runtime and test targets"
run cmake --build "${BUILD_DIR}" --parallel

log "Running runtime test suite"
run ctest --test-dir "${BUILD_DIR}" --output-on-failure
