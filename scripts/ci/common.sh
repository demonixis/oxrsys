#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

CI_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${CI_SCRIPTS_DIR}/../.." && pwd)"
BUILD_ROOT="${REPO_ROOT}/build/ci"

log() {
    printf '==> %s\n' "$*"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

android_sdk_root() {
    if [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
        printf '%s\n' "${ANDROID_SDK_ROOT}"
        return
    fi

    if [[ -n "${ANDROID_HOME:-}" ]]; then
        printf '%s\n' "${ANDROID_HOME}"
        return
    fi

    if command -v sdkmanager >/dev/null 2>&1; then
        local sdkmanager_dir
        sdkmanager_dir="$(cd "$(dirname "$(command -v sdkmanager)")" && pwd)"
        cd "${sdkmanager_dir}/../../.." && pwd
        return
    fi

    die "ANDROID_SDK_ROOT is not set and sdkmanager is not available"
}

run() {
    printf '+'
    for arg in "$@"; do
        printf ' %q' "${arg}"
    done
    printf '\n'
    "$@"
}

ensure_build_root() {
    mkdir -p "${BUILD_ROOT}"
}

derived_data_path() {
    local name="$1"
    ensure_build_root
    printf '%s/xcode-%s' "${BUILD_ROOT}" "${name}"
}

ensure_android_packages() {
    require_command sdkmanager

    local sdk_root
    sdk_root="$(android_sdk_root)"
    export ANDROID_SDK_ROOT="${sdk_root}"
    export ANDROID_HOME="${sdk_root}"

    local packages=(
        "platform-tools"
        "platforms;android-34"
        "platforms;android-35"
        "build-tools;34.0.0"
        "ndk;26.3.11579264"
        "cmake;3.22.1"
    )

    log "Accepting Android SDK licenses"
    set +o pipefail
    yes 2>/dev/null | sdkmanager --licenses >/dev/null
    set -o pipefail

    log "Installing Android SDK packages into ${sdk_root}"
    run sdkmanager --install "${packages[@]}"
}

write_android_local_properties() {
    local sdk_root
    sdk_root="$(android_sdk_root)"
    local target="${REPO_ROOT}/clients/android-openxr/local.properties"

    log "Writing ${target}"
    cat > "${target}" <<EOF
sdk.dir=${sdk_root}
EOF
}

bootstrap_macos_host() {
    require_command xcodebuild
    require_command xcrun
    require_command brew

    log "Ensuring Metal Toolchain is available"
    if ! xcrun --find metallib >/dev/null 2>&1; then
        run xcodebuild -downloadComponent MetalToolchain
    fi

    local missing_formulae=()
    local formula
    for formula in vulkan-headers vulkan-loader; do
        if ! brew list "${formula}" >/dev/null 2>&1; then
            missing_formulae+=("${formula}")
        fi
    done

    if [[ "${#missing_formulae[@]}" -gt 0 ]]; then
        log "Installing Vulkan host prerequisites"
        run brew install "${missing_formulae[@]}"
    fi
}

has_visionos_platform() {
    local output
    output="$(xcodebuild -showsdks 2>/dev/null || true)"
    [[ "${output}" == *"visionOS SDKs:"* ]] && [[ "${output}" == *"visionOS Simulator SDKs:"* ]]
}

ensure_visionos_platform() {
    require_command xcodebuild

    if has_visionos_platform; then
        return
    fi

    local allow_download="${OPENXR_OSX_ALLOW_VISIONOS_PLATFORM_DOWNLOAD:-}"
    if [[ -z "${allow_download}" && -n "${CI:-}" ]]; then
        allow_download=1
    fi

    if [[ -n "${allow_download}" ]]; then
        log "Downloading visionOS platform support"
        run xcodebuild -downloadPlatform visionOS

        if has_visionos_platform; then
            return
        fi
    fi

    die "visionOS platform support is not installed. Install it with 'xcodebuild -downloadPlatform visionOS' or set OPENXR_OSX_ALLOW_VISIONOS_PLATFORM_DOWNLOAD=1."
}
