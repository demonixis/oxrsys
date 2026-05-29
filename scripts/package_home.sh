#!/bin/zsh
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIGURATION="${CONFIGURATION:-Release}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
DERIVED_DATA_DIR="${DERIVED_DATA_DIR:-${REPO_ROOT}/build/home-derived-data}"
PROJECT_PATH="${REPO_ROOT}/clients/Apple/oxrsys-home/OXRSys Home.xcodeproj"
SCHEME="OXRSys Home"
APP_PRODUCTS_DIR="${DERIVED_DATA_DIR}/Build/Products/${CONFIGURATION}"
APP_PATH="${APP_PRODUCTS_DIR}/OXRSys Home.app"
RUNTIME_SOURCE_DIR="${BUILD_DIR}/runtime"
RUNTIME_RESOURCE_DIR="${APP_PATH}/Contents/Resources/OXRSysRuntime"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD_DIR}" --target oxrsys_runtime

xcodebuild \
    -project "${PROJECT_PATH}" \
    -scheme "${SCHEME}" \
    -configuration "${CONFIGURATION}" \
    -derivedDataPath "${DERIVED_DATA_DIR}" \
    OXRSYS_EMBED_RUNTIME=YES \
    CODE_SIGNING_ALLOWED=NO \
    build

if [[ ! -d "${APP_PATH}" ]]; then
    echo "OXRSys Home app not found at ${APP_PATH}" >&2
    exit 1
fi

mkdir -p "${RUNTIME_RESOURCE_DIR}"
cp "${RUNTIME_SOURCE_DIR}/liboxrsys-runtime.dylib" "${RUNTIME_RESOURCE_DIR}/"
cp "${RUNTIME_SOURCE_DIR}/oxrsys-runtime.toml" "${RUNTIME_RESOURCE_DIR}/"
cp "${RUNTIME_SOURCE_DIR}/oxrsys-runtime.json" "${RUNTIME_RESOURCE_DIR}/"

if [[ -n "${CODE_SIGN_IDENTITY:-}" ]]; then
    codesign \
        --force \
        --options runtime \
        --timestamp \
        --sign "${CODE_SIGN_IDENTITY}" \
        "${APP_PATH}"
    echo "Signed Home app with ${CODE_SIGN_IDENTITY}"
else
    echo "CODE_SIGN_IDENTITY is not set; leaving the app unsigned."
fi

echo "Packaged Home app:"
echo "  ${APP_PATH}"
