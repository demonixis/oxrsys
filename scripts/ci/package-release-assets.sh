#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command cmake
require_command ninja
require_command xcodebuild
require_command zip
require_command ditto
require_command shasum

if [[ "${OSTYPE}" == darwin* ]]; then
    bootstrap_macos_host
fi

DIST_DIR="${REPO_ROOT}/dist/release"
RUNTIME_BUILD_DIR="${BUILD_ROOT}/runtime-release"
mkdir -p "${DIST_DIR}"

log "Building release runtime artifacts"
cd "${REPO_ROOT}"
run cmake -B "${RUNTIME_BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
run cmake --build "${RUNTIME_BUILD_DIR}" --parallel

log "Packaging runtime zip"
(
    cd "${RUNTIME_BUILD_DIR}/runtime"
    run zip -r "${DIST_DIR}/openxr-osx-runtime.zip" \
        libopenxr_osx.dylib \
        openxr_osx.json \
        openxr_osx.toml
)

log "Building release Quest APK"
run "${REPO_ROOT}/scripts/ci/build-android.sh"
cp "${REPO_ROOT}/clients/android-openxr/app/build/outputs/apk/debug/app-debug.apk" \
   "${DIST_DIR}/openxr-quest-player.apk"

log "Building release companion app"
run /usr/bin/env \
    HOME="$(derived_data_path companion-release)/home" \
    xcodebuild \
    -project "${REPO_ROOT}/clients/companion/OpenXR OSX Companion.xcodeproj" \
    -scheme "OpenXR OSX Companion" \
    -configuration Release \
    -derivedDataPath "$(derived_data_path companion-release)" \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build
run ditto -c -k --keepParent \
    "$(derived_data_path companion-release)/Build/Products/Release/OpenXR OSX Companion.app" \
    "${DIST_DIR}/OpenXR.OSX.Companion.zip"

log "Building release simulator app"
run xcodebuild \
    -project "${REPO_ROOT}/clients/simulator/OpenXR Simulator.xcodeproj" \
    -scheme "OpenXR Simulator" \
    -configuration Release \
    -destination 'platform=macOS' \
    -derivedDataPath "$(derived_data_path simulator-release)" \
    build
run ditto -c -k --keepParent \
    "$(derived_data_path simulator-release)/Build/Products/Release/OpenXR Simulator.app" \
    "${DIST_DIR}/OpenXR.Simulator.zip"

log "Writing release checksums"
(
    cd "${DIST_DIR}"
    run shasum -a 256 \
        openxr-osx-runtime.zip \
        openxr-quest-player.apk \
        OpenXR.OSX.Companion.zip \
        OpenXR.Simulator.zip \
        > SHA256SUMS.txt
)
