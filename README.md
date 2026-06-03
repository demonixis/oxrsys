# OXRSys Runtime

[![License: MPL-2.0](https://img.shields.io/badge/License-MPL--2.0-blue.svg)](LICENSE)

## Project

OXRSys Runtime is an unofficial OpenXR runtime for Apple Silicon Macs. The repository includes a macOS runtime, a unified macOS/iOS viewer with `Simulator` and `StereoView` modes, a first-pass visionOS viewer, and a streaming stack with an Android client for Quest-class headsets.

OXRSys is independent software. It is not affiliated with, endorsed by, sponsored by, or approved by The Khronos Group, Meta, Apple, LunarG, or the owners of the platforms, SDKs, runtimes, and trademarks referenced by this project.

### Meta Quest Client

The Meta Quest client can be used over WiFi or USB. The USB path is the best way to experiment with the runtime because it gives the lowest latency. Install `adb` first; in this setup, `brew install adb-enhanced` provides it on your `PATH`.

### Home App

OXRSys Home is a macOS application for launching configured apps with the runtime, installing and registering the runtime, configuring USB for Quest, tuning streaming settings, and listing available apps.

## Disclaimer

**Current Status**: This project is in early development and is not yet production-ready.

### Technical Limitations

- macOS Support: Due to non-standard OpenXR implementation on macOS, specific workarounds are required. OXRSys Home can launch configured apps with `XR_RUNTIME_JSON`; command-line launches remain useful for debugging.
- Meta Quest Integration: The interface is currently minimal; the app displays a blue screen during standby and a green screen during loading.

### Stability & Contributions

Expect frequent crashes and bugs. Contributions are welcome through bug reports, feature requests, and pull requests.

### AI Disclosure

This project uses AI-generated code and documentation. We appreciate professional cooperation regarding this approach.

## Dependencies

- macOS 13 or later on Apple Silicon
- C++20
- CMake with FetchContent
- Ninja
- OpenXR SDK headers and loader
- Metal
- Vulkan headers for interop paths
- Android SDK, Android NDK, and Java 17 for the Android client

## Status

- Metal rendering, core runtime flow, Vulkan interop, and loader-backed runtime tests are in place.
- `XR_EXT_conformance_automation`, `XR_EXT_hand_tracking`, `XR_EXT_hand_interaction`, and `XR_EXT_debug_utils` are implemented.
- The Quest/PICO Android client feeds real hand joints into the runtime, gates controller poses with explicit active flags, supports WiFi UDP and reconnecting USB ADB reverse TCP streaming, matches per-frame render poses for smoother headset reprojection, exposes a first-pass `XR_FB_foveation` path when supported by the headset, and can request a build-configured display refresh rate.
- The visionOS viewer uses a minimal floating search window, then enters immersive VR automatically once the stream connects and sends head pose, hand joints, and first-pass tracked accessory controller data back to the runtime when available.
- OXRSys Home is now a direct-distribution launcher and runtime installer for compatible apps such as Godot and Unity, with a main-window runtime activity summary, transport readiness controls, and an optional Developer tab for opening the integrated simulator and viewing live runtime streaming stats.
- As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is green locally: 63 passed, 36 skipped, 0 failed.

## Documentation

- [Install](docs/install.md)
- [Build and versioning](docs/build.md)
- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Simulator](docs/simulator.md)
- [Quest](docs/platforms/quest.md)
- [macOS Home](docs/platforms/macos-home.md)
- [iOS Viewer](docs/platforms/ios-viewer.md)
- [Vision OS](docs/platforms/visionos.md)
- [Testing And Conformance](docs/testing-and-conformance.md)
- [Licensing](docs/licensing.md)
- [Scripts](scripts/README.md)

## Contributing

Contributions from humans and LLM-assisted workflows are welcome. Keep changes small, tested, and documented: if behavior, architecture, build steps, or platform support changes, update the relevant files in `docs/` and `AGENTS.md` in the same patch.

Before considering a change ready, run the macOS build and tests. If you touch the Android client, also run the Android build. If you touch runtime API or conformance-sensitive behavior, run the CTS lane when practical.

## License

The project is licensed under [MPL-2.0](LICENSE). Third-party SDKs, tools, platform runtimes, and OpenXR/Khronos components keep their own licenses and terms; see [Licensing](docs/licensing.md).
