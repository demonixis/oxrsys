# OXRSys Runtime

OpenXR runtime that started on macOS and is now being moved toward a measured cross-platform
runtime shape. The project currently combines the shared runtime, a unified macOS/iOS viewer with
`Simulator` and `StereoView` modes, a first-pass visionOS viewer, a Quest/Pico-oriented Android VR
client, and Linux-first Qt frontends.
The repository also includes a native SwiftUI macOS Home app and a Qt Home app for compatible app
launching, runtime selection, runtime configuration, and runtime registration workflows.

**Current state:** Metal/core runtime, Vulkan interop, Linux Vulkan/FFmpeg scaffolding,
typed internal graphics/frame plumbing, release-time Metal streaming snapshots,
portable platform/socket helpers,
controller and hand input paths, loader-backed
runtime tests, `XR_EXT_conformance_automation`, `XR_EXT_hand_interaction`, and `XR_EXT_debug_utils`
are in place. Windows is scaffolded in layout/docs only for this pass. The Android VR client now feeds
real `XR_EXT_hand_tracking` joints into the runtime, gates controller poses with explicit active flags,
keeps hand-interaction bindings available alongside active controllers with controller-first priority,
supports WiFi UDP and USB ADB reverse TCP streaming, matches per-frame render poses for headset
compositor reprojection, reuses short decode/network gaps through configurable client reprojection
with conservative optional pose warp, shows a local headset shell with status text, reset,
controller lasers, hand laser/pinch input, visible hand-joint markers, a simple 3D grid, and
optional `XR_FB_passthrough` while waiting for
video, requests the server-selected display refresh rate, enables preset-driven
dynamic `XR_FB_foveation` when the headset supports it, and supports the Quest shader path for
foveated-encoding decompression plus edge-aware upscaling. ABR full mode can reconfigure encoded
stream resolution over USB TCP through protocol v1.2 `StreamConfigUpdate/Ack` when the client
supports it, without resizing the OpenXR application's swapchains, and the passthrough feature can keep Quest passthrough
active during negotiated MR streaming while local shell GL resources are released.
Runtime video sends now run behind a
bounded encoded-frame sender queue so socket backpressure does not run inside VideoToolbox callbacks,
the Quest client drains MediaCodec output off the XR frame loop, and the runtime ABR controller
uses client latency, displayed frame age, keyframe requests, send/encoder drops, and reprojection
pressure to adjust bitrate with sliding windows and hysteresis. The visionOS
viewer now starts from a minimal floating search window, enters immersive VR automatically when the
stream connects, and sends head pose, hand joints, and first-pass tracked accessory controller data
while the immersive space is open. The macOS SwiftUI Home app now targets direct notarized
distribution so it can scan known apps, launch compatible apps with the user-selected
`XR_RUNTIME_JSON`, register that selected runtime, and capture app logs.
The Home app shows a main-window runtime activity summary from
`~/Library/Application Support/OXRSys/runtime_status.json`, including idle/streaming state,
transport, connected device family, active OpenXR application, WiFi/USB transport readiness,
first-launch runtime registration guidance, one-step USB reverse setup, SDK-free native USB ADB
setup in the SwiftUI Home app, native ADB-server protocol support with external `adb` fallback, and per-app custom ADB path selection for USB setup. Home streaming controls include the shared
runtime 1-200 Mbps bitrate bounds, server-selected refresh rate, encoder preset, foveated encoding
preset, ABR/dynamic-resolution mode, mixed reality, occlusion, spatial toggles, and a separate Headset Client section for client foveation override,
Quest shader upscaling, client reprojection, and reserved headset-audio configuration;
clients can send `ClientConnect.maxBitrateMbps = 0` to use the server-configured bitrate without
adding a client-side cap.
The Home app can enable a Developer tab from its Settings tab, open the macOS simulator in a
same-process window backed by the shared `OXRSysSimulator` Swift package, and show live runtime
streaming statistics from the existing telemetry path. The Qt Home Developer tab opens the shared
Qt simulator widget in a dedicated window with UDP video preview, mouse-driven synthetic head
tracking, simulator-owned vertical FOV sent through tracking eye-FOV metadata, explicit
FFmpeg-disabled fallback, frame-loss/FEC status, and keyframe recovery requests.
The macOS package helper builds the runtime dylib and Home app into one local folder with a complete
`runtime/` directory; the distribution helper signs that package, creates a combined archive, and can
submit that archive for notarization with Apple Developer account credentials.
The `net.demonixis.oxrsys-unity` Unity Package Manager package under `scripts/unity/` covers editor
runtime selection and the macOS Player OpenXR loader bundle-name workaround needed by exported Unity
apps.
As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is fully green locally:
63 passed, 36 skipped, 0 failed.

