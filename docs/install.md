# Install

## Scope

This document lists the host tools and SDKs required to build and test the project on macOS and Linux, plus the Android tooling needed for the Android VR client.

## Host Tools

Install the base macOS toolchain:

```bash
xcode-select --install
brew install cmake ninja gradle openjdk@17
```

The macOS SwiftUI Home app does not require Android Studio, the Android SDK, or a Homebrew `adb`
install for normal Quest USB setup: it can claim the headset USB ADB interface directly, complete
ADB authentication, and configure reverse mappings itself. External `adb` remains optional for
diagnostics, logcat, manual server startup, and fallback workflows. Install `adb-enhanced` or
Android Platform Tools only when you need those command-line tools. If `adb` is installed outside
the automatic search paths, both Home apps can store a custom ADB executable path from the Quest USB
ADB panel. The SwiftUI Home and Qt Home preferences are intentionally separate; clear the custom
path to return to automatic native/server/Homebrew/PATH detection.

Qt frontends need Qt 6 Core, Widgets, and Network. On macOS, the build helper checks Homebrew,
MacPorts, `QTDIR`, `Qt6_DIR`, and Qt Online Installer layouts under `~/Qt/<version>/<kit>`, such as
`~/Qt/6.10.2/macos`.

For the Swift/Xcode applications and Swift package Metal shaders, install the full Xcode app, not only the Command Line Tools. Finish first-launch setup after installing or updating Xcode:

```bash
sudo xcodebuild -license accept
sudo xcodebuild -runFirstLaunch
xcodebuild -downloadComponent MetalToolchain
```

If simulator builds report that `CoreSimulator` is older than the selected SDK, update Xcode and the simulator runtime components so their versions match.

Linux runtime and Qt frontend builds need equivalent distro packages for:

- CMake, Ninja, and a C++20 compiler
- Vulkan headers
- FFmpeg development libraries: `libavcodec`, `libavutil`, `libswscale`
- pkg-config
- Qt 6 Core, Widgets, and Network
- adb / Android Platform Tools for starting an ADB server, logcat, and USB fallback setup

On Fedora with RPM Fusion FFmpeg packages installed, use the matching RPM Fusion
development package:

```bash
sudo dnf install cmake ninja-build gcc-c++ pkgconf-pkg-config \
  vulkan-headers vulkan-loader-devel qt6-qtbase-devel android-tools \
  ffmpeg-devel
```

On Fedora systems that only use Fedora's free FFmpeg package set, use
`ffmpeg-free-devel` instead of `ffmpeg-devel`.

## Android SDK And NDK

Install Android command-line tools, then install the required packages with `sdkmanager`.

Recommended packages:

- Android SDK Platform `34`
- Android Build-Tools `34.0.0`
- Android NDK `26.3.11579264`
- CMake `3.22.1`
- Platform-Tools

Example:

```bash
sdkmanager --install \
  "platform-tools" \
  "platforms;android-34" \
  "build-tools;34.0.0" \
  "ndk;26.3.11579264" \
  "cmake;3.22.1"
```

Then set `clients/Android/android-vr/local.properties`:

```text
sdk.dir=/Users/<you>/Library/Android/sdk
```

## Android Version Note

The current Gradle configuration in the repository uses `compileSdk = 35`, `targetSdk = 32`, and `minSdk = 29`. If `compileSdk` stays at `35`, you may also need:

```bash
sdkmanager --install "platforms;android-35"
```

Keep this document aligned with `clients/Android/android-vr/app/build.gradle.kts`.

## Vulkan SDK And MoltenVK

For Metal-only work, the macOS runtime builds without a full Vulkan SDK. For Vulkan interop work and Vulkan applications running through MoltenVK, install the macOS Vulkan SDK from LunarG.

What you need from it:

- Vulkan headers
- MoltenVK
- Vulkan tools useful for validation and debugging

If you only need headers for local compilation, a lighter option is:

```bash
brew install vulkan-headers
```

## OpenXR Samples And Clients

Optional but useful:

- OpenXR SDK examples such as `hello_xr`
- Unity for editor-side runtime selection testing
- Godot if you validate the Vulkan app path regularly

## Next Steps

- Build overview: [build.md](build.md)
- Quest client workflow: [quest.md](platforms/quest.md)
- Testing and CTS: [testing-and-conformance.md](testing-and-conformance.md)
