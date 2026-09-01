# OXRSys Runtime

[![License: MPL-2.0](https://img.shields.io/badge/License-MPL--2.0-blue.svg)](LICENSE)

OXRSys is an open-source OpenXR runtime built specifically for macOS. It supports native Apple
Silicon and Intel hosts, Metal and Vulkan through MoltenVK, and low-latency H.264/H.265 streaming
through VideoToolbox.

The runtime streams to Meta Quest, Pico, Vision Pro, and the shared macOS/iOS simulator. The iOS
variant provides a Cardboard-style stereo view with ARKit tracking. OXRSys Home is a native SwiftUI
app for runtime registration, compatible-app launching, configuration, telemetry, and USB setup.

OXRSys is independent software. It is not affiliated with, endorsed by, sponsored by, or approved
by The Khronos Group, Meta, Apple, LunarG, or the owners of the platforms, SDKs, runtimes, and
trademarks referenced by this project.

## Highlights

- macOS runtime for `arm64` and `x86_64`, with universal release packaging
- `XR_KHR_metal_enable` and Vulkan/MoltenVK graphics paths
- VideoToolbox H.264, H.265, and negotiated HEVC Main10 streaming
- WiFi UDP and reconnecting USB ADB reverse TCP transport
- SDK-free native ADB setup in OXRSys Home, with local ADB server and external `adb` fallbacks
- Quest controller, hand tracking, reprojection, passthrough, foveation, and ABR paths
- native visionOS immersive viewer with automatic and direct-IP discovery, hand tracking, and
  accessory-controller tracking
- shared Swift streaming and simulator packages for macOS, iOS, and visionOS clients
- loader-backed runtime tests and a pinned OpenXR-CTS qualification lane

The current support and qualification levels are tracked in the
[support matrix](docs/support-matrix.md). Build success, packaging, application launch, live
streaming, and physical-device qualification are reported as separate gates.

## Repository Layout

```text
runtime/                 macOS OpenXR runtime
clients/
  home/                  SwiftUI macOS Home app
  simulator/             macOS/iOS simulator and Cardboard viewer
  visionos/              visionOS immersive viewer
  android-vr/            Quest/Pico OpenXR client
  shared/                shared Swift streaming and simulator packages
common/protocol/         shared C++ wire protocol
scripts/                 packaging, notarization, registration, and Unity helpers
tests/                   runtime and protocol tests
docs/                    detailed project documentation
```

Open `clients/OXRSys Clients.xcworkspace` when working on more than one Apple client.

## Quick Start

Requirements are macOS, Xcode, CMake, Ninja, C++20 tooling, Vulkan headers, and the Xcode Metal
Toolchain. Android client builds additionally require Java 17 and the Android SDK/NDK.

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

Build the native Home app:

```bash
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  -destination 'platform=macOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Build a universal local package:

```bash
./scripts/macos_build_package.sh \
  --configuration Release \
  --architectures universal
```

See [Install](docs/install.md), [Build](docs/build.md), and
[Testing and Conformance](docs/testing-and-conformance.md) before submitting a change.

## Documentation

- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Simulator](docs/simulator.md)
- [macOS Home](docs/platforms/macos-home.md)
- [Quest and Pico](docs/platforms/quest.md)
- [iOS Viewer](docs/platforms/ios-viewer.md)
- [visionOS](docs/platforms/visionos.md)
- [Scripts](scripts/README.md)
- [Changes](CHANGES.md)
- [Licensing](docs/licensing.md)

## Contributing

Contributions and LLM-assisted workflows are welcome. Keep changes focused, preserve the
latency-sensitive and non-blocking contracts in `AGENTS.md`, update tests and the owning
documentation, and run every affected build lane before declaring success.

## License

Project-owned source is licensed under [MPL-2.0](LICENSE). Third-party components retain their
upstream licenses and terms; see [Licensing](docs/licensing.md).