## Repository Rules

- **Always build and verify before declaring success** — run the macOS build + tests and/or Android build as appropriate before saying everything works
- **Always update `README.md`, `AGENTS.md`, and the relevant files in `docs/` when making significant project changes**
- Keep SwiftUI Home and Qt Home companion behavior in sync when changing shared Home workflows; only diverge for frontend-specific changes or when the user explicitly asks for a feature to be limited to one frontend.
- Core C++ dependencies are fetched via CMake FetchContent; Qt, FFmpeg, Vulkan SDKs, and platform SDKs are system/toolchain dependencies.
- Product versions are centralized in `config/OXRSysVersion.xcconfig`; do not hardcode
  marketing versions or build numbers in CMake, Xcode, Gradle, or native client code.
- Commit messages must read naturally and must not mention Codex or include `[codex]`.
- All source code and documentation must be in English
- Project-owned source code is licensed under MPL-2.0; preserve SPDX headers and keep third-party code under its upstream license.

## Quality Bar

- Keep patches focused and avoid silent behavior changes.
- Add or update tests when behavior changes. If a test cannot be added, explain why.
- Preserve non-blocking frame submission and latency-sensitive paths.
- Do not add new dependencies without a clear reason.

## Documentation Rules

`README.md` must stay short. Put detailed build, platform, protocol, simulator, and test guidance in `docs/`.

`CHANGES.md` owns release notes. Keep pending release dates as `TBD` until the release owner sets the final date.

Avoid duplicating the same guidance in multiple files. If commands, platform status, release notes, or CTS results change, update the single page that owns that topic and keep cross-links accurate.

## Important Technical Constraints

- The runtime does not link directly against Vulkan. Resolve Vulkan functions through the app-provided loader path; Vulkan v1 may fall back only to an already-loaded process `vkGetInstanceProcAddr` and must not call `LoadLibrary`/`dlopen` for the Vulkan loader.
- `Session::EndFrame()` must stay non-blocking.
- The streaming encoder queue is latest-frame-only; replacing a pending frame must release its `FrameSource` resources.
- Metal streaming must snapshot dynamic swapchain images through the app-provided command queue and GPU-side shared-event waits; if no staging slot is safe to reuse, drop that streaming frame instead of reading a live reused swapchain slot.
- Quest USB streaming uses reconnecting ADB reverse TCP on localhost ports `9944`, `9945`, `9946`, and the reserved reliable spatial port `9948`; app-level Android USB permission dialogs are only for `UsbManager`-visible devices and are not required for ADB reverse streaming.
- Home USB setup should prefer the native ADB host-server protocol on `127.0.0.1:5037` when available, fall back to a selected or auto-detected `adb` executable only when needed, and configure missing reverse mappings automatically when the user selects USB.
- Quest USB TCP sockets must keep bounded send behavior; failed video sends must clear stale TCP dispatch state and must not block the encoded-frame sender, VideoToolbox callback, or `Session::EndFrame()`.
- Encoded video dispatch is latest-frame-oriented and bounded; stale queued frames may be dropped instead of building latency when the transport cannot keep up.
- Runtime-managed Quest logcat capture is optional and disabled by default; if enabled, clearing the headset log before capture must remain bounded/best-effort and must not block runtime startup or tests.
- Headset refresh rate is selected by the server config/Home, requested by the Quest client through `XR_FB_display_refresh_rate`, and negotiated back from the active client rate.
- The Quest Android client still uses the build-time `OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ` value as a fallback before a server is discovered.
- Latency reports feed bounded pose prediction.
- Headset clients must match `VIDEO_FLAG_RENDER_POSE` metadata to the decoded frame before projection submission.
- Quest client reprojection must stay bounded: use exact render-pose matches first, only fall back to recent monotone render poses, disable image-space pose warp on old frames, strong translation, missing pose, recovery, or repeated stale reuse, and keep all work on the GLES/EGL GPU path.
- ABR must avoid oscillation: lower bitrate quickly on latency/loss/reprojection pressure, recover slowly through hysteresis, and do not raise bitrate/resolution while displayed frame age or reprojection pressure is high. Dynamic resolution only changes the encoded streaming size, never the OpenXR application's swapchain size, and live decoder reconfiguration is limited to reliable USB TCP until WiFi has a safe reliable control path.
- Server-side foveated encoding uses the ALVR-style AADT transform on the async Metal encode path only; write AADT output through a Metal compute pass into a private GPU scratch texture before blitting into VideoToolbox pixel buffers, keep it fail-closed when layout/source dimensions are not coherent, and do not send distorted video to clients without `CLIENT_CAPABILITY_FOVEATED_ENCODING`.
- Quest shader upscaling and foveated-encoding decompression must preserve render-pose matching and keep the plain bilinear path working when the server flags are off.
- Quest decoder input buffers must keep bounded headroom above `encodedWidth * encodedHeight`; foveated encoding can shrink encoded dimensions while high-bitrate IDR frames remain large.
- Headset speaker audio has protocol/config scaffolding only until a real capture/playback path is attached; do not advertise `SERVER_FEATURE_HEADSET_AUDIO` without that pipeline.
- UDP FEC uses the existing 24-byte `VideoPacketHeader` padding to carry the final data packet size for each FEC group; clients must use it only when recovering the last packet in that group.
- Quest hand tracking depends on the Android manifest permission `com.oculus.permission.HAND_TRACKING` and the optional `oculus.software.handtracking` feature.
- Quest passthrough shell mode depends on `XR_FB_passthrough` support and the optional Android feature `com.oculus.feature.PASSTHROUGH`; streaming video remains the priority display path. The Android client must advertise `CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH` only after the headset runtime reports support and passthrough objects are created, and runtime status must distinguish `passthrough_enabled`, `passthrough_supported`, and `passthrough_ready`. Streaming disables the local passthrough underlay and releases local shell GL resources unless effective passthrough is active; current alpha/depth transport is approximated by the Quest shader black-key policy documented in `docs/platforms/quest.md`.
- OpenXR environment blend mode support is snapshotted at instance creation; changing `passthrough_enabled` requires restarting the OpenXR app/runtime session to change advertised blend modes.
- Streaming controller poses are valid only when `TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE` or `TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE` is present; missing controller flags must not overwrite the last valid runtime pose.
- The action system is profile-aware and must not regress to hard-forcing `KHR simple_controller`.
- `xrLocateSpacesKHR` is accepted as an alias of the OpenXR 1.1 `xrLocateSpaces` entry point.
- Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.
- Runtime configuration is loaded from the platform config directory:
  macOS `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`,
  Linux `${XDG_CONFIG_HOME:-~/.config}/oxrsys/oxrsys-runtime.toml`,
  Windows `%APPDATA%/OXRSys/oxrsys-runtime.toml`.
