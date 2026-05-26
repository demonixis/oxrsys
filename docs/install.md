# Install

## Scope

This document lists the host tools and SDKs required to build and test the project on macOS, plus the Android tooling needed for the Quest client.

## Host Tools

Install the base macOS toolchain:

```bash
xcode-select --install
brew install cmake ninja gradle adb-enhanced openjdk@17
```

Install `adb-enhanced` via Homebrew to get `adb` on the command line in this setup.

For the Swift/Xcode applications and Swift package Metal shaders, install the full Xcode app, not only the Command Line Tools. Finish first-launch setup after installing or updating Xcode:

```bash
sudo xcodebuild -license accept
sudo xcodebuild -runFirstLaunch
xcodebuild -downloadComponent MetalToolchain
```

If simulator builds report that `CoreSimulator` is older than the selected SDK, update Xcode and the simulator runtime components so their versions match.

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

Then set `clients/oxrsys-android/local.properties`:

```text
sdk.dir=/Users/<you>/Library/Android/sdk
```

## Android Version Note

The current Gradle configuration in the repository uses `compileSdk = 35`, `targetSdk = 32`, and `minSdk = 29`. If `compileSdk` stays at `35`, you may also need:

```bash
sdkmanager --install "platforms;android-35"
```

Keep this document aligned with `clients/oxrsys-android/app/build.gradle.kts`.

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
