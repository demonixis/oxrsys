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
