# Build

## Scope

This document is the entry point for source build workflows. If a tagged binary already covers your use case, start with [releases.md](releases.md) instead. Installation steps live in [install.md](install.md). Platform-specific steps live under `docs/platforms/`.

## Before You Build

- Install the required tools first: [install.md](install.md)
- Install the pinned portable tools from the repo root with `mise install` if you want the documented Java/CMake/Ninja/Android SDK versions.
- The checked-in Xcode projects intentionally do not contain a personal Apple development team.
  The macOS companion Debug build disables Xcode signing so the standard build command is
  non-interactive. Use `scripts/package_companion.sh` with `CODE_SIGN_IDENTITY` when preparing a
  direct-distribution app.
- For Xcode UI work on multiple Swift clients, open `clients/OpenXR Clients.xcworkspace`
  instead of opening the individual `.xcodeproj` files in separate windows. The simulator and
  visionOS targets share the local `OpenXRStreaming` Swift package, and one workspace avoids Xcode
  loading that package from multiple project containers.
- Use the platform pages for client-specific build and deployment details:
  - [Quest](platforms/quest.md)
  - [iOS Viewer](platforms/ios-viewer.md)
  - [Simulator](simulator.md)
  - [Vision OS](platforms/visionos.md)
  - [macOS Companion](platforms/macos-companion.md)

If your branch contains `scripts/ci/`, treat those checked-in scripts as the automation entry points and keep CI docs aligned with them. This branch currently documents the raw local build commands below.

## Build The macOS Runtime

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Key outputs:

- `build/runtime/libopenxr_osx.dylib`
- `build/runtime/openxr_osx.json`
- `build/runtime/openxr_osx.toml`
- `compile_commands.json` symlinked at the project root for editor integration

All third-party C++ dependencies are fetched through CMake `FetchContent`.

## Run The Runtime

For terminal-launched applications:

```bash
export XR_RUNTIME_JSON=$(pwd)/build/runtime/openxr_osx.json
```

For GUI applications such as Unity, Steam, or Godot launched outside a shell:

```bash
./scripts/openxr_runtime_default.sh set
./scripts/openxr_runtime_default.sh status
./scripts/openxr_runtime_default.sh unset
```

The helper creates `~/.config/openxr/1/active_runtime.json` and installs a per-user LaunchAgent that restores `XR_RUNTIME_JSON` for GUI sessions.

### Native macOS Companion App

The SwiftUI companion app provides a launcher for compatible apps, a runtime installer, the server
TOML editor, and the per-user runtime registration workflow:

```bash
xcodebuild -project "clients/companion/OpenXR OSX Companion.xcodeproj" \
  -scheme "OpenXR OSX Companion" \
  -configuration Debug \
  build
```

The default Debug build does not embed the runtime. It falls back to `build/runtime/openxr_osx.json`
when no installed runtime is available.

To build a direct-distribution companion bundle with the runtime copied into
`Contents/Resources/OpenXRRuntime`:

```bash
scripts/package_companion.sh
```

Set `CODE_SIGN_IDENTITY="Developer ID Application: ..."` to sign the packaged app with Hardened
Runtime options. See [macos-companion.md](platforms/macos-companion.md) for the launcher,
installation, and signing workflow.

### Unified Viewer App

The unified viewer target under `clients/simulator/` now covers both the local simulator workflow and the iOS stereo viewer workflow:

```bash
xcodebuild -project "clients/simulator/OpenXR Simulator.xcodeproj" \
  -scheme "OpenXR Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  build
```

Optional iOS build:

```bash
xcodebuild -project "clients/simulator/OpenXR Simulator.xcodeproj" \
  -scheme "OpenXR Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  build
```

See [simulator.md](simulator.md) for the simulator mode details and [ios-viewer.md](platforms/ios-viewer.md) for the `StereoView` workflow.

### Vision OS Viewer

The native visionOS viewer under `clients/visionos/` reuses the shared streaming package for discovery, decode, and transport:

```bash
xcodebuild -project "clients/visionos/Vision Player.xcodeproj" \
  -scheme "Vision Player" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  build
```

See [visionos.md](platforms/visionos.md) for the current workflow and limits.

For TestFlight, archive with `-destination 'generic/platform=visionOS'`. The visionOS target does
not use macOS-only `LSApplicationCategoryType` or App Sandbox settings.

### Unity Editor Helper

If you want to force the runtime only inside a Unity project, use the editor helper documented in [scripts/README.md](../scripts/README.md). It provides `Tools/OpenXR` menu entries to select and apply a runtime JSON for the current Unity editor session.

## Platform Pages

- Quest Android client: [quest.md](platforms/quest.md)
- Unified simulator/viewer app: [simulator.md](simulator.md)
- iOS `StereoView` workflow: [ios-viewer.md](platforms/ios-viewer.md)
- visionOS viewer: [visionos.md](platforms/visionos.md)
- macOS companion app: [macos-companion.md](platforms/macos-companion.md)

## Troubleshooting

- If a GUI app does not pick up the runtime, use `scripts/openxr_runtime_default.sh` instead of relying on shell startup files.
- If a companion-launched app does not pick up the runtime, check the Apps tab logs and the Runtime
  tab launch target. The launcher prefers the installed manifest, then a selected manifest, then
  `build/runtime/openxr_osx.json`.
- If Android tooling is not found, verify `clients/android-openxr/local.properties`, Java 17, and the installed SDK/NDK versions described in [install.md](install.md).
- If the runtime is not discovered, check that `XR_RUNTIME_JSON` or `~/.config/openxr/1/active_runtime.json` points to `build/runtime/openxr_osx.json`.
- If Xcode cannot execute the `metal` tool, install the Metal Toolchain component as described in [install.md](install.md).
- If simulator builds fail with a `CoreSimulator` version mismatch, update Xcode and the simulator runtime components together.
