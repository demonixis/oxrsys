#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

require_command xcodebuild

log "Building macOS simulator app"
cd "${REPO_ROOT}"
CI_HOME="$(derived_data_path simulator)/home"
mkdir -p "${CI_HOME}"
run /usr/bin/env \
    HOME="${CI_HOME}" \
    xcodebuild \
    -project "clients/simulator/OpenXR Simulator.xcodeproj" \
    -scheme "OpenXR Simulator" \
    -configuration Debug \
    -destination "platform=macOS" \
    -derivedDataPath "$(derived_data_path simulator)" \
    CODE_SIGNING_ALLOWED=NO \
    build
