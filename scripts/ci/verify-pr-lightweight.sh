#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

BASE_REF="${OPENXR_OSX_COMMITLINT_BASE_REF:-origin/main}"
HEAD_REF="${OPENXR_OSX_COMMITLINT_HEAD_REF:-HEAD}"
RUN_COMMITLINT=1
RUN_ANDROID=1
RUN_COMPANION=1
RUN_SIMULATOR=1
RUN_VISIONOS=1

usage() {
    cat <<'EOF'
Usage: scripts/ci/verify-pr-lightweight.sh [options]

Run the same lightweight verification lanes used on pull requests:
  - commitlint on the current branch commit range
  - Android client build
  - macOS companion app build
  - macOS simulator app build
  - visionOS player build

Options:
  --base-ref REF         Base ref for commitlint range. Default: origin/main
  --head-ref REF         Head ref for commitlint range. Default: HEAD
  --skip-commitlint      Skip commit message validation
  --skip-android         Skip Android client build
  --skip-companion       Skip macOS companion build
  --skip-simulator       Skip macOS simulator build
  --skip-visionos        Skip visionOS player build
  -h, --help             Show this help text

Environment:
  OPENXR_OSX_COMMITLINT_BASE_REF
  OPENXR_OSX_COMMITLINT_HEAD_REF
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base-ref)
            [[ $# -ge 2 ]] || die "missing value for --base-ref"
            BASE_REF="$2"
            shift 2
            ;;
        --head-ref)
            [[ $# -ge 2 ]] || die "missing value for --head-ref"
            HEAD_REF="$2"
            shift 2
            ;;
        --skip-commitlint)
            RUN_COMMITLINT=0
            shift
            ;;
        --skip-android)
            RUN_ANDROID=0
            shift
            ;;
        --skip-companion)
            RUN_COMPANION=0
            shift
            ;;
        --skip-simulator)
            RUN_SIMULATOR=0
            shift
            ;;
        --skip-visionos)
            RUN_VISIONOS=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

if [[ "${RUN_COMMITLINT}" -eq 1 ]]; then
    require_command git
    require_command python3

    local_base="${BASE_REF}"
    local_head="${HEAD_REF}"

    if ! git rev-parse --verify --quiet "${local_base}" >/dev/null; then
        if git rev-parse --verify --quiet main >/dev/null; then
            local_base="main"
        else
            die "unable to resolve commitlint base ref: ${BASE_REF}"
        fi
    fi

    log "Linting commits between ${local_base} and ${local_head}"
    merge_base="$(git merge-base "${local_base}" "${local_head}")"
    run python3 "${REPO_ROOT}/scripts/ci/commitlint.py" range "${merge_base}" "${local_head}"
fi

if [[ "${RUN_ANDROID}" -eq 1 ]]; then
    run "${REPO_ROOT}/scripts/ci/build-android.sh"
fi

if [[ "${RUN_COMPANION}" -eq 1 ]]; then
    run "${REPO_ROOT}/scripts/ci/build-companion.sh"
fi

if [[ "${RUN_SIMULATOR}" -eq 1 ]]; then
    run "${REPO_ROOT}/scripts/ci/build-simulator.sh"
fi

if [[ "${RUN_VISIONOS}" -eq 1 ]]; then
    run "${REPO_ROOT}/scripts/ci/build-visionos.sh"
fi
