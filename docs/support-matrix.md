# Support Matrix

This matrix separates declared product support from automated and physical qualification. A green
build proves compilation only; it does not prove application launch, live streaming, visual
correctness, USB behavior, or device tracking.

## Host Runtime

| Area | Supported | Automated gate | Release qualification |
| --- | --- | --- | --- |
| macOS Apple Silicon | macOS 14+, `arm64` | CMake, CTest, Home, simulator | Real Apple Silicon Mac launch and streaming |
| macOS Intel | macOS 14+, `x86_64` | Native Intel CMake, CTest, Home, simulator | Real Intel Mac launch and streaming |
| Universal distribution | `arm64` + `x86_64` | Release package and `lipo` validation | Sign, notarize, unpack, register, and launch |
| Metal | `XR_KHR_metal_enable` | Loader and frame-path tests | Real Metal OpenXR application stream |
| Vulkan/MoltenVK | Vulkan v1/v2 extensions | Build, loader/dispatch, generic `HostFence` wait contract | Real image export, fence completion, and MoltenVK application stream |
| Video encode | H.264, H.265, HEVC Main10 when negotiated | Codec and VideoToolbox tests | Visual decode and latency replay |

OXRSys does not provide a host runtime outside macOS and does not advertise host OpenGL or Direct3D
graphics bindings.

## Streaming Clients

| Client | Declared support | Automated gate | Physical gate |
| --- | --- | --- | --- |
| Meta Quest 1 | Android API 29, arm64 | Debug and stable Release APK | WiFi, USB, video, controllers/hands |
| Meta Quest 2 | Android arm64 | Debug and stable Release APK | WiFi, USB, passthrough/reprojection |
| Meta Quest 3 | Android arm64 | Debug and stable Release APK | Full Quest feature replay |
| Meta Quest Pro | Android arm64 | Debug and stable Release APK | WiFi, USB, tracking and decode |
| Pico | Android arm64 | Debug and stable Release APK | WiFi/USB, controller profile, decode |
| Vision Pro | visionOS 26+ | visionOS Simulator and generic-device compile | Broadcast/direct UDP discovery, immersion, tracking, reconnect, latency |
| macOS simulator | macOS 14+, Apple Silicon and Intel | Native builds plus Swift package tests | Live video and synthetic tracking |
| iOS Cardboard viewer | iOS 17+ with ARKit | Simulator and generic-device compile | Physical iPhone stereo and ARKit tracking |

The reusable `OXRSysStreaming` package supports visionOS 1+, but the Vision Pro app requires
visionOS 26+ for its compositor path.

Physical Vision Pro networking requires Local Network permission. Automatic `Find Server` broadcast
discovery additionally requires Apple's restricted `com.apple.developer.networking.multicast`
entitlement in the provisioning profile and app signature. Direct IPv4/hostname discovery is the
unicast fallback when that entitlement is unavailable; Simulator and unsigned device compilation do
not qualify either physical path.

## Transport And Features

| Capability | Clients | Qualification note |
| --- | --- | --- |
| WiFi UDP streaming | Quest, Pico, Apple clients | Test loss, keyframe recovery, reconnect, and bounded latency |
| USB ADB reverse TCP | Quest/Pico Android client | Test native Home ADB first, then server/external fallback |
| H.264/H.265 negotiation | Android and Apple clients | Verify configured preference and safe fallback |
| HEVC Main10 | Advertised capable Apple clients | Verify exact color range and fallback to 8-bit |
| Controller and hand input | Headset clients | Verify active flags and profile-aware actions |
| Foveated encoding | Capability-advertising clients | Verify inverse transform and undistorted fallback |
| Client reprojection | Quest and visionOS paths | Verify pose matching, stale-frame bounds, and recovery |
| Passthrough/MR | Supported Quest devices | Verify support/readiness state and explicit alpha opt-in |
| Dynamic encoded resolution | Reliable USB control | Must never resize application swapchains |

## Reporting Rules

Use these labels in release and pull-request reports:

- **Built**: compilation completed for the named target and architecture.
- **Tested**: the named automated suite passed.
- **Packaged**: the archive or directory was assembled and structurally checked.
- **Launched**: the named app/runtime process started on the named host.
- **Streamed**: a real OpenXR application produced live headset or simulator video.
- **Device-qualified**: the named physical device and feature scenario was replayed.

Never infer a later gate from an earlier one.
