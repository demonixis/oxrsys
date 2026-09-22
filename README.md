# OXRSys

OXRSys is an open-source OpenXR runtime for macOS. It runs on Apple Silicon and Intel, draws with Metal or Vulkan through MoltenVK, and encodes with VideoToolbox.

It streams to Quest, Pico, Vision Pro, and a macOS/iOS simulator. OXRSys Home registers the runtime, launches titles, and sets up USB.

OXRSys is independent software. It is not affiliated with Khronos, Meta, Apple, or the other projects named here.

## Features

- Apple Silicon and Intel
- Metal, and Vulkan through MoltenVK
- H.264, H.265, and HEVC Main10
- Wi-Fi and USB to Quest and Pico
- Vision Pro viewer
- macOS and iOS simulator
- Controllers, hands, passthrough, foveation, and reprojection

## Install

You need macOS 14, Xcode, CMake, Ninja, and the Vulkan headers.

```bash
brew install cmake ninja vulkan-headers
xcodebuild -downloadComponent MetalToolchain

cmake --preset default
cmake --build build
```

## Documentation

Start with [Install](docs/install.md). Build lanes, the protocol, and each client are linked from there.

## Contributing

Help is welcome, including changes written with AI. Keep each change focused, follow `AGENTS.md`, and update the tests and the doc page that own the behavior.

## License

Project source is [MPL-2.0](LICENSE). Third-party code keeps its own license. See [Licensing](docs/licensing.md).