- Qt Home transport readiness and USB ADB reverse configuration run asynchronously on a worker; keep slow process calls off the UI thread and ignore stale worker results after path, serial, or transport changes.

## Project Layout

```text
oxrsys_runtime/
├── CMakeLists.txt
├── CHANGES.md
├── cmake/RunOpenXRCTS.cmake
├── config/OXRSysVersion.xcconfig
├── runtime/
├── clients/
│   ├── Android/
│   │   └── android-vr/
│   ├── Apple/
│   │   ├── OXRSys Clients.xcworkspace/
│   │   ├── common/
│   │   │   ├── OXRSysStreaming/
│   │   │   └── OXRSysSimulator/
│   │   ├── oxrsys-home/
│   │   ├── oxrsys-simulator/
│   │   └── oxrsys-visionos/
│   └── Qt/
│       ├── apps/
│       └── libs/
├── common/
│   └── protocol/include/oxrsys/protocol/
├── scripts/
│   ├── macos_build_package.sh
│   ├── macos_sign_notarize.sh
│   └── unity/
│       ├── package.json
│       └── Editor/
├── tests/
│   ├── TestConfig.cpp
│   ├── TestInputManager.cpp
│   ├── TestRuntimePlatform.cpp
│   ├── TestStreamingFrameQueue.cpp
│   ├── TestVulkanDispatch.cpp
│   ├── HomeLauncherTests.swift
│   ├── TestProtocolLayout.cpp
│   ├── TestRuntimeStatus.cpp
│   └── TestRuntimeApi.cpp
└── docs/
```

## Verification Commands

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
swift test --package-path clients/Apple/common/OXRSysStreaming
swift build --package-path clients/Apple/common/OXRSysSimulator
swiftc -parse-as-library \
  "clients/Apple/oxrsys-home/OXRSys Home/HomeSupport.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/OXRSysServerConfig.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/HomeLauncher.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/HomePreferences.swift" \
  tests/HomeLauncherTests.swift \
  -o /tmp/oxrsys_home_launcher_tests && /tmp/oxrsys_home_launcher_tests
xcodebuild -project "clients/Apple/oxrsys-home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  build

xcodebuild -project "clients/Apple/oxrsys-simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  build

xcodebuild -project "clients/Apple/oxrsys-visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  build

cd clients/Android/android-vr && ./gradlew assembleDebug

cmake -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_BUILD_QT_FRONTENDS=ON
cmake --build build-qt
ctest --test-dir build-qt --output-on-failure
```

Optional CTS lane:

```bash
cmake -B build_cts -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_ENABLE_CTS=ON
cmake --build build_cts --target openxr_cts_run
```
