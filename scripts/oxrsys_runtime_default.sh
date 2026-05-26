#!/bin/zsh
# SPDX-License-Identifier: MPL-2.0


set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_MANIFEST="${REPO_ROOT}/build/runtime/oxrsys-runtime.json"
ACTIVE_RUNTIME_DIR="${HOME}/.config/openxr/1"
ACTIVE_RUNTIME_LINK="${ACTIVE_RUNTIME_DIR}/active_runtime.json"
LAUNCH_AGENTS_DIR="${HOME}/Library/LaunchAgents"
LAUNCH_AGENT_PLIST="${LAUNCH_AGENTS_DIR}/net.demonixis.oxrsys.runtime-env.plist"

usage() {
    cat <<EOF
Usage:
  $(basename "$0") set [manifest.json]
  $(basename "$0") unset
  $(basename "$0") status [manifest.json]

Commands:
  set     Register the manifest as the default OpenXR runtime for this user.
          Also installs a LaunchAgent that restores XR_RUNTIME_JSON for GUI apps at login.
  unset   Remove the user-level active runtime link and the LaunchAgent.
  status  Print the current active runtime link and GUI environment state.
EOF
}

require_manifest() {
    local manifest_path="$1"

    if [[ ! -f "${manifest_path}" ]]; then
        echo "Manifest not found: ${manifest_path}" >&2
        exit 1
    fi
}

write_launch_agent() {
    local manifest_path="$1"

    mkdir -p "${LAUNCH_AGENTS_DIR}"

    cat > "${LAUNCH_AGENT_PLIST}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>net.demonixis.oxrsys.runtime-env</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/launchctl</string>
        <string>setenv</string>
        <string>XR_RUNTIME_JSON</string>
        <string>${manifest_path}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
EOF
}

set_runtime() {
    local manifest_path="${1:-${DEFAULT_MANIFEST}}"
    require_manifest "${manifest_path}"

    mkdir -p "${ACTIVE_RUNTIME_DIR}"
    ln -sfn "${manifest_path}" "${ACTIVE_RUNTIME_LINK}"

    write_launch_agent "${manifest_path}"
    launchctl unload "${LAUNCH_AGENT_PLIST}" >/dev/null 2>&1 || true
    launchctl load "${LAUNCH_AGENT_PLIST}"
    launchctl setenv XR_RUNTIME_JSON "${manifest_path}"

    echo "Default OpenXR runtime set to:"
    echo "  ${manifest_path}"
    echo
    echo "Active runtime link:"
    echo "  ${ACTIVE_RUNTIME_LINK} -> $(readlink "${ACTIVE_RUNTIME_LINK}")"
    echo
    echo "GUI session XR_RUNTIME_JSON:"
    echo "  $(launchctl getenv XR_RUNTIME_JSON)"
}

unset_runtime() {
    rm -f "${ACTIVE_RUNTIME_LINK}"
    launchctl unsetenv XR_RUNTIME_JSON >/dev/null 2>&1 || true
    launchctl unload "${LAUNCH_AGENT_PLIST}" >/dev/null 2>&1 || true
    rm -f "${LAUNCH_AGENT_PLIST}"

    echo "Removed default OpenXR runtime registration for this user."
}

status_runtime() {
    local expected_manifest="${1:-${DEFAULT_MANIFEST}}"

    echo "Expected manifest:"
    echo "  ${expected_manifest}"
    echo

    if [[ -L "${ACTIVE_RUNTIME_LINK}" || -f "${ACTIVE_RUNTIME_LINK}" ]]; then
        echo "Active runtime link:"
        echo "  ${ACTIVE_RUNTIME_LINK} -> $(readlink "${ACTIVE_RUNTIME_LINK}")"
    else
        echo "Active runtime link:"
        echo "  not set"
    fi

    echo
    if [[ -f "${LAUNCH_AGENT_PLIST}" ]]; then
        echo "LaunchAgent:"
        echo "  ${LAUNCH_AGENT_PLIST}"
    else
        echo "LaunchAgent:"
        echo "  not installed"
    fi

    echo
    echo "GUI session XR_RUNTIME_JSON:"
    echo "  $(launchctl getenv XR_RUNTIME_JSON || true)"
}

COMMAND="${1:-}"

case "${COMMAND}" in
    set)
        shift
        set_runtime "${1:-${DEFAULT_MANIFEST}}"
        ;;
    unset)
        unset_runtime
        ;;
    status)
        shift
        status_runtime "${1:-${DEFAULT_MANIFEST}}"
        ;;
    ""|-h|--help|help)
        usage
        ;;
    *)
        echo "Unknown command: ${COMMAND}" >&2
        echo >&2
        usage >&2
        exit 1
        ;;
esac
