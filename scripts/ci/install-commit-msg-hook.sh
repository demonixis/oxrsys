#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HOOK_PATH="${REPO_ROOT}/.git/hooks/commit-msg"

printf '==> Installing commit-msg hook at %s\n' "${HOOK_PATH}"
cat > "${HOOK_PATH}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
exec python3 "${REPO_ROOT}/scripts/ci/commitlint.py" edit "$1"
EOF

chmod +x "${HOOK_PATH}"
printf '==> commit-msg hook installed\n'
