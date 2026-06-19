# OXRSys Runtime

[![License: MPL-2.0](https://img.shields.io/badge/License-MPL--2.0-blue.svg)](LICENSE)

## Project

OXRSys Runtime is an unofficial OpenXR runtime that started on macOS and is being moved toward a measured cross-platform shape. The repository includes the shared runtime, Apple frontends, Qt frontends, and an Android VR streaming client for Quest/Pico-class headsets.

OXRSys is independent software. It is not affiliated with, endorsed by, sponsored by, or approved by The Khronos Group, Meta, Apple, LunarG, or the owners of the platforms, SDKs, runtimes, and trademarks referenced by this project.

### Android VR Client

The Android VR client can be used over WiFi or USB. The USB path is the best way to experiment with the runtime because it gives the lowest latency. Install `adb` first.

### Home Apps

OXRSys Home exists as a native Apple app and a Qt app. The Apple app owns the macOS direct-distribution workflow. The Qt app is Linux-first and also keeps its launcher, transport readiness, custom ADB selection, and simulator window code portable for macOS and Windows.
The macOS package helper builds the runtime and Home app into one local folder; the distribution helper signs that package and can submit the archive for notarization.

## Disclaimer

**Current Status**: This project is in early development and is not yet production-ready.

### Technical Limitations

- macOS Support: Due to non-standard OpenXR implementation on macOS, specific workarounds are required. OXRSys Home can launch configured apps with `XR_RUNTIME_JSON`; command-line launches remain useful for debugging. Unity projects should use the `net.demonixis.oxrsys-unity` Package Manager package under `scripts/unity/`.
- Meta Quest Integration: The headset client now shows a local status shell with reset and passthrough/3D controls while waiting for video.

### Stability & Contributions

Expect frequent crashes and bugs. Contributions are welcome through bug reports, feature requests, and pull requests.

### AI Disclosure

This project uses AI-generated code and documentation. We appreciate professional cooperation regarding this approach.

## Dependencies

- macOS 13 or later for Apple frontends and the Metal runtime path
- Linux with Vulkan, FFmpeg development libraries, pkg-config, and Qt 6 for the Linux runtime and Qt frontends
- C++20
- CMake with FetchContent
- Ninja
- OpenXR SDK headers and loader
- Metal
- Vulkan headers for interop paths
- Android SDK, Android NDK, and Java 17 for the Android client

## Status

- macOS: Metal rendering, release-time Metal streaming snapshots, core runtime flow, Vulkan interop, typed graphics/frame plumbing, and loader-backed runtime tests are in place.
- Linux: Vulkan runtime scaffolding and an FFmpeg encoder path are wired; real Vulkan image readback is still the main remaining Linux video gap.
- Windows: the runtime backend remains scaffold-only, while shared platform, config, status, and socket helpers are being kept portable.
- `XR_EXT_conformance_automation`, `XR_EXT_hand_tracking`, `XR_EXT_hand_interaction`, and `XR_EXT_debug_utils` are implemented.
- The Android VR client feeds real Quest/PICO hand joints into the runtime, gates controller poses and actions with explicit active flags, keeps hand-interaction bindings available alongside active controllers with controller-first priority, supports WiFi UDP and reconnecting USB ADB reverse TCP streaming, shows a local status shell with controller lasers plus hand laser/pinch controls before video arrives, matches per-frame render poses for smoother headset reprojection, reprojects short decode/network gaps with a configurable Quest client mode, applies server-selected refresh requests, supports optional server-announced `XR_FB_foveation` overrides for the headset viewer, supports the Quest shader path for foveated-encoding decompression and edge-aware upscaling, recovers when an initial video stream never arrives, reports frame age/reprojection telemetry for runtime status, and keeps decoder output draining off the XR frame loop.
- The visionOS viewer uses a minimal floating search window, then enters immersive VR automatically once the stream connects and sends head pose, hand joints, and first-pass tracked accessory controller data back to the runtime when available.
- OXRSys Home is now a direct-distribution launcher and runtime selector for compatible apps such as Godot and Unity, with a main-window runtime activity summary, autosaved streaming settings up to the shared 200 Mbps runtime cap, refresh/encoder/foveated-encoding/ABR controls, a separate Headset Client section for client foveation, reprojection, upscaling, and reserved audio, bounded Quest logcat capture setup, runtime log reveal actions, transport readiness controls, per-app custom ADB path selection, and optional Developer simulator workflows. Qt Home keeps the same shared streaming controls and keeps slow WiFi/ADB readiness work off the UI thread. The Apple and Qt simulators own simulator FOV locally and send eye-FOV tracking metadata; the Qt Home simulator opens in a dedicated window, uses decoded video as the interaction surface when FFmpeg is available, and keeps tracking-only fallback visible when it is not.
- As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is green locally: 63 passed, 36 skipped, 0 failed.

## Documentation

- [Install](docs/install.md)
- [Changes](CHANGES.md)
- [Build and versioning](docs/build.md)
- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Simulator](docs/simulator.md)
- [Quest](docs/platforms/quest.md)
- [macOS Home](docs/platforms/macos-home.md)
- [Qt Home](docs/platforms/qt-home.md)
- [iOS Viewer](docs/platforms/ios-viewer.md)
- [Vision OS](docs/platforms/visionos.md)
- [Testing And Conformance](docs/testing-and-conformance.md)
- [Licensing](docs/licensing.md)
- [Scripts](scripts/README.md)

## Contributing

Contributions from humans and LLM-assisted workflows are welcome. Keep changes small, tested, and documented: if behavior, architecture, build steps, or platform support changes, update the relevant files in `docs/` and `AGENTS.md` in the same patch.

Before considering a change ready, run the build and tests for the affected platform. If you touch the Android client, also run the Android build. If you touch runtime API or conformance-sensitive behavior, run the CTS lane when practical.

## License

The project is licensed under [MPL-2.0](LICENSE). Third-party SDKs, tools, platform runtimes, and OpenXR/Khronos components keep their own licenses and terms; see [Licensing](docs/licensing.md).
