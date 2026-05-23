## Summary

- Describe the user-visible or developer-visible change.

## Verification

- [ ] `scripts/ci/install-commit-msg-hook.sh` for local commit-msg enforcement when needed
- [ ] `python3 scripts/ci/commitlint.py range <base-sha> <head-sha>` when commit history changed
- [ ] `scripts/ci/build-android.sh` when Android code changed
- [ ] `scripts/ci/build-companion.sh` when companion app code changed
- [ ] `scripts/ci/build-simulator.sh` when simulator code changed
- [ ] `scripts/ci/build-visionos.sh` when visionOS code changed
- [ ] `scripts/ci/verify-macos-runtime-heavy.sh` for runtime, CMake, or protocol-sensitive changes

## Release Notes

- Conventional commit title used for squash or final merge commit: `type(scope): summary`
