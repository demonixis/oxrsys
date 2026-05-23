# Scripts

## CI

The checked-in scripts under `scripts/ci/` are the local entry points for the repository's CI lanes. Keep GitHub Actions steps thin and route build, verification, and packaging logic through these scripts so contributors can reproduce CI behavior without copying workflow YAML by hand.

### Local PR Verification

Use the lightweight PR wrapper to reproduce the always-on pull request checks locally:

```bash
scripts/ci/verify-pr-lightweight.sh
```

That wrapper runs:

- commitlint for the current branch range against `origin/main`
- Android client build
- macOS companion app build
- macOS simulator app build
- visionOS player build

Useful flags:

- `--base-ref <ref>` to lint against a different PR base
- `--head-ref <ref>` to lint a different branch head
- `--skip-android` when you are not validating Android locally
- `--skip-visionos` when the local Xcode install does not include visionOS simulator support

For the path-triggered runtime lane, run:

```bash
scripts/ci/verify-macos-runtime-heavy.sh
```

That script reproduces the heavy macOS runtime CI lane, including host bootstrap for Metal Toolchain and Vulkan prerequisites unless `OPENXR_OSX_SKIP_HOST_BOOTSTRAP=1` is already appropriate for the current machine.

### Release Packaging

To reproduce release artifact assembly locally, run:

```bash
scripts/ci/package-release-assets.sh
```

This packages the runtime zip, Quest APK, macOS Companion app, Simulator app, and `SHA256SUMS.txt` under `dist/release/`.

### visionOS Platform Support

`scripts/ci/build-visionos.sh` probes for installed visionOS platform support before building.

- On GitHub Actions, it may download the visionOS platform automatically if the runner image is missing it.
- Locally, it fails with the exact `xcodebuild -downloadPlatform visionOS` command unless you opt in with `OPENXR_OSX_ALLOW_VISIONOS_PLATFORM_DOWNLOAD=1`.
- If the build fails after the probe step, the problem is in the Xcode/CoreSimulator environment or the project itself, not in platform detection.

## Companion Packaging

`scripts/package_companion.sh` builds the runtime, builds the macOS companion without Xcode signing,
copies the runtime files into `OpenXR OSX Companion.app/Contents/Resources/OpenXRRuntime`, and
optionally signs the app when `CODE_SIGN_IDENTITY` is set.

```bash
scripts/package_companion.sh
CODE_SIGN_IDENTITY="Developer ID Application: Example Team" scripts/package_companion.sh
```
## Unity

`scripts/unity/Editor/OpenXRRuntimeAutoSelector.cs` is a Unity Editor helper that forces the OpenXR runtime JSON for the current Unity editor process.

### What It Does

- sets `XR_RUNTIME_JSON`
- sets `XR_SELECTED_RUNTIME_JSON`
- sets `OTHER_XR_RUNTIME_JSON`
- remembers the selected path in `EditorPrefs`
- uses `XR_RUNTIME_JSON` as the initial runtime path when it is already set

### How To Use It

1. Copy `scripts/unity/Editor/OpenXRRuntimeAutoSelector.cs` into the `Assets/Editor/` folder of your Unity project.
2. Build the runtime so `build/runtime/openxr_osx.json` exists.
3. Open the Unity editor.
4. Use one of the menu entries under `Tools/OpenXR`:
   - `Use OpenXR OSX Runtime`
   - `Choose Custom Runtime Json...`
   - `Clear Forced Runtime`
   - `Log Active Runtime`

`Use OpenXR OSX Runtime` applies `XR_RUNTIME_JSON` when it points to an existing manifest. If it is not set, the helper asks you to choose the generated `openxr_osx.json`.

### When To Use It

Use this helper when you want Unity to target the local runtime JSON without relying on a shell environment or the user-wide runtime registration helper.

For system-wide registration outside Unity, use `scripts/openxr_runtime_default.sh` instead.
