# OXRSys Runtime

OXRSys is an open-source OpenXR runtime for macOS. The host runtime supports both Apple Silicon
(`arm64`) and Intel (`x86_64`), exposes Metal and Vulkan/MoltenVK graphics paths, and streams through
Apple VideoToolbox. Headset clients cover Quest 1/2/3/Pro, Pico, and Vision Pro. The shared
simulator runs on macOS and iOS; its iOS mode supports Cardboard-style stereo display and ARKit
tracking.

The desktop user interface is the native SwiftUI OXRSys Home app. AppKit may be used only in small,
platform-scoped adapters for capabilities SwiftUI does not expose directly, such as `NSOpenPanel`,
`NSWorkspace`, cursor/input handling, and `MTKView` integration.

As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is green locally: 63 passed,
36 skipped, and 0 failed.

## Repository Rules

- Always build and verify before declaring success. Report source/build, package, launch, live
  streaming, visual, and physical-device results as separate gates.
- Significant changes must update `README.md`, `AGENTS.md`, `CHANGES.md`, and the page in `docs/`
  that owns the affected workflow.
- All project-owned source and documentation must be in English.
- Product versions come only from `config/OXRSysVersion.xcconfig`.
- Project-owned source uses MPL-2.0. Preserve SPDX headers and upstream third-party licenses.
- Do not add dependencies without a clear need. C++ dependencies use CMake FetchContent; platform
  SDKs and Vulkan headers are toolchain dependencies.
- Keep commits focused. Commit messages must read naturally and must not mention Codex or contain
  `[codex]`.

## Quality Bar

- Add or update tests whenever behavior changes. If a behavior cannot be automated, name the
  manual qualification gate explicitly.
- Preserve non-blocking frame submission and bounded latency-sensitive queues.
- Keep pull requests easy to rebase: separate mechanical moves from semantic edits and avoid broad
  formatting changes.
- Preserve bundle identifiers, Xcode schemes, protocol layouts, runtime configuration keys, and
  user preferences unless the change explicitly migrates them.
- Keep `README.md` short. Detailed build, platform, protocol, simulator, and test guidance belongs
  in `docs/`; release notes belong in `CHANGES.md`.
- Pending release dates remain `TBD` until the release owner assigns a date. Do not rewrite released
  history when changing current support.

## Core Runtime Contracts

- The host runtime is macOS-only and must configure successfully for both `arm64` and `x86_64`.
  Release packages are universal unless explicitly requested otherwise.
- The runtime advertises only `XR_KHR_metal_enable`, `XR_KHR_vulkan_enable`, and
  `XR_KHR_vulkan_enable2` as graphics bindings. There is no host OpenGL or Direct3D path.
- The runtime must not link or load a Vulkan loader. Resolve Vulkan functions through the
  application-provided dispatch path. Vulkan v1 may fall back only to an already-loaded process
  `vkGetInstanceProcAddr` through `dlsym(RTLD_DEFAULT, ...)`.
- `Session::EndFrame()` must remain non-blocking.
- Encoded-frame dispatch is bounded and latest-frame-oriented. Replacing a pending frame must
  release its `FrameSource` resources; backpressure must never run in VideoToolbox callbacks or
  `Session::EndFrame()`.
- Metal streaming snapshots dynamic swapchain images on the application-provided command queue,
  uses GPU-side shared-event synchronization, and drops a streaming frame when no staging slot can
  be reused safely.
- Vulkan streaming snapshots released color images with application-dispatched Vulkan functions
  into bounded exportable images. Queue submission is non-blocking; fence waits belong to the
  encoder worker. Missing export support, exhausted slots, or wait failures drop the streaming frame
  instead of falling back to a live image or CPU readback.
- VideoToolbox is the only host encoder. Its stream is BT.709 SDR limited-range YCbCr; encoder
  metadata and client conversion must use the exact matching 8-bit and 10-bit code ranges.
- The VideoToolbox decoder path is latency-first: enable but do not require hardware decode, set
  real-time decode, never combine it with maximize-power-efficiency, and scan NAL units in place.
- Codec negotiation is conservative. `supportedCodecs = 0` means a legacy H.265-only client. H.265
  remains preferred; H.264 is selected only for clients that advertise it; encoder initialization
  retries the next compatible codec.
- Ten-bit streaming is HEVC Main10 only and requires both server configuration and
  `CLIENT_CAPABILITY_TEN_BIT_ENCODING`. H.264 and legacy clients remain 8-bit.
- `ServerAnnounce.clientSharpeningPercent` remains a 0-100 field in the existing reserved protocol
  slot. Keep its C++ and Swift layouts synchronized.
- The visionOS foveated-stream inverse warp must remain the exact inverse of the encoder
  `compress_axis` transform. Numerically revalidate fp32 round trips whenever either side changes.

## Streaming And Headset Contracts

- `DiscoveryRequest` remains the append-only one-byte message type `0x04`. A runtime in the
  broadcasting state may answer it on UDP `9946` with a real `ServerAnnounce` sent to the request
  source; the request must not fabricate announce fields, select a codec, or change connection
  state. Broadcast discovery and older clients remain unchanged.
