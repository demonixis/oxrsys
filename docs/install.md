# Install

## Scope

This document lists the host tools and SDKs required to build and test the project on macOS, Linux, and Windows, plus the Android tooling needed for the Android VR client.

## Host Tools

Install the base macOS toolchain:

```bash
xcode-select --install
brew install cmake ninja gradle adb-enhanced openjdk@17
```

Install `adb-enhanced` via Homebrew to get `adb` on the command line in this setup. If `adb` is
installed elsewhere, both Home apps can store a custom ADB executable path from the Quest USB ADB
panel. The SwiftUI Home and Qt Home preferences are intentionally separate; clear the custom path to
return to automatic SDK/Homebrew/PATH detection.

Qt frontends need Qt 6 Core, Widgets, and Network. The build helper checks `QTDIR`, `Qt6_DIR`, and
Qt Online Installer layouts such as `~/Qt/<version>/<kit>` on macOS/Linux and
`C:\Qt\<version>\<kit>` on Windows. You can also set `CMAKE_PREFIX_PATH` to the kit root, such as
`C:\Qt\6.11.1\msvc2022_64`, or `Qt6_DIR` to its `lib\cmake\Qt6` directory.

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
- adb / Android Platform Tools for USB transport setup

On Fedora with RPM Fusion FFmpeg packages installed, use the matching RPM Fusion
development package:

```bash
sudo dnf install cmake ninja-build gcc-c++ pkgconf-pkg-config \
  vulkan-headers vulkan-loader-devel qt6-qtbase-devel android-tools \
  ffmpeg-devel
```

On Fedora systems that only use Fedora's free FFmpeg package set, use
`ffmpeg-free-devel` instead of `ffmpeg-devel`.

Windows runtime builds need:

- Visual Studio Build Tools or a Visual Studio installation with the Windows SDK
- CMake and Ninja
- Vulkan SDK headers for the Vulkan OpenXR path, unless the CMake FetchContent fallback is used
- Windows SDK Direct3D headers and libraries for the D3D11/D3D12 OpenXR paths
- FFmpeg development headers and import libraries for `avcodec`, `avutil`, and `swscale`, or vcpkg through the Windows configure helper
- Android Platform Tools if you use Quest USB ADB transport setup

Qt frontend builds additionally need Qt 6 Core, Widgets, and Network.

The easiest Windows path is the repository helper, which locates CMake, Ninja, Visual Studio, and
vcpkg. When `-FFmpegRoot` is omitted, vcpkg manifest mode builds the required FFmpeg libraries with
static library linkage and dynamic MSVC CRT linkage (`x64-windows-static-md` or
`arm64-windows-static-md`). The helper defaults Qt frontends to `AUTO`, so a runtime-only Windows
configure does not require Qt. Use the matching build helper so MSVC and Windows SDK library paths
are active during link:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows_configure.ps1 -Architecture x64
powershell -ExecutionPolicy Bypass -File scripts\windows_build.ps1 -Architecture x64
```

To use a local FFmpeg development package instead, set `FFMPEG_ROOT` or pass `-FFmpegRoot`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows_configure.ps1 `
  -Architecture x64 `
  -FFmpegRoot C:\dev\ffmpeg
```

When used, `FFMPEG_ROOT` must contain `include\libavcodec\avcodec.h` and import libraries under `lib\`.
At runtime, local dynamic FFmpeg packages still need their matching FFmpeg DLLs on `PATH` or copied
next to the executable/DLLs. To force dynamic vcpkg FFmpeg instead of the static default, pass
`-DynamicFFmpeg` to `scripts/windows_configure.ps1`.
Pass `-QtFrontends ON` after installing Qt 6 to build Qt Home and the Qt simulator. Use FFmpeg and
Qt packages that match the selected Windows target architecture
(`windows-x64` or `windows-arm64`). The ARM64 preset should be run from an ARM64 Visual Studio
developer environment, or from an IDE that activates the preset architecture.
Qt Home can also store a custom `adb.exe` path from the Quest USB ADB panel if Platform Tools are
installed outside `PATH`.

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

For Metal-only work, the macOS runtime builds without a full Vulkan SDK. For Vulkan interop work,
Linux/Windows Vulkan runtime builds, and Vulkan applications running through MoltenVK, install the
platform Vulkan SDK from LunarG or equivalent distro packages.

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
