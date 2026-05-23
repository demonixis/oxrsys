#!/bin/zsh
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIGURATION="${CONFIGURATION:-Release}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
DERIVED_DATA_DIR="${DERIVED_DATA_DIR:-${REPO_ROOT}/build/companion-derived-data}"
PROJECT_PATH="${REPO_ROOT}/clients/companion/OpenXR OSX Companion.xcodeproj"
SCHEME="OpenXR OSX Companion"
APP_PRODUCTS_DIR="${DERIVED_DATA_DIR}/Build/Products/${CONFIGURATION}"
APP_PATH="${APP_PRODUCTS_DIR}/OpenXR OSX Companion.app"
RUNTIME_SOURCE_DIR="${BUILD_DIR}/runtime"
RUNTIME_RESOURCE_DIR="${APP_PATH}/Contents/Resources/OpenXRRuntime"

cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD_DIR}" --target openxr_osx

xcodebuild \
    -project "${PROJECT_PATH}" \
    -scheme "${SCHEME}" \
    -configuration "${CONFIGURATION}" \
    -derivedDataPath "${DERIVED_DATA_DIR}" \
    OPENXR_OSX_EMBED_RUNTIME=YES \
    CODE_SIGNING_ALLOWED=NO \
    build

if [[ ! -d "${APP_PATH}" ]]; then
    echo "Companion app not found at ${APP_PATH}" >&2
    exit 1
fi

mkdir -p "${RUNTIME_RESOURCE_DIR}"
cp "${RUNTIME_SOURCE_DIR}/libopenxr_osx.dylib" "${RUNTIME_RESOURCE_DIR}/"
cp "${RUNTIME_SOURCE_DIR}/openxr_osx.toml" "${RUNTIME_RESOURCE_DIR}/"
cp "${RUNTIME_SOURCE_DIR}/openxr_osx.json" "${RUNTIME_RESOURCE_DIR}/"

if [[ -n "${CODE_SIGN_IDENTITY:-}" ]]; then
    codesign \
        --force \
        --options runtime \
        --timestamp \
        --sign "${CODE_SIGN_IDENTITY}" \
        "${APP_PATH}"
    echo "Signed companion app with ${CODE_SIGN_IDENTITY}"
else
    echo "CODE_SIGN_IDENTITY is not set; leaving the app unsigned."
fi

echo "Packaged companion app:"
echo "  ${APP_PATH}"