- Quest USB uses reconnecting ADB reverse TCP on ports `9944`, `9945`, `9946`, and reserved reliable
  spatial port `9948`. ADB reverse does not require an Android `UsbManager` permission dialog.
- Home first tries its native ADB host protocol on `127.0.0.1:5037`, then the configured external
  `adb` fallback. USB readiness and reverse setup stay asynchronous, request-scoped, bounded, and
  reject stale results after the mode, path, device serial, or transport changes.
- Quest TCP sends are bounded. Failed video sends clear stale dispatch state without blocking the
  encoder, callback, or OpenXR frame path.
- The render-device preset fixes the OpenXR application swapchain size before a client connects.
  `resolution_scale` and ABR dynamic resolution change encode size only, never application
  swapchain dimensions.
- ABR lowers bitrate quickly under latency, loss, old-frame, drop, or reprojection pressure and
  recovers slowly with hysteresis. Live resolution changes remain limited to reliable USB control.
- Headset clients match `VIDEO_FLAG_RENDER_POSE` metadata to decoded frames before projection.
  Reprojection uses exact matches first, permits only recent monotone fallbacks, and disables image
  warp for stale frames, strong translation, missing poses, or recovery.
- Server foveated encoding is an asynchronous Metal compute transform into a private scratch
  texture. It fails closed if geometry is incoherent and is sent only to clients advertising
  `CLIENT_CAPABILITY_FOVEATED_ENCODING`.
- UDP FEC uses the existing 24-byte video-header padding to carry the final packet size; clients use
  it only when recovering the final data packet in a group.
- Streaming controller poses are valid only with the matching left/right controller-active flag.
  Missing flags must not overwrite the last valid runtime pose.
- The action system remains profile-aware and must not globally force `KHR simple_controller`.
- Quest hand tracking retains the Android permission and optional hand-tracking feature. Quest
  passthrough retains the optional passthrough feature and advertises capability only after runtime
  support and object creation are confirmed.
- Keep `passthrough_enabled`, `passthrough_supported`, and `passthrough_ready` distinct. Application
  alpha blending is a separate restart-bound opt-in; ordinary dark pixels are never transparent by
  default.
- Headset audio remains protocol/config scaffolding. Do not advertise the server audio feature
  until capture and playback are implemented.
- `xrLocateSpacesKHR` remains accepted as the alias of OpenXR 1.1 `xrLocateSpaces`.
- Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.

## Runtime Files And Registration

- Configuration: `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`
- Runtime status: `~/Library/Application Support/OXRSys/runtime_status.json`
- Generated manifest: `build/runtime/oxrsys-runtime.json`
- User loader registration: `~/.config/openxr/1/active_runtime.json`

The loader registration location is the convention used by the macOS OpenXR loader. It is not a
second supported host-platform configuration path.

## Project Layout

```text
oxrsys_runtime/
├── CMakeLists.txt
├── CMakePresets.json
├── CHANGES.md
├── config/OXRSysVersion.xcconfig
├── runtime/
├── clients/
│   ├── OXRSys Clients.xcworkspace/
│   ├── home/
│   ├── simulator/
│   ├── visionos/
│   ├── android-vr/
│   └── shared/
│       ├── OXRSysStreaming/
│       └── OXRSysSimulator/
├── common/protocol/include/oxrsys/protocol/
├── scripts/
├── tests/
└── docs/
```

Keep all user-facing screens in SwiftUI. Place direct AppKit/UIKit imports only in clearly named
platform adapters or rendering bridges.

## Verification Commands

Runtime, native architecture:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

Explicit host architectures:

```bash
cmake --preset macos-arm64
cmake --build build/macos-arm64
ctest --test-dir build/macos-arm64 --output-on-failure

cmake --preset macos-x64
cmake --build build/macos-x64
ctest --test-dir build/macos-x64 --output-on-failure
```

Swift packages and Home tests:

```bash
swift test --package-path clients/shared/OXRSysStreaming
swift build --package-path clients/shared/OXRSysSimulator
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" -configuration Debug \
  -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO test
```

Apple apps:

```bash
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" -configuration Debug \
  -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build

xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" -configuration Debug \
  -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build

xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" -configuration Debug \
  -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build

xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' CODE_SIGNING_ALLOWED=NO build

xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" -configuration Debug \
  -destination 'generic/platform=visionOS' CODE_SIGNING_ALLOWED=NO build
```

Android client and universal package:

```bash
(cd clients/android-vr && ./gradlew assembleDebug assembleRelease)
./scripts/macos_build_package.sh --configuration Release --architectures universal
```

The Android CI baseline uses Java 17, SDK platform 35, CMake 3.22.1, and NDK 28.2.13676358.

Optional CTS lane:

```bash
xcodebuild -downloadComponent MetalToolchain
cmake -B build_cts -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_ENABLE_CTS=ON
cmake --build build_cts --target openxr_cts_run
```

For release qualification, replay Metal and MoltenVK streaming on both a real Apple Silicon Mac and
a real Intel Mac, then validate the affected Quest/Pico, Vision Pro, and iPhone hardware paths.
