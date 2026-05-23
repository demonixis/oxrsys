# OpenXR OSX Runtime

OpenXR runtime for macOS that brings PC-style OpenXR support to Apple Silicon Macs without Windows.
The project currently combines a macOS runtime, a unified macOS/iOS viewer with `Simulator` and
`StereoView` modes, a first-pass visionOS viewer, and a Quest/Pico-oriented streaming stack with an
Android client scaffold.
The repository also includes a native SwiftUI macOS companion app for compatible app launching,
runtime installation, runtime configuration, and runtime registration workflows.

**Current state:** Metal/core runtime, Vulkan interop, controller and hand input paths, loader-backed
runtime tests, `XR_EXT_conformance_automation`, `XR_EXT_hand_interaction`, and `XR_EXT_debug_utils`
are in place. The Quest/Android client now feeds real `XR_EXT_hand_tracking` joints into the runtime,
supports WiFi UDP and USB ADB reverse TCP streaming, matches per-frame render poses for headset
compositor reprojection, enables a first-pass dynamic `XR_FB_foveation` path when the headset supports it,
and can request a build-configured display refresh rate. The visionOS
viewer now starts from a minimal floating search window, enters immersive VR automatically when the
stream connects, and sends head pose, hand joints, and first-pass tracked accessory controller data
while the immersive space is open. The macOS SwiftUI companion now targets direct notarized
distribution so it can scan known apps, install the bundled runtime, launch compatible apps with
`XR_RUNTIME_JSON`, and capture app logs.
The companion shows a main-window runtime activity summary from
`~/Library/Application Support/OpenXR-OSX/runtime_status.json`, including idle/streaming state,
transport, connected device family, active OpenXR application, and WiFi/USB transport readiness.
As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is fully green locally:
63 passed, 36 skipped, 0 failed.

## Repository Rules

- **Always build and verify before declaring success** — run the macOS build + tests and/or Android build as appropriate before saying everything works
- **Always update `README.md`, `AGENTS.md`, and the relevant files in `docs/` when making significant project changes**
- If a branch contains `scripts/ci/`, treat those checked-in scripts as the source of truth for CI entry points. Do not document or recommend workflow-only commands that cannot be run locally.
- All dependencies are fetched via CMake FetchContent
- All source code and documentation must be in English
- Project-owned source code is licensed under MPL-2.0; preserve SPDX headers and keep third-party code under its upstream license.

## Quality Bar

- Keep patches focused and avoid silent behavior changes.
- Add or update tests when behavior changes. If a test cannot be added, explain why.
- Preserve non-blocking frame submission and latency-sensitive paths.
- Do not add new dependencies without a clear reason.

## Documentation Rules

`README.md` must stay short. Put detailed build, platform, protocol, simulator, and test guidance in `docs/`.

Avoid duplicating the same guidance in multiple files. If commands, platform status, or CTS results change, update the single page that owns that topic and keep cross-links accurate.

Keep binary-first release guidance in `docs/releases.md`, host prerequisite details in `docs/install.md`, and source build commands in `docs/build.md`.

## Important Technical Constraints

- The runtime does not link directly against Vulkan. Resolve Vulkan functions through the app-provided loader path.
- `Session::EndFrame()` must stay non-blocking.
- The streaming encoder queue is latest-frame-only.
- Quest USB streaming uses reconnecting ADB reverse TCP on localhost ports `9944`, `9945`, and `9946`; app-level Android USB permission dialogs are only for `UsbManager`-visible devices/accessories and are not required for ADB reverse streaming.
- Headset refresh rate is negotiated from the client.
- The Quest Android client requests its preferred display refresh rate from the build-time `OPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ` value.
- Latency reports feed bounded pose prediction.
- Headset clients must match `VIDEO_FLAG_RENDER_POSE` metadata to the decoded frame before projection submission.
- Quest hand tracking depends on the Android manifest permission `com.oculus.permission.HAND_TRACKING` and the optional `oculus.software.handtracking` feature.
- The action system is profile-aware and must not regress to hard-forcing `KHR simple_controller`.
- `xrLocateSpacesKHR` is accepted as an alias of the OpenXR 1.1 `xrLocateSpaces` entry point.
- Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.
- Runtime configuration is loaded from `~/Library/Application Support/OpenXR-OSX/openxr_osx.toml` with compatibility fallback for older local paths.

## Project Layout

```text
openxr_osx/
├── CMakeLists.txt
├── cmake/RunOpenXRCTS.cmake
├── runtime/
├── clients/
│   ├── common/src/Protocol.h
│   ├── android-openxr/
│   ├── simulator/
│   ├── companion/
│   └── visionos/
├── tests/
│   ├── TestConfig.cpp
│   ├── TestInputManager.cpp
│   ├── CompanionLauncherTests.swift
│   ├── TestProtocolLayout.cpp
│   └── TestRuntimeApi.cpp
└── docs/
```

## Verification Commands

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
swiftc -parse-as-library \
  "clients/companion/OpenXR OSX Companion/CompanionSupport.swift" \
  "clients/companion/OpenXR OSX Companion/OpenXRServerConfig.swift" \
  "clients/companion/OpenXR OSX Companion/CompanionLauncher.swift" \
  tests/CompanionLauncherTests.swift \
  -o /tmp/openxr_companion_launcher_tests && /tmp/openxr_companion_launcher_tests
xcodebuild -project "clients/companion/OpenXR OSX Companion.xcodeproj" \
  -scheme "OpenXR OSX Companion" \
  -configuration Debug \
  build

xcodebuild -project "clients/simulator/OpenXR Simulator.xcodeproj" \
  -scheme "OpenXR Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  build

xcodebuild -project "clients/visionos/Vision Player.xcodeproj" \
  -scheme "Vision Player" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  build
```

Optional CTS lane:

```bash
cmake -B build_cts -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENXR_OSX_ENABLE_CTS=ON
cmake --build build_cts --target openxr_cts_run
```
