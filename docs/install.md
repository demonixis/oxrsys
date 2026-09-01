# Install

## Host Requirements

OXRSys builds on macOS only. Both Apple Silicon and Intel Macs are supported.

The supported deployment minimums are:

- macOS 14 or later for the runtime, OXRSys Home, and the macOS simulator
- iOS 17 or later for the simulator/Cardboard viewer
- visionOS 26 or later for the Vision Pro app

The shared `OXRSysStreaming` package retains a visionOS 1 minimum for reuse, but that does not lower
the Vision Pro app's visionOS 26 requirement. Building the newest client SDKs may require a newer
macOS development host than these deployment minimums.

Install:

- Xcode with the macOS, iOS, and visionOS SDKs needed by the clients you build
- the Xcode command-line tools
- CMake and Ninja
- Vulkan headers
- the Xcode Metal Toolchain for Metal shader compilation and CTS
- Git and a C++20 compiler

With Homebrew:

```bash
brew install cmake ninja vulkan-headers
xcodebuild -downloadComponent MetalToolchain
```

The runtime does not link a Vulkan loader. Install the LunarG macOS Vulkan SDK or another MoltenVK
distribution only when building or running Vulkan applications against OXRSys.

## Apple Client SDKs

The Swift packages require Swift 5.10 or later. Install the matching Xcode simulator runtimes for:

- macOS Home and simulator
- iOS Simulator and physical iPhone Cardboard/ARKit validation
- visionOS Simulator and physical Vision Pro validation

Physical-device builds require an Apple Development team and normal Xcode provisioning. CI builds
generic devices with `CODE_SIGNING_ALLOWED=NO`; this proves compilation, not installation.

## Android Client Tooling

Quest and Pico builds require:

- Java 17
- Android SDK platform 35
- Android build tools
- Android NDK 28.2.13676358 (the CI baseline)
- CMake `3.22.1+` from the Android SDK

Create `clients/android-vr/local.properties` when Gradle cannot discover the SDK:

```properties
sdk.dir=/absolute/path/to/Android/sdk
```

The Android client targets `arm64-v8a` and API 29 or later so Quest 1 remains supported. Keep these
requirements aligned with `clients/android-vr/app/build.gradle.kts`.

An Android SDK installation is not required for normal USB use of an already-installed headset
client. OXRSys Home can speak the native ADB host protocol, use an existing server on
`127.0.0.1:5037`, or fall back to a configured `adb` executable.

## Runtime Build

After installing dependencies:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

The generated loader manifest is `build/runtime/oxrsys-runtime.json`.

## Next Steps

- [Build](build.md) for every target and architecture
- [macOS Home](platforms/macos-home.md) for launch, registration, and USB setup
- [Quest and Pico](platforms/quest.md) for Android installation
- [Testing and Conformance](testing-and-conformance.md) before submitting a change
