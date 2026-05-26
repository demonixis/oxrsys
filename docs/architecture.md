# Architecture

## Overview

OXRSys Runtime is a macOS OpenXR runtime backed by Metal, with a simulator/debug path and a streaming path for external headsets. The runtime is exposed through `liboxrsys-runtime.dylib`, discovered by the OpenXR loader through the generated `oxrsys-runtime.json` manifest.

## Repository Layout

- `runtime/`: runtime library, graphics integration, input, configuration, streaming server, tracking receiver, and video encoder.
- `clients/common/`: shared protocol definitions used by the runtime and headset clients.
- `clients/oxrsys-android/`: Quest-oriented Android client for decode, display, and tracking return.
- `clients/oxrsys-home/`: native SwiftUI macOS Home app for config and runtime registration.
- `clients/oxrsys-visionos/`: native visionOS viewer that reuses the shared Swift streaming package.
- `tests/`: unit-style and loader-backed runtime tests.
- `cmake/`: CMake helpers, including the OpenXR-CTS lane.
- `docs/`: focused project documentation.

## Runtime Modes

The runtime operates in two modes:

- `Simulator`: local keyboard and mouse input, local debug rendering, no headset required.
- `Streaming`: a headset client connects, sends tracking, and receives encoded frames.

The simulator is useful for API validation and local debugging. Streaming is the path used for Quest-class headsets and future remote clients.

## Frame And Input Flow

At a high level:

1. The application drives the normal OpenXR frame loop.
2. The runtime maintains instance, session, space, action, swapchain, and hand-tracker state.
3. Input comes either from the simulator path or from the tracking receiver.
4. `xrEndFrame` validates and publishes the submitted composition data.
5. The local renderer presents to the debug window, and the streaming path can encode the latest frame for the client.

`Session::EndFrame()` must remain non-blocking. The streaming path uses a latest-frame-only queue to avoid building latency.

## Graphics Integration

### Metal

Metal is the native rendering path. Applications provide an `MTLDevice` through `XR_KHR_metal_enable`, and swapchain textures are backed by native Metal resources.

### Vulkan

Vulkan support is exposed through `XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2`. The runtime does not link directly against Vulkan. Instead, it resolves Vulkan functions through the application-provided loader path to avoid dual-MoltenVK crashes on macOS.

When Vulkan images need to be surfaced to the debug renderer, the runtime uses `VK_EXT_metal_objects` to bridge Vulkan-backed images to Metal textures.

## Input And Actions

The input system is profile-aware. The runtime currently supports:

- `KHR simple_controller`
- `oculus/touch_controller`
- `ext/hand_interaction_ext`

The runtime also supports:

- `XR_EXT_hand_tracking`
- `XR_EXT_conformance_automation`
- `XR_EXT_debug_utils`
- `xrLocateSpacesKHR` as an alias for OpenXR 1.1 `xrLocateSpaces`

Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.

## Configuration

Runtime configuration is loaded from:

- `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`
- fallback: `build/runtime/oxrsys-runtime.toml`

For terminal-launched applications, use `XR_RUNTIME_JSON`. For GUI applications, `scripts/oxrsys_runtime_default.sh` can register `build/runtime/oxrsys-runtime.json` as the user default runtime and restore `XR_RUNTIME_JSON` through a LaunchAgent.

The native Home app in `clients/oxrsys-home/` manages the same files directly, controls whether the runtime is enabled, and can register or unregister the runtime JSON through the user OpenXR config path.

The runtime reloads config changes opportunistically when the file timestamp changes. `runtime_enabled` is enforced on subsequent `xrCreateInstance` calls, dynamic streaming values such as FOV and keyframe cadence update without a full process restart, while initialization-time resources such as the file logger sink still require a restart.
