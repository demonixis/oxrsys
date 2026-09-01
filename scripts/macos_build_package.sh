#!/bin/zsh
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SCRIPT_NAME="$(basename "$0")"

CONFIGURATION="Debug"
ARCHITECTURES=""
CMAKE_ARCHITECTURES=""
XCODE_ARCHITECTURES=""
EXPECTED_ARCHITECTURES=()
DEFAULT_PACKAGE_BUILD_ROOT="${REPO_ROOT}/build/macos-package"
CMAKE_BUILD_DIR="${DEFAULT_PACKAGE_BUILD_ROOT}/cmake"
HOME_DERIVED_DATA="${DEFAULT_PACKAGE_BUILD_ROOT}/xcode/OXRSysHome"
OUTPUT_DIR="${REPO_ROOT}/build/OXRSys-macOS"
BUILD_RUNTIME=1
BUILD_HOME=1
CLEAN_OUTPUT=1

usage() {
    cat <<EOF
Usage:
  ${SCRIPT_NAME} [options]

Builds the macOS runtime and OXRSys Home, then assembles one local package directory:
  build/OXRSys-macOS/
    OXRSys Home.app
    runtime/liboxrsys-runtime.dylib
    runtime/oxrsys-runtime.json
    runtime/oxrsys-runtime.toml

Options:
  --configuration NAME    Xcode and CMake configuration. Default: Debug
  --architectures VALUE   native, arm64, x86_64, or universal.
                          Default: native for Debug, universal for Release.
  --cmake-build-dir DIR   CMake build directory. Default: build/macos-package/cmake
  --home-derived-data DIR DerivedData path for the Home build. Default: build/macos-package/xcode/OXRSysHome
  --output-dir DIR        Assembled package directory. Default: build/OXRSys-macOS
  --skip-runtime-build    Reuse the runtime already present in --cmake-build-dir.
  --skip-home-build       Reuse the Home app already present in --home-derived-data.
  --keep-output           Do not clean the package directory before copying files.
  -h, --help              Show this help.

Examples:
  ${SCRIPT_NAME}
  ${SCRIPT_NAME} --configuration Release --architectures universal \
    --output-dir build/OXRSys-macOS-Release
EOF
}

fail() {
    echo "error: $*" >&2
    exit 1
}

warn() {
    echo "warning: $*" >&2
}

