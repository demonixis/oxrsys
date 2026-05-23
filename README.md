# OpenXR OSX Runtime

[![License: MPL-2.0](https://img.shields.io/badge/License-MPL--2.0-blue.svg)](LICENSE)

## Project

OpenXR OSX Runtime brings OpenXR runtime support to Apple Silicon Macs. The repository includes a macOS runtime, a unified macOS/iOS viewer with `Simulator` and `StereoView` modes, a first-pass visionOS viewer, and a streaming stack with an Android client for Quest-class headsets.

Created by Yannick Comte.

If you want to try a tagged build instead of developing from source, start with [Releases](docs/releases.md). Use the source build workflow only when you need current branch changes or local debugging.

## Disclaimer
**Current Status**: This project is in early development and is not yet production-ready.

### Technical Limitations
* macOS Support: Due to non-standard OpenXR implementation on macOS, specific workarounds are required. The companion app can launch configured apps with `XR_RUNTIME_JSON`; command-line launches remain useful for debugging.
* Meta Quest Integration: The **interface is currently minimal**; the app displays a blue screen during standby and a green screen during loading.

### Stability & Contributions
Expect **frequent crashes and bugs**. This is a normal part of the development phase. We welcome contributions through:
•	Bug reports
•	Feature requests
•	Pull requests

### AI Disclosure
This project utilizes AI-generated code and documentation. We appreciate your professional cooperation regarding this approach.

## Source Build Prerequisites

- macOS 13 or later on Apple Silicon
- C++20
- Full Xcode app plus Command Line Tools
- Metal Toolchain component for Xcode
- Vulkan headers, and a Vulkan SDK or loader installation that satisfies `find_package(Vulkan)` on macOS
- Java 17, CMake, Ninja, and Android SDK command-line tools for the Quest client

The repository now ships a local [`mise.toml`](mise.toml) for portable tool pinning. OpenXR SDK headers and the loader are fetched automatically through CMake `FetchContent`; they are not a separate manual install step for source builds.

## Status

- Metal rendering, core runtime flow, Vulkan interop, and loader-backed runtime tests are in place.
- `XR_EXT_conformance_automation`, `XR_EXT_hand_tracking`, `XR_EXT_hand_interaction`, and `XR_EXT_debug_utils` are implemented.
- The Quest/Android client feeds real hand joints into the runtime, supports WiFi UDP and reconnecting USB ADB reverse TCP streaming, matches per-frame render poses for smoother headset reprojection, exposes a first-pass `XR_FB_foveation` path when supported by the headset, and can request a build-configured display refresh rate.
- The visionOS viewer uses a minimal floating search window, then enters immersive VR automatically once the stream connects and sends head pose, hand joints, and first-pass tracked accessory controller data back to the runtime when available.
- The macOS SwiftUI companion is now a direct-distribution launcher and runtime installer for compatible apps such as Godot and Unity, with a main-window runtime activity and transport readiness summary.
- As of March 17, 2026, the pinned non-interactive OpenXR-CTS baseline is green locally: 63 passed, 36 skipped, 0 failed.

## Documentation

- [Install](docs/install.md)
- [Releases](docs/releases.md)
- [Build](docs/build.md)
- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Simulator](docs/simulator.md)
- [Quest](docs/platforms/quest.md)
- [macOS Companion](docs/platforms/macos-companion.md)
- [iOS Viewer](docs/platforms/ios-viewer.md)
- [Vision OS](docs/platforms/visionos.md)
- [Testing And Conformance](docs/testing-and-conformance.md)
- [Licensing](docs/licensing.md)
- [Scripts](scripts/README.md)

## Contributing

Contributions from humans and LLM-assisted workflows are welcome. Keep changes small, tested, and documented: if behavior, architecture, build steps, or platform support changes, update the relevant files in `docs/` and `AGENTS.md` in the same patch.

Before considering a change ready, run `scripts/ci/verify-pr-lightweight.sh` for the always-on pull request checks. If you touch runtime, CMake, or protocol-sensitive behavior, also run `scripts/ci/verify-macos-runtime-heavy.sh`. If you touch runtime API or conformance-sensitive behavior, run the CTS lane when practical.

For automation, prefer checked-in entry points under `scripts/ci/` when that directory exists on the branch you are working on. Do not document workflow-only commands that cannot be run locally.

## License

The project is licensed under [MPL-2.0](LICENSE). Third-party SDKs, tools, platform runtimes, and OpenXR/Khronos components keep their own licenses and terms; see [Licensing](docs/licensing.md).
