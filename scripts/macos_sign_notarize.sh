#!/bin/zsh
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SCRIPT_NAME="$(basename "$0")"

DEFAULT_CMAKE_BUILD_DIR="${REPO_ROOT}/build"
DEFAULT_HOME_DERIVED_DATA="${REPO_ROOT}/build/xcode/OXRSysHome"
DEFAULT_HOME_APP="${DEFAULT_HOME_DERIVED_DATA}/Build/Products/Release/OXRSys Home.app"
DEFAULT_HOME_ENTITLEMENTS="${REPO_ROOT}/clients/Apple/oxrsys-home/OXRSys Home/OXRSys Home.entitlements"
DEFAULT_ARCHIVE_DIR="${REPO_ROOT}/build/dist"

CMAKE_BUILD_DIR="${DEFAULT_CMAKE_BUILD_DIR}"
HOME_DERIVED_DATA="${DEFAULT_HOME_DERIVED_DATA}"
HOME_APP=""
HOME_ENTITLEMENTS="${DEFAULT_HOME_ENTITLEMENTS}"
RUNTIME_DIR=""
RUNTIME_DYLIB=""
RUNTIME_MANIFEST=""
RUNTIME_CONFIG=""
ARCHIVE_DIR="${DEFAULT_ARCHIVE_DIR}"
ARCHIVE_PATH=""
SIGN_IDENTITY=""
APPLE_ID=""
APP_PASSWORD=""
TEAM_ID=""
BUILD_RUNTIME=0
BUILD_HOME=0
NOTARIZE=0
STAPLE=1

TEMP_DIRS=()

usage() {
    cat <<EOF
Usage:
  ${SCRIPT_NAME} [options]

Signs the macOS runtime dylib and OXRSys Home app, then creates a single zip archive containing:
  OXRSys-macOS/
    OXRSys Home.app
    runtime/liboxrsys-runtime.dylib
    runtime/oxrsys-runtime.json
    runtime/oxrsys-runtime.toml

Options:
  --identity NAME          Developer ID Application signing identity. If omitted, the script
                           auto-selects the only Developer ID Application identity in the keychain.
  --notarize              Submit the combined archive with xcrun notarytool.
  --apple-id EMAIL        Apple Developer account email used by notarytool.
  --password PASSWORD     App-specific password used by notarytool.
  --team-id TEAM_ID       Apple Developer Team ID. Recommended when notarizing.
  --skip-staple           Do not staple the accepted notarization ticket to OXRSys Home.app.

Build options:
  --build-runtime         Configure and build the runtime target before signing.
  --build-home            Build the Release OXRSys Home app before signing.
  --cmake-build-dir DIR   CMake build directory. Default: build
  --home-derived-data DIR DerivedData path used by --build-home. Default: build/xcode/OXRSysHome

Path options:
  --runtime-dir DIR       Runtime output directory. Default: <cmake-build-dir>/runtime
  --runtime-dylib PATH    Runtime dylib path. Default: <runtime-dir>/liboxrsys-runtime.dylib
  --runtime-manifest PATH Runtime manifest path. Default: <runtime-dir>/oxrsys-runtime.json
  --runtime-config PATH   Runtime TOML path. Default: <runtime-dir>/oxrsys-runtime.toml
  --home-app PATH         OXRSys Home.app path. Default: <home-derived-data>/Build/Products/Release/OXRSys Home.app
  --home-entitlements PATH
                           Entitlements used for the final app signature.
  --archive-dir DIR       Directory for the generated zip. Default: build/dist
  --archive PATH          Full output zip path. Overrides --archive-dir.
  -h, --help              Show this help.

Examples:
  ${SCRIPT_NAME} \\
    --identity "Developer ID Application: OXRSys Team (ABCDE12345)" \\
    --home-app "build/xcode/OXRSysHome/Build/Products/Release/OXRSys Home.app"

  ${SCRIPT_NAME} --notarize \\
    --identity "Developer ID Application: OXRSys Team (ABCDE12345)" \\
    --team-id ABCDE12345 \\
    --apple-id developer@example.com \\
    --password "xxxx-xxxx-xxxx-xxxx" \\
    --home-app "build/xcode/OXRSysHome/Build/Products/Release/OXRSys Home.app"
EOF
}

