#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command xcodebuild

log "Building visionOS player"
cd "${REPO_ROOT}"
CI_HOME="$(derived_data_path visionos)/home"
mkdir -p "${CI_HOME}"
run /usr/bin/env \
    HOME="${CI_HOME}" \
    xcodebuild \
    -project "clients/visionos/Vision Player.xcodeproj" \
    -scheme "Vision Player" \
    -configuration Debug \
    -destination "generic/platform=visionOS Simulator" \
    -derivedDataPath "$(derived_data_path visionos)" \
    CODE_SIGNING_ALLOWED=NO \
    build
