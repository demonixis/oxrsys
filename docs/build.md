# Build

## Runtime

The host runtime builds only on macOS and supports deployment on macOS 14 or later. Install the
dependencies from [Install](install.md), then use the host-native Debug preset:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

Explicit architecture presets keep Apple Silicon and Intel support visible:

```bash
cmake --preset macos-arm64
cmake --build build/macos-arm64
ctest --test-dir build/macos-arm64 --output-on-failure

cmake --preset macos-x64
cmake --build build/macos-x64
ctest --test-dir build/macos-x64 --output-on-failure
```

For a universal local build:

```bash
cmake --preset macos-universal
cmake --build build/macos-universal
```

Runtime outputs are under the selected build directory:

```text
runtime/liboxrsys-runtime.dylib
runtime/oxrsys-runtime.json
runtime/oxrsys-runtime.toml
```

The runtime needs Vulkan headers at compile time but does not link a Vulkan loader. Metal and
VideoToolbox are macOS frameworks.

## Versioning

`config/OXRSysVersion.xcconfig` is the only product-version source. CMake, Xcode, and Gradle consume
`OXRSYS_VERSION` and `OXRSYS_BUILD`; do not copy version values into project files.

## Deployment Baselines

- Runtime, OXRSys Home, and macOS simulator: macOS 14 or later
- Simulator/Cardboard viewer: iOS 17 or later
- Vision Pro app: visionOS 26 or later
- Shared `OXRSysStreaming` package: visionOS 1 or later

The package minimum is intentionally lower for reuse; the Vision Pro application still requires
visionOS 26.

## Swift Packages

```bash
swift test --package-path clients/shared/OXRSysStreaming
swift build --package-path clients/shared/OXRSysSimulator
```

`OXRSysStreaming` supports macOS 14+, iOS 17+, and visionOS 1+. `OXRSysSimulator` is the shared
macOS 14+/iOS 17+ viewer implementation. Open `clients/OXRSys Clients.xcworkspace` for coordinated
Xcode work.

## OXRSys Home

```bash
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  -destination 'platform=macOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Run the Home XCTest target with:

```bash
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  -destination 'platform=macOS' \
  CODE_SIGNING_ALLOWED=NO \
  test
```

## Simulator And iOS Cardboard Viewer

macOS:

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

iOS Simulator:

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Generic physical iOS compile check:

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Compilation does not qualify Cardboard projection, touch interaction, or ARKit tracking; replay
those paths on a physical iPhone.

## visionOS Viewer

The application target requires visionOS 26 or later, independently of the shared streaming
package's lower reusable minimum.

```bash
xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Also compile the physical-device destination without signing:

```bash
xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Vision Pro tracking, immersive presentation, hardware decode, and latency require a physical-device
qualification run. Automatic broadcast discovery additionally requires the restricted multicast
entitlement in the signed provisioning profile; direct IPv4/hostname discovery remains available as
a unicast fallback with Local Network permission.

## Android VR Client

```bash
cd clients/android-vr
./gradlew assembleDebug assembleRelease
```

`assembleRelease` is the stable debug-signed, debuggable sideload APK. Use
`assembleOptimizedRelease` only to diagnose optimized-build regressions. APK build success does not
qualify WiFi, USB ADB reverse, headset decode, tracking, or reprojection.

## Local And Universal Packages

The package helper builds both the runtime and Home and assembles a relocatable directory:

```bash
./scripts/macos_build_package.sh
./scripts/macos_build_package.sh --configuration Release --architectures universal
```

`--architectures` accepts `native`, `arm64`, `x86_64`, or `universal`. Debug defaults to `native`;
Release defaults to `universal`. The helper checks every requested slice with `lipo`.

The output contains:

```text
OXRSys Home.app
runtime/liboxrsys-runtime.dylib
runtime/oxrsys-runtime.json
runtime/oxrsys-runtime.toml
```

The packaged manifest uses `./liboxrsys-runtime.dylib`, so the folder remains relocatable.

## Signing And Notarization

Build and sign a universal Release directly:

```bash
./scripts/macos_sign_notarize.sh \
  --build-runtime \
  --build-home \
  --architectures universal \
  --identity "Developer ID Application: Example Team (ABCDE12345)"
```

Add `--notarize`, `--apple-id`, `--password`, and `--team-id` for submission. Credentials must never
be committed. See [Scripts](../scripts/README.md) for all options.

## Runtime Selection

Prefer OXRSys Home for interactive launches. For a shell session:

```bash
export XR_RUNTIME_JSON="$PWD/build/runtime/oxrsys-runtime.json"
open /path/to/OpenXRApp.app
```

The Unity package under `scripts/unity/` handles editor runtime selection and the exported macOS
Player loader path.

## Troubleshooting

- Re-run CMake after switching architecture or Xcode SDK.
- Confirm `lipo -archs` reports the intended runtime and Home slices.
- Ensure Vulkan headers are installed; a separate loader is not a runtime link dependency.
- If Metal tools are missing, run `xcodebuild -downloadComponent MetalToolchain`.
- If Gradle cannot find Android tooling, verify `clients/android-vr/local.properties` and Java 17.
- Treat compilation, signing, launch, live streaming, and physical-device behavior as different
  qualification stages.
