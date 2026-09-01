# Architecture

## Overview

OXRSys is a macOS OpenXR runtime with remote display clients. The runtime accepts OpenXR
applications through the generated loader manifest, renders through application-owned Metal or
Vulkan resources, snapshots released swapchain images, encodes them with VideoToolbox, and streams
them to a headset or simulator. Tracking, input, status, and control data return to the runtime.

The host runtime builds natively for Apple Silicon and Intel. Android, iOS, and visionOS are client
targets, not additional runtime hosts.

## Repository Layout

- `runtime/`: OpenXR entry points, instance/session/action state, graphics integration, streaming,
  VideoToolbox encoding, configuration, and status.
- `common/protocol/`: wire-layout definitions shared with native clients.
- `clients/home/`: SwiftUI macOS launcher, runtime selector, configuration, ADB, and diagnostics.
- `clients/simulator/`: standalone macOS/iOS simulator and Cardboard viewer.
- `clients/visionos/`: immersive Vision Pro client.
- `clients/android-vr/`: Quest/Pico OpenXR client.
- `clients/shared/`: Swift streaming and simulator packages shared by Apple clients.

Use `clients/OXRSys Clients.xcworkspace` for coordinated Apple-client development.

## Runtime And Frame Flow

1. The macOS OpenXR loader resolves `oxrsys-runtime.json` and loads the runtime dylib.
2. The application creates a Metal or Vulkan session and swapchains.
3. The runtime receives tracking from the active client and supplies predicted views/actions.
4. The application renders and releases its swapchain images.
5. The runtime snapshots the released color layers into a bounded backend-owned slot.
6. `xrEndFrame` enqueues the newest available snapshot without waiting for encode or transport.
7. VideoToolbox encodes H.264 or H.265, and the bounded sender dispatches the newest frame.
8. The client decodes and presents the frame matched to its render-pose metadata.

Encoder and socket backpressure never runs inside `Session::EndFrame()` or a VideoToolbox callback.
When a bounded slot or queue is full, OXRSys drops stale streaming work instead of accumulating
latency.

## Graphics Integration

### Metal

`XR_KHR_metal_enable` sessions retain the application-provided Metal device and command queue.
Released images are copied into bounded private staging textures on that queue. GPU-side shared
events synchronize the encoder worker. A slot that cannot be reused safely causes a streaming-frame
drop; the encoder never reads a live application swapchain image after release.

### Vulkan Through MoltenVK

The runtime supports `XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2`. It stores application-owned
instance, device, physical-device, queue, family, and dispatch state. Released color images are
copied into bounded exportable Vulkan snapshot images and exposed as Metal textures for the
VideoToolbox path. Fence waits occur on the encoder worker, never in `Session::EndFrame()`.

OXRSys deliberately does not link or load a Vulkan loader. Vulkan v2 uses the application's
`pfnGetInstanceProcAddr`; Vulkan v1 may reuse that dispatch or find an already-loaded
`vkGetInstanceProcAddr` with `dlsym(RTLD_DEFAULT, ...)`. Vulkan headers are a build dependency, while
MoltenVK and the Vulkan loader belong to the application/toolchain environment.

The macOS runtime does not advertise an OpenGL or Direct3D graphics binding.

## Video And Protocol

VideoToolbox is the host encoder. Codec selection combines configured preference, encoder
capability, and client advertisement. H.265 remains preferred; H.264 requires explicit client
support. HEVC Main10 additionally requires the server setting and the client ten-bit capability.

Apple streams use BT.709 SDR limited-range YCbCr. Foveated encoding runs as a Metal compute pass
before the VideoToolbox pixel-buffer copy and is enabled only for clients that advertise the exact
inverse transform.

The C++ and Swift protocol layouts must remain byte-compatible. See [Protocol](protocol.md) for
ports, messages, feature flags, and compatibility rules.

## Input And Actions

Tracking packets update head, controller, hand-joint, velocity, eye-FOV, and client status data.
Controller poses are accepted only when the corresponding active flag is set. The action system is
profile-aware and keeps hand interaction available alongside controller-first bindings.

Reference spaces currently include `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.
`xrLocateSpacesKHR` aliases the OpenXR 1.1 `xrLocateSpaces` entry point.

## Configuration And Status

The runtime reads:

```text
~/Library/Application Support/OXRSys/oxrsys-runtime.toml
```

It publishes current activity under the same Application Support directory in
`runtime_status.json`. OXRSys Home edits the configuration, launches applications with the selected
`XR_RUNTIME_JSON`, and manages the user loader registration at
`~/.config/openxr/1/active_runtime.json`.

Initialization-bound settings require restarting the OpenXR application. Dynamic streaming values
are reloaded only where the runtime explicitly supports safe live changes.

## Client Boundaries

- Quest/Pico clients own headset OpenXR, GLES composition, MediaCodec decode, WiFi/USB transport,
  controller/hand tracking, passthrough, and client reprojection.
- The visionOS client owns CompositorServices presentation, VideoToolbox decode, ARKit head/hands,
  tracked accessory controllers, and immersive lifecycle.
- The simulator package owns synthetic tracking on macOS and ARKit tracking plus Cardboard stereo
  presentation on iOS.
- Home owns desktop workflows and native ADB setup. All user-facing desktop screens are SwiftUI;
  AppKit is isolated to platform adapters.
