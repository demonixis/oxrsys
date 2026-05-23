#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command java

log "Building Android client"
cd "${REPO_ROOT}/clients/android-openxr"
run ./gradlew --no-daemon :app:assembleDebug