cleanup() {
    local temp_dir
    for temp_dir in "${TEMP_DIRS[@]}"; do
        if [[ -n "${temp_dir}" && -d "${temp_dir}" ]]; then
            rm -rf "${temp_dir}"
        fi
    done
}
trap cleanup EXIT

fail() {
    echo "error: $*" >&2
    exit 1
}

warn() {
    echo "warning: $*" >&2
}

require_macos() {
    if [[ "$(/usr/bin/uname -s)" != "Darwin" ]]; then
        fail "macOS signing and notarization require macOS."
    fi
}

require_tool() {
    local tool_name="$1"
    if ! command -v "${tool_name}" >/dev/null 2>&1; then
        fail "Required tool not found: ${tool_name}"
    fi
}

absolute_path() {
    local path="$1"
    if [[ "${path}" == /* ]]; then
        print -r -- "${path}"
    else
        print -r -- "$(pwd)/${path}"
    fi
}

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --identity)
                [[ $# -ge 2 ]] || fail "--identity requires a value"
                SIGN_IDENTITY="$2"
                shift 2
                ;;
            --notarize)
                NOTARIZE=1
                shift
                ;;
            --apple-id)
                [[ $# -ge 2 ]] || fail "--apple-id requires a value"
                APPLE_ID="$2"
                shift 2
                ;;
            --password|--app-password)
                [[ $# -ge 2 ]] || fail "$1 requires a value"
                APP_PASSWORD="$2"
                shift 2
                ;;
            --team-id)
                [[ $# -ge 2 ]] || fail "--team-id requires a value"
                TEAM_ID="$2"
                shift 2
                ;;
            --skip-staple)
                STAPLE=0
                shift
                ;;
            --build-runtime)
                BUILD_RUNTIME=1
                shift
                ;;
            --build-home)
                BUILD_HOME=1
                shift
                ;;
            --cmake-build-dir)
                [[ $# -ge 2 ]] || fail "--cmake-build-dir requires a value"
                CMAKE_BUILD_DIR="$(absolute_path "$2")"
                shift 2
                ;;
            --home-derived-data)
                [[ $# -ge 2 ]] || fail "--home-derived-data requires a value"
                HOME_DERIVED_DATA="$(absolute_path "$2")"
                shift 2
                ;;
            --runtime-dir)
                [[ $# -ge 2 ]] || fail "--runtime-dir requires a value"
                RUNTIME_DIR="$(absolute_path "$2")"
                shift 2
                ;;
            --runtime-dylib)
                [[ $# -ge 2 ]] || fail "--runtime-dylib requires a value"
                RUNTIME_DYLIB="$(absolute_path "$2")"
                shift 2
                ;;
            --runtime-manifest)
                [[ $# -ge 2 ]] || fail "--runtime-manifest requires a value"
                RUNTIME_MANIFEST="$(absolute_path "$2")"
                shift 2
                ;;
            --runtime-config)
                [[ $# -ge 2 ]] || fail "--runtime-config requires a value"
                RUNTIME_CONFIG="$(absolute_path "$2")"
                shift 2
                ;;
            --home-app)
                [[ $# -ge 2 ]] || fail "--home-app requires a value"
                HOME_APP="$(absolute_path "$2")"
                shift 2
                ;;
            --home-entitlements)
                [[ $# -ge 2 ]] || fail "--home-entitlements requires a value"
                HOME_ENTITLEMENTS="$(absolute_path "$2")"
                shift 2
                ;;
            --archive-dir)
                [[ $# -ge 2 ]] || fail "--archive-dir requires a value"
                ARCHIVE_DIR="$(absolute_path "$2")"
                shift 2
                ;;
            --archive)
                [[ $# -ge 2 ]] || fail "--archive requires a value"
                ARCHIVE_PATH="$(absolute_path "$2")"
                shift 2
                ;;
            -h|--help|help)
                usage
                exit 0
                ;;
            *)
                fail "Unknown option: $1"
                ;;
        esac
    done
}

apply_defaults() {
    if [[ -z "${RUNTIME_DIR}" ]]; then
        RUNTIME_DIR="${CMAKE_BUILD_DIR}/runtime"
    fi
    if [[ -z "${RUNTIME_DYLIB}" ]]; then
        RUNTIME_DYLIB="${RUNTIME_DIR}/liboxrsys-runtime.dylib"
    fi
    if [[ -z "${RUNTIME_MANIFEST}" ]]; then
        RUNTIME_MANIFEST="${RUNTIME_DIR}/oxrsys-runtime.json"
    fi
    if [[ -z "${RUNTIME_CONFIG}" ]]; then
        RUNTIME_CONFIG="${RUNTIME_DIR}/oxrsys-runtime.toml"
    fi
    if [[ -z "${HOME_APP}" ]]; then
        HOME_APP="${HOME_DERIVED_DATA}/Build/Products/Release/OXRSys Home.app"
    fi
    if [[ -z "${ARCHIVE_PATH}" ]]; then
        local timestamp
        timestamp="$(/bin/date +%Y%m%d-%H%M%S)"
        ARCHIVE_PATH="${ARCHIVE_DIR}/OXRSys-macOS-${timestamp}.zip"
    fi
}

resolve_sign_identity() {
    if [[ -n "${SIGN_IDENTITY}" ]]; then
        return
    fi

    local -a identities
    identities=()

    local identity
    while IFS= read -r identity; do
        identities+=("${identity}")
    done < <(/usr/bin/security find-identity -v -p codesigning 2>/dev/null | /usr/bin/sed -n 's/.*"\(Developer ID Application:.*\)".*/\1/p')

    if [[ ${#identities[@]} -eq 1 ]]; then
        SIGN_IDENTITY="${identities[1]}"
        echo "Using signing identity:"
        echo "  ${SIGN_IDENTITY}"
        return
    fi

    if [[ ${#identities[@]} -eq 0 ]]; then
        fail "No Developer ID Application signing identity found. Pass --identity after installing the certificate."
    fi

    echo "Multiple Developer ID Application identities found:" >&2
    for identity in "${identities[@]}"; do
        echo "  ${identity}" >&2
    done
    fail "Pass the intended identity with --identity."
}

validate_inputs() {
    [[ -f "${RUNTIME_DYLIB}" ]] || fail "Runtime dylib not found: ${RUNTIME_DYLIB}"
    [[ -f "${RUNTIME_MANIFEST}" ]] || fail "Runtime manifest not found: ${RUNTIME_MANIFEST}"
    [[ -d "${HOME_APP}" ]] || fail "Home app not found: ${HOME_APP}"

    if [[ -n "${HOME_ENTITLEMENTS}" && ! -f "${HOME_ENTITLEMENTS}" ]]; then
        fail "Home entitlements file not found: ${HOME_ENTITLEMENTS}"
    fi

    if [[ "${NOTARIZE}" -eq 1 ]]; then
        [[ -n "${APPLE_ID}" ]] || fail "--notarize requires --apple-id"
        [[ -n "${APP_PASSWORD}" ]] || fail "--notarize requires --password"
        if [[ -z "${TEAM_ID}" ]]; then
            warn "--team-id was not provided. notarytool can fail when the Apple ID belongs to multiple teams."
        fi
    fi
}

build_runtime() {
    require_tool cmake

    run cmake -B "${CMAKE_BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
    run cmake --build "${CMAKE_BUILD_DIR}" --target oxrsys_runtime
}

build_home() {
    run /usr/bin/xcodebuild \
        -project "${REPO_ROOT}/clients/Apple/oxrsys-home/OXRSys Home.xcodeproj" \
        -scheme "OXRSys Home" \
        -configuration Release \
        -derivedDataPath "${HOME_DERIVED_DATA}" \
        CODE_SIGNING_ALLOWED=NO \
        build
}

sign_runtime() {
    echo "Signing runtime dylib:"
    echo "  ${RUNTIME_DYLIB}"

    run /usr/bin/codesign \
        --force \
        --timestamp \
        --options runtime \
        --sign "${SIGN_IDENTITY}" \
        "${RUNTIME_DYLIB}"

    run /usr/bin/codesign --verify --strict --verbose=2 "${RUNTIME_DYLIB}"
}

sign_home() {
    echo "Signing Home app:"
    echo "  ${HOME_APP}"

    run /usr/bin/codesign \
        --force \
        --deep \
        --timestamp \
        --options runtime \
        --sign "${SIGN_IDENTITY}" \
        "${HOME_APP}"

    local -a final_sign_args
    final_sign_args=(
        /usr/bin/codesign
        --force
        --timestamp
        --options runtime
        --sign "${SIGN_IDENTITY}"
    )

    if [[ -n "${HOME_ENTITLEMENTS}" ]]; then
        final_sign_args+=(--entitlements "${HOME_ENTITLEMENTS}")
    fi

    final_sign_args+=("${HOME_APP}")
    run "${final_sign_args[@]}"

    run /usr/bin/codesign --verify --deep --strict --verbose=2 "${HOME_APP}"
}

create_archive() {
    local archive_path="$1"
    local archive_parent
    archive_parent="$(dirname "${archive_path}")"

    mkdir -p "${archive_parent}"

    local temp_dir
    temp_dir="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/oxrsys-package.XXXXXX")"
    TEMP_DIRS+=("${temp_dir}")

    local package_root="${temp_dir}/OXRSys-macOS"
    local package_runtime_dir="${package_root}/runtime"
    mkdir -p "${package_runtime_dir}"

    run /usr/bin/ditto "${HOME_APP}" "${package_root}/OXRSys Home.app"
    run /usr/bin/ditto "${RUNTIME_DYLIB}" "${package_runtime_dir}/$(basename "${RUNTIME_DYLIB}")"

    local packaged_manifest="${package_runtime_dir}/$(basename "${RUNTIME_MANIFEST}")"
    run /usr/bin/ditto "${RUNTIME_MANIFEST}" "${packaged_manifest}"
    run /usr/bin/plutil \
        -replace runtime.library_path \
        -string "./$(basename "${RUNTIME_DYLIB}")" \
        "${packaged_manifest}"

    if [[ -f "${RUNTIME_CONFIG}" ]]; then
        run /usr/bin/ditto "${RUNTIME_CONFIG}" "${package_runtime_dir}/$(basename "${RUNTIME_CONFIG}")"
    else
        warn "Runtime config was not found and will not be packaged: ${RUNTIME_CONFIG}"
    fi

    rm -f "${archive_path}"
    run /usr/bin/ditto -c -k --sequesterRsrc --keepParent "${package_root}" "${archive_path}"

    echo "Created archive:"
    echo "  ${archive_path}"
}

notarize_archive() {
    local archive_path="$1"
    local -a notary_args

    notary_args=(
        notarytool
        submit "${archive_path}"
        --apple-id "${APPLE_ID}"
        --password "${APP_PASSWORD}"
        --wait
    )

    if [[ -n "${TEAM_ID}" ]]; then
        notary_args+=(--team-id "${TEAM_ID}")
    fi

    echo "Submitting archive for notarization:"
    echo "  ${archive_path}"
    /usr/bin/xcrun "${notary_args[@]}"
}

staple_home() {
    if [[ "${STAPLE}" -eq 0 ]]; then
        echo "Skipping stapling by request."
        return
    fi

    echo "Stapling notarization ticket to Home app:"
    echo "  ${HOME_APP}"
    run /usr/bin/xcrun stapler staple "${HOME_APP}"
    run /usr/bin/xcrun stapler validate "${HOME_APP}"
}

main() {
    parse_args "$@"
    require_macos
    require_tool /usr/bin/codesign
    require_tool /usr/bin/ditto
    require_tool /usr/bin/plutil
    require_tool /usr/bin/security

    if [[ "${NOTARIZE}" -eq 1 ]]; then
        require_tool /usr/bin/xcrun
    fi

    apply_defaults

    if [[ "${BUILD_RUNTIME}" -eq 1 ]]; then
        build_runtime
    fi
    if [[ "${BUILD_HOME}" -eq 1 ]]; then
        build_home
    fi

    resolve_sign_identity
    validate_inputs

    sign_runtime
    sign_home
    create_archive "${ARCHIVE_PATH}"

    if [[ "${NOTARIZE}" -eq 1 ]]; then
        notarize_archive "${ARCHIVE_PATH}"
        staple_home
        echo "Rebuilding archive so it contains the stapled Home app."
        create_archive "${ARCHIVE_PATH}"
        echo "The runtime dylib was included in the notarized archive. Stapling is only applied to the app bundle."
    fi
}

main "$@"
