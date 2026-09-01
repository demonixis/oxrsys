# Testing And Conformance

## Automated Runtime Tests

Run the native macOS build first:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

Before merging host-runtime changes, repeat the explicit `macos-arm64` and `macos-x64` lanes. Tests
cover configuration, platform helpers, input/actions, protocol layout, status, frame-queue resource
release, codec selection, Vulkan dispatch, and loader-backed OpenXR behavior.

Graphics-extension tests must verify that Metal and Vulkan are exposed and that unsupported host
graphics bindings are not advertised.

Vulkan automation currently covers compilation, loader/dispatch behavior, and the generic bounded
`HostFence` wait contract. It does not execute MoltenVK image export, real fence completion, or a
stream from a Vulkan OpenXR application; those remain manual qualification gates.

## Apple Client Tests

```bash
swift test --package-path clients/shared/OXRSysStreaming
swift build --package-path clients/shared/OXRSysSimulator
xcodebuild -project "clients/home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" -configuration Debug \
  -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO test
```

The Home tests cover launcher and support behavior through the Xcode test target. Protocol changes
must update and pass both C++ and Swift layout tests.

CI also builds Home and the standalone simulator on native arm64 and Intel runners, builds the
simulator for generic iOS Simulator and iOS device destinations, and builds the visionOS viewer for
visionOS Simulator and the generic visionOS device destination with code signing disabled.

## Android Client Tests

```bash
(cd clients/android-vr && ./gradlew assembleDebug assembleRelease)
```

Keep C++ policy tests for Quest/Pico shell interaction and passthrough in the top-level CTest suite.
Use `assembleOptimizedRelease` as an additional diagnostic, not as the stable sideload gate.

## Packaging Tests

```bash
./scripts/macos_build_package.sh \
  --configuration Release \
  --architectures universal
```

The helper validates both `arm64` and `x86_64` slices in the runtime and Home executable. Also check
that the package manifest contains the relative `./liboxrsys-runtime.dylib` path. A package build is
not evidence that signing, notarization, launch, or streaming succeeds.

## CTS Lane

Install the Xcode Metal Toolchain, then run the pinned non-interactive CTS lane:

```bash
xcodebuild -downloadComponent MetalToolchain
cmake -B build_cts -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOXRSYS_ENABLE_CTS=ON
cmake --build build_cts --target openxr_cts_run
```

The CTS sub-build receives the resolved `metal` and `metallib` paths. As of March 17, 2026, the
pinned local baseline is 63 passed, 36 skipped, and 0 failed. Record a new date and exact counts
whenever the pin or result changes.

## Physical Qualification

Automated checks do not replace these release gates:

- launch and register OXRSys on a real Apple Silicon Mac and real Intel Mac
- stream an actual Metal application and an actual Vulkan/MoltenVK application
- test WiFi and native ADB reverse USB on affected Quest 1/2/3/Pro and Pico devices
- validate controller, hand, codec, refresh-rate, passthrough, ABR, and reprojection changes
- validate immersive presentation, tracking, reconnect, and latency on a physical Vision Pro
- validate Cardboard stereo presentation and ARKit tracking on a physical iPhone
- sign, notarize, staple, unpack, register, and launch the distribution archive

Report each gate independently. Do not collapse a successful build into a claim of runtime, visual,
or device success.

## Merge Expectations

- Keep mechanical moves separate from semantic changes.
- Run `git diff --check` and the lanes affected by the patch.
- Add tests for behavior changes or state the exact manual gate when automation is impossible.
- Update `README.md`, `AGENTS.md`, `CHANGES.md`, and the single owning documentation page for
  significant changes.
