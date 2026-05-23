# Install

## Scope

This document lists the host tools and SDKs required to build and test the project on macOS, plus the Android tooling needed for the Quest client.

If you only want to run tagged binaries, start with [releases.md](releases.md) and install only the host pieces needed for that artifact. The rest of this page is for source builds.

## Prefer Repo-Local Tooling With Mise

The repository includes a local [`mise.toml`](../mise.toml) that pins the portable command-line tools used by the source workflow:

- Java 17
- CMake
- Ninja
- Android SDK command-line tools

If `mise` is not installed yet:

```bash
brew install mise
```

Trust the repo-local tool definition, then install from the repository root:

```bash
mise trust
mise install
```

The pinned Android SDK provides `sdkmanager`. `adb` comes from the `platform-tools` package installed into that same SDK.

## macOS Host Prerequisites

Install the base Apple toolchain first:

```bash
xcode-select --install
```

For the Swift/Xcode applications and Swift package Metal shaders, install the full Xcode app, not only the Command Line Tools. Finish first-launch setup after installing or updating Xcode:

```bash
sudo xcodebuild -license accept
sudo xcodebuild -runFirstLaunch
xcodebuild -downloadComponent MetalToolchain
```

These are still required even if you use `mise` for Java, CMake, Ninja, and the Android command-line tools.

If simulator builds report that `CoreSimulator` is older than the selected SDK, update Xcode and the simulator runtime components so their versions match.

If you want to build the visionOS client locally and the platform is not already installed, add it with:

```bash
xcodebuild -downloadPlatform visionOS
```

## Vulkan Loader And Headers

The macOS runtime CMake project uses `find_package(Vulkan REQUIRED)`, so Vulkan headers are an explicit host prerequisite even when your day-to-day work is Metal-first.

Choose one of these setups:

- install the full LunarG macOS Vulkan SDK if you need MoltenVK, Vulkan tools, or to run Vulkan applications through the runtime
- install a lighter header-only path only if it still satisfies your local `find_package(Vulkan)` configuration

Header-only example:

```bash
brew install vulkan-headers
```

For Vulkan interop work or Vulkan application testing, prefer the full LunarG SDK because it provides the loader, MoltenVK, and the surrounding tools in one consistent macOS install.

## Android SDK And NDK

With the repository `mise.toml`, install Android packages into the pinned SDK with `sdkmanager`.

Recommended packages:

- Android Platform-Tools for `adb`
- Android SDK Platform `34`
- Android Build-Tools `34.0.0`
- Android NDK `26.3.11579264`
- CMake `3.22.1`
- Android SDK Platform `35` while `compileSdk = 35`

Example:

```bash
mise exec -- sdkmanager --install \
  "platform-tools" \
  "platforms;android-34" \
  "platforms;android-35" \
  "build-tools;34.0.0" \
  "ndk;26.3.11579264" \
  "cmake;3.22.1"
```

Then set `clients/android-openxr/local.properties`:

```text
sdk.dir=/Users/<you>/.local/share/mise/installs/android-sdk/20.0
```

If you are not using `mise`, point `sdk.dir` at your own Android SDK root instead.

## Android Version Note

The current Gradle configuration in the repository uses `compileSdk = 35`, `targetSdk = 32`, and `minSdk = 29`. Keep this document aligned with `clients/android-openxr/app/build.gradle.kts`.

## OpenXR Samples And Clients

Optional but useful:

- OpenXR SDK examples such as `hello_xr`
- Unity for editor-side runtime selection testing
- Godot if you validate the Vulkan app path regularly

## Next Steps

- Tagged binaries first: [releases.md](releases.md)
- Build overview: [build.md](build.md)
- Quest client workflow: [quest.md](platforms/quest.md)
- Testing and CTS: [testing-and-conformance.md](testing-and-conformance.md)
