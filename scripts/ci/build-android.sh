#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command java
require_command sdkmanager

ensure_android_packages
write_android_local_properties

log "Building Android client"
cd "${REPO_ROOT}/clients/android-openxr"
run ./gradlew --no-daemon :app:assembleDebug
