#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command xcodebuild

log "Building macOS companion app"
cd "${REPO_ROOT}"
CI_HOME="$(derived_data_path companion)/home"
mkdir -p "${CI_HOME}"
run /usr/bin/env \
    HOME="${CI_HOME}" \
    xcodebuild \
    -project "clients/companion/OpenXR OSX Companion.xcodeproj" \
    -scheme "OpenXR OSX Companion" \
    -configuration Debug \
    -derivedDataPath "$(derived_data_path companion)" \
    CODE_SIGNING_ALLOWED=NO \
    build
