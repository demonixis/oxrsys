# Build

## Scope

This document is the entry point for build workflows. Installation steps live in [install.md](install.md). Platform-specific steps live under `docs/platforms/`.

## Before You Build

- Install the required tools first: [install.md](install.md)
- The checked-in Xcode projects intentionally do not contain a personal Apple development team.
  The macOS Home Debug build disables Xcode signing so the standard build command is
  non-interactive. Sign direct-distribution app bundles outside the checked-in build workflow.
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

## Versioning

Product versioning is centralized in `config/OXRSysVersion.xcconfig`.

- `OXRSYS_VERSION` is the global marketing version used by the CMake runtime, Xcode app
  `MARKETING_VERSION`, and Android `versionName`.
- `OXRSYS_BUILD` is the global integer build number used by Xcode
  `CURRENT_PROJECT_VERSION`, Android `versionCode`, and the Android native
  `XrApplicationInfo.applicationVersion`.
- For a release, update `OXRSYS_VERSION` when the public version changes and increment
  `OXRSYS_BUILD` for each distributable build.
- Do not edit product versions directly in `.pbxproj`, Gradle, or native source files.
  Streaming protocol versions and the OpenXR manifest `file_format_version` are separate
  compatibility values and are not tied to the product version.

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

The SwiftUI Home app provides a launcher for compatible apps, user-selected runtime manifest
registration, an autosaved server TOML editor, and an optional Developer tab that opens the
integrated simulator:

```bash
xcodebuild -project "clients/Apple/oxrsys-home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  build
```

The default Debug build pre-fills the runtime manifest field with
`build/runtime/oxrsys-runtime.json` for local development. Home-launched apps use the manifest path
shown in that field; Home does not embed, install, or silently prefer another runtime.

### macOS Local Package Folder

Use `scripts/macos_build_package.sh` to build the runtime and Home app without opening Xcode, then
copy the outputs into one local package folder:

```bash
./scripts/macos_build_package.sh
```

The default output is:

```text
build/OXRSys-macOS/
├── OXRSys Home.app
└── runtime/
    ├── liboxrsys-runtime.dylib
    ├── oxrsys-runtime.json
    └── oxrsys-runtime.toml
```

The packaged manifest is rewritten to load `./liboxrsys-runtime.dylib` from the same `runtime/`
directory. Use `--configuration Debug` for a debug package or `--output-dir` for another package
location.

### macOS Release Signing And Notarization

Use `scripts/macos_sign_notarize.sh` for direct-distribution macOS packages. It signs the packaged
runtime dylib and `OXRSys Home.app`, then creates a single zip containing the Home app plus the
`runtime/` folder. The manifest copy in the archive is rewritten to load the packaged dylib with a
relative path; the build-tree manifest is left unchanged.

Signing-only package:

```bash
./scripts/macos_build_package.sh
./scripts/macos_sign_notarize.sh \
  --runtime-dir build/OXRSys-macOS/runtime \
  --home-app "build/OXRSys-macOS/OXRSys Home.app" \
  --identity "Developer ID Application: OXRSys Team (ABCDE12345)"
```

Signing plus notarization:

```bash
./scripts/macos_build_package.sh
./scripts/macos_sign_notarize.sh --notarize \
  --runtime-dir build/OXRSys-macOS/runtime \
  --home-app "build/OXRSys-macOS/OXRSys Home.app" \
  --identity "Developer ID Application: OXRSys Team (ABCDE12345)" \
  --team-id ABCDE12345 \
  --apple-id developer@example.com \
  --password "xxxx-xxxx-xxxx-xxxx"
```

`--apple-id` is the Apple Developer account email and `--password` is an app-specific password for
`notarytool`. If `--identity` is omitted, the script auto-selects the only Developer ID Application
certificate in the keychain; pass it explicitly when more than one identity is installed.

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

The standalone targets are `oxrsys-home` and `oxrsys-simulator`. The Qt Home Developer tab opens
the same shared simulator widget in a dedicated window. FFmpeg development libraries are optional;
when they are found at configure time, the Qt simulator decodes video into the preview surface,
otherwise it stays in tracking-only preview mode with an explicit status message. See
[qt-home.md](platforms/qt-home.md) for Linux registration/install behavior.

### Unity Editor And macOS Player Helpers

If you want to force the runtime only inside a Unity project, use the editor helper documented in
[scripts/README.md](../scripts/README.md). It provides `Tools/OpenXR` menu entries to select and
apply a runtime JSON for the current Unity editor session.

For exported macOS Unity Players, also install
`scripts/unity/Editor/OXRSysMacOpenXRLoaderPostprocessor.cs` into the Unity project's
`Assets/Editor/` folder. Unity's OpenXR package can place the macOS loader at
`Contents/PlugIns/ARM64/libopenxr_loader.dylib`, while `UnityOpenXR.dylib` loads
`Contents/PlugIns/openxr_loader.dylib`. Without the top-level loader, the editor can work while the
exported Player logs `Failed to load openxr runtime loader.` and never starts XR.

The postprocessor copies the package loader to the bundle path Unity loads and ad-hoc signs the
`.app` after the copy. It only fixes the Unity Player bundle layout; the Player still needs runtime
selection through `XR_RUNTIME_JSON`, OXRSys Home, or `scripts/oxrsys_runtime_default.sh`.

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
  Registration launch target. The launcher uses the selected manifest path shown by Home.
- If Android tooling is not found, verify `clients/Android/android-vr/local.properties`, Java 17, and the installed SDK/NDK versions described in [install.md](install.md).
- If the runtime is not discovered, check that `XR_RUNTIME_JSON` or `~/.config/openxr/1/active_runtime.json` points to `build/runtime/oxrsys-runtime.json`.
- If an exported macOS Unity Player logs `Failed to load openxr runtime loader.`, install
  `scripts/unity/Editor/OXRSysMacOpenXRLoaderPostprocessor.cs` in the Unity project and rebuild the
  `.app`.
- If Xcode cannot execute the `metal` tool, install the Metal Toolchain component as described in [install.md](install.md).
- If simulator builds fail with a `CoreSimulator` version mismatch, update Xcode and the simulator runtime components together.