require_macos() {
    if [[ "$(/usr/bin/uname -s)" != "Darwin" ]]; then
        fail "The macOS package build requires macOS."
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
    if [[ "${path}" != /* ]]; then
        path="$(pwd)/${path}"
    fi
    print -r -- "${path:A}"
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
            --configuration)
                [[ $# -ge 2 ]] || fail "--configuration requires a value"
                CONFIGURATION="$2"
                shift 2
                ;;
            --architectures)
                [[ $# -ge 2 ]] || fail "--architectures requires a value"
                ARCHITECTURES="$2"
                shift 2
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
            --output-dir)
                [[ $# -ge 2 ]] || fail "--output-dir requires a value"
                OUTPUT_DIR="$(absolute_path "$2")"
                shift 2
                ;;
            --skip-runtime-build)
                BUILD_RUNTIME=0
                shift
                ;;
            --skip-home-build)
                BUILD_HOME=0
                shift
                ;;
            --keep-output)
                CLEAN_OUTPUT=0
                shift
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

runtime_dir() {
    print -r -- "${CMAKE_BUILD_DIR}/runtime"
}

runtime_dylib_path() {
    print -r -- "$(runtime_dir)/liboxrsys-runtime.dylib"
}

runtime_manifest_path() {
    print -r -- "$(runtime_dir)/oxrsys-runtime.json"
}

runtime_config_path() {
    print -r -- "$(runtime_dir)/oxrsys-runtime.toml"
}

home_app_path() {
    print -r -- "${HOME_DERIVED_DATA}/Build/Products/${CONFIGURATION}/OXRSys Home.app"
}

validate_configuration() {
    case "${CONFIGURATION}" in
        Debug|Release)
            ;;
        *)
            fail "--configuration must be Debug or Release"
            ;;
    esac
}

configure_architectures() {
    if [[ -z "${ARCHITECTURES}" ]]; then
        if [[ "${CONFIGURATION}" == "Release" ]]; then
            ARCHITECTURES="universal"
        else
            ARCHITECTURES="native"
        fi
    fi

    case "${ARCHITECTURES}" in
        native)
            local native_arch
            native_arch="$(/usr/bin/uname -m)"
            case "${native_arch}" in
                arm64|x86_64)
                    ;;
                *)
                    fail "Unsupported native macOS architecture: ${native_arch}"
                    ;;
            esac
            CMAKE_ARCHITECTURES="${native_arch}"
            XCODE_ARCHITECTURES="${native_arch}"
            EXPECTED_ARCHITECTURES=("${native_arch}")
            ;;
        arm64)
            CMAKE_ARCHITECTURES="arm64"
            XCODE_ARCHITECTURES="arm64"
            EXPECTED_ARCHITECTURES=(arm64)
            ;;
        x86_64)
            CMAKE_ARCHITECTURES="x86_64"
            XCODE_ARCHITECTURES="x86_64"
            EXPECTED_ARCHITECTURES=(x86_64)
            ;;
        universal)
            CMAKE_ARCHITECTURES="arm64;x86_64"
            XCODE_ARCHITECTURES="arm64 x86_64"
            EXPECTED_ARCHITECTURES=(arm64 x86_64)
            ;;
        *)
            fail "--architectures must be native, arm64, x86_64, or universal"
            ;;
    esac
}

build_runtime() {
    run cmake \
        -S "${REPO_ROOT}" \
        -B "${CMAKE_BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
        -DCMAKE_OSX_ARCHITECTURES="${CMAKE_ARCHITECTURES}"
    run cmake --build "${CMAKE_BUILD_DIR}" --target oxrsys_runtime
}

build_home() {
    run /usr/bin/xcodebuild \
        -project "${REPO_ROOT}/clients/home/OXRSys Home.xcodeproj" \
        -scheme "OXRSys Home" \
        -configuration "${CONFIGURATION}" \
        -destination "platform=macOS" \
        -derivedDataPath "${HOME_DERIVED_DATA}" \
        ARCHS="${XCODE_ARCHITECTURES}" \
        ONLY_ACTIVE_ARCH=NO \
        CODE_SIGNING_ALLOWED=NO \
        build
}

validate_binary_architectures() {
    local binary_path="$1"
    local label="$2"
    local actual_architectures
    actual_architectures="$(/usr/bin/lipo -archs "${binary_path}")" \
        || fail "Could not inspect ${label}: ${binary_path}"

    local expected_arch
    for expected_arch in "${EXPECTED_ARCHITECTURES[@]}"; do
        if [[ " ${actual_architectures} " != *" ${expected_arch} "* ]]; then
            fail "${label} is missing ${expected_arch}; found: ${actual_architectures}"
        fi
    done
}

home_executable_path() {
    local executable_name
    executable_name="$(/usr/libexec/PlistBuddy \
        -c 'Print :CFBundleExecutable' \
        "$(home_app_path)/Contents/Info.plist")" \
        || fail "Could not read CFBundleExecutable from $(home_app_path)"
    print -r -- "$(home_app_path)/Contents/MacOS/${executable_name}"
}

validate_build_outputs() {
    [[ -f "$(runtime_dylib_path)" ]] || fail "Runtime dylib not found: $(runtime_dylib_path)"
    [[ -f "$(runtime_manifest_path)" ]] || fail "Runtime manifest not found: $(runtime_manifest_path)"
    [[ -d "$(home_app_path)" ]] || fail "Home app not found: $(home_app_path)"
    [[ -f "$(home_executable_path)" ]] || fail "Home executable not found: $(home_executable_path)"

    validate_binary_architectures "$(runtime_dylib_path)" "Runtime dylib"
    validate_binary_architectures "$(home_executable_path)" "Home executable"
}

clean_output_dir() {
    if [[ "${CLEAN_OUTPUT}" -eq 0 ]]; then
        return
    fi

    local normalized_output
    normalized_output="$(absolute_path "${OUTPUT_DIR}")"
    case "${normalized_output}" in
        "${REPO_ROOT}/build/"*)
            ;;
        *)
            fail "Refusing to clean an output directory outside ${REPO_ROOT}/build: ${OUTPUT_DIR}"
            ;;
    esac

    rm -rf "${normalized_output}"
}

assemble_package() {
    local output_runtime_dir="${OUTPUT_DIR}/runtime"
    clean_output_dir
    mkdir -p "${output_runtime_dir}"

    run /usr/bin/ditto "$(home_app_path)" "${OUTPUT_DIR}/OXRSys Home.app"
    run /usr/bin/ditto "$(runtime_dylib_path)" "${output_runtime_dir}/liboxrsys-runtime.dylib"

    local packaged_manifest="${output_runtime_dir}/oxrsys-runtime.json"
    run /usr/bin/ditto "$(runtime_manifest_path)" "${packaged_manifest}"
    run /usr/bin/plutil \
        -replace runtime.library_path \
        -string "./liboxrsys-runtime.dylib" \
        "${packaged_manifest}"

    if [[ -f "$(runtime_config_path)" ]]; then
        run /usr/bin/ditto "$(runtime_config_path)" "${output_runtime_dir}/oxrsys-runtime.toml"
    else
        warn "Runtime config was not found and will not be copied: $(runtime_config_path)"
    fi
}

print_summary() {
    echo
    echo "macOS package assembled:"
    echo "  ${OUTPUT_DIR}"
    echo
    echo "Home app:"
    echo "  ${OUTPUT_DIR}/OXRSys Home.app"
    echo
    echo "Runtime manifest:"
    echo "  ${OUTPUT_DIR}/runtime/oxrsys-runtime.json"
}

main() {
    parse_args "$@"
    require_macos
    require_tool cmake
    require_tool /usr/bin/ditto
    require_tool /usr/bin/plutil
    require_tool /usr/bin/lipo
    require_tool /usr/libexec/PlistBuddy
    require_tool /usr/bin/xcodebuild
    validate_configuration
    configure_architectures

    if [[ "${BUILD_RUNTIME}" -eq 1 ]]; then
        build_runtime
    fi
    if [[ "${BUILD_HOME}" -eq 1 ]]; then
        build_home
    fi

    validate_build_outputs
    assemble_package
    print_summary
}

main "$@"
