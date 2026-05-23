## Summary

- Describe the user-visible or developer-visible change.

## Verification

- [ ] `scripts/ci/install-commit-msg-hook.sh` for local commit-msg enforcement when needed
- [ ] `scripts/ci/verify-pr-lightweight.sh` for the always-on pull request lanes
- [ ] `scripts/ci/verify-macos-runtime-heavy.sh` for runtime-sensitive changes: `runtime/**`, `tests/**`, `cmake/**`, `CMakeLists.txt`, or shared protocol files

## Release Notes

- Conventional commit title used for squash or final merge commit: `type(scope): summary`
