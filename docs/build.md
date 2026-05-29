# Build

## Scope

This document is the entry point for build workflows. Installation steps live in [install.md](install.md). Platform-specific steps live under `docs/platforms/`.

## Before You Build

- Install the required tools first: [install.md](install.md)
- The checked-in Xcode projects intentionally do not contain a personal Apple development team.
  The macOS Home Debug build disables Xcode signing so the standard build command is
  non-interactive. Use `scripts/package_home.sh` with `CODE_SIGN_IDENTITY` when preparing a
  direct-distribution app.
- For Xcode UI work on multiple Swift clients, open `clients/Apple/OXRSys Clients.xcworkspace`
  instead of opening the individual `.xcodeproj` files in separate windows. The simulator and
  visionOS targets share local Swift packages, and one workspace avoids Xcode loading them from
  multiple project containers.
- Use the platform pages for client-specific build and deployment details:
  - [Quest](platforms/quest.md)
  - [iOS Viewer](platforms/ios-viewer.md)
  - [Simulator](simulator.md)
  - [Vision OS](platforms/visionos.md)
  - [macOS Home](platforms/macos-home.md)
  - [Qt Home](platforms/qt-home.md)

## Build The Runtime

The default configure is host-native. Presets are shortcuts, not the source of truth for the target OS or architecture.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Equivalent preset form:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

macOS explicit architecture presets:

```bash
cmake --preset macos-arm64
cmake --preset macos-x64
cmake --preset macos-universal
```

Linux native preset:

```bash
cmake --preset linux-native
cmake --build build/linux-native
ctest --test-dir build/linux-native --output-on-failure
```

Key outputs in the selected build directory. With the default build these are under `build/runtime`; with a preset they are under `build/<preset>/runtime`.

- `build/runtime/liboxrsys-runtime.dylib`
- `build/runtime/liboxrsys-runtime.so` on Linux
- `build/runtime/oxrsys-runtime.json`
- `build/runtime/oxrsys-runtime.toml`
- `compile_commands.json` symlinked at the project root for editor integration

All third-party C++ dependencies are fetched through CMake `FetchContent`.
Linux additionally requires system/toolchain packages for Vulkan headers, FFmpeg development libraries, and pkg-config.

Windows is a scaffold only in this pass; do not treat Windows runtime builds as an acceptance gate yet.

## Run The Runtime

For terminal-launched applications:

```bash
export XR_RUNTIME_JSON=$(pwd)/build/runtime/oxrsys-runtime.json
```

For GUI applications such as Unity, Steam, or Godot launched outside a shell:

```bash
./scripts/oxrsys_runtime_default.sh set
./scripts/oxrsys_runtime_default.sh status
./scripts/oxrsys_runtime_default.sh unset
```

The helper creates `~/.config/openxr/1/active_runtime.json` and installs a per-user LaunchAgent that restores `XR_RUNTIME_JSON` for GUI sessions.

### Native macOS Home App

The SwiftUI Home app provides a launcher for compatible apps, a runtime installer, the server
TOML editor, the per-user runtime registration workflow, and an optional Developer tab that opens
the integrated simulator:

```bash
xcodebuild -project "clients/Apple/oxrsys-home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  build
```

The default Debug build does not embed the runtime. It falls back to `build/runtime/oxrsys-runtime.json`
when no installed runtime is available.

To build a direct-distribution Home bundle with the runtime copied into
`Contents/Resources/OXRSysRuntime`:

```bash
scripts/package_home.sh
```

Set `CODE_SIGN_IDENTITY="Developer ID Application: ..."` to sign the packaged app with Hardened
Runtime options. See [macos-home.md](platforms/macos-home.md) for the launcher,
installation, and signing workflow.

### Unified Viewer App

The shared simulator package can be checked directly:

```bash
swift build --package-path clients/Apple/common/OXRSysSimulator
```

The unified viewer target under `clients/Apple/oxrsys-simulator/` wraps that package for both the standalone
macOS simulator workflow and the iOS stereo viewer workflow:

```bash
xcodebuild -project "clients/Apple/oxrsys-simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  build
```

Optional iOS build:

```bash
xcodebuild -project "clients/Apple/oxrsys-simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  build
```

See [simulator.md](simulator.md) for the simulator mode details and [ios-viewer.md](platforms/ios-viewer.md) for the `StereoView` workflow.

### Vision OS Viewer

The native visionOS viewer under `clients/Apple/oxrsys-visionos/` reuses the shared streaming package for discovery, decode, and transport:

```bash
xcodebuild -project "clients/Apple/oxrsys-visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  build
```

See [visionos.md](platforms/visionos.md) for the current workflow and limits.

For TestFlight, archive with `-destination 'generic/platform=visionOS'`. The visionOS target does
not use macOS-only `LSApplicationCategoryType` or App Sandbox settings.

### Qt Frontends

The Qt apps live under `clients/Qt/`. They build automatically on Linux when Qt6 is found. On macOS or Windows, enable them explicitly:

```bash
cmake -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_BUILD_QT_FRONTENDS=ON
cmake --build build-qt
ctest --test-dir build-qt --output-on-failure
```

The standalone targets are `oxrsys-home` and `oxrsys-simulator`. See [qt-home.md](platforms/qt-home.md) for Linux registration/install behavior.

### Unity Editor Helper

If you want to force the runtime only inside a Unity project, use the editor helper documented in [scripts/README.md](../scripts/README.md). It provides `Tools/OpenXR` menu entries to select and apply a runtime JSON for the current Unity editor session.

## Platform Pages

- Quest Android client: [quest.md](platforms/quest.md)
- Unified simulator/viewer app: [simulator.md](simulator.md)
- iOS `StereoView` workflow: [ios-viewer.md](platforms/ios-viewer.md)
- visionOS viewer: [visionos.md](platforms/visionos.md)
- macOS Home app: [macos-home.md](platforms/macos-home.md)
- Qt Home app: [qt-home.md](platforms/qt-home.md)

## Troubleshooting

- If a GUI app does not pick up the runtime, use `scripts/oxrsys_runtime_default.sh` instead of relying on shell startup files.
- If a Home-launched app does not pick up the runtime, check the Apps tab logs and the Runtime
  tab launch target. The launcher prefers the installed manifest, then a selected manifest, then
  `build/runtime/oxrsys-runtime.json`.
- If Android tooling is not found, verify `clients/Android/android-vr/local.properties`, Java 17, and the installed SDK/NDK versions described in [install.md](install.md).
- If the runtime is not discovered, check that `XR_RUNTIME_JSON` or `~/.config/openxr/1/active_runtime.json` points to `build/runtime/oxrsys-runtime.json`.
- If Xcode cannot execute the `metal` tool, install the Metal Toolchain component as described in [install.md](install.md).
- If simulator builds fail with a `CoreSimulator` version mismatch, update Xcode and the simulator runtime components together.
