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
Metal swapchain validation accepts sampled, render-target, depth/stencil, and transfer-destination
usage. Transfer-destination support covers applications such as Blender that render elsewhere and
blit the completed view into the runtime-owned private texture.
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

### Encode Path

The runtime decides once per encoder session where the VideoToolbox encode runs. VideoToolbox does
not give an `x86_64` process under Rosetta, such as a runtime loaded by CrossOver's Wine host, the
hardware H.265 encoder; that process only gets the software encoder, which misses a 90 Hz frame
budget. Rosetta does get hardware H.264. A native `arm64` process gets both.

With `streaming.encoder_helper = "auto"`, the default, the runtime asks VideoToolbox whether its own
process can obtain a hardware encoder for the negotiated codec. If it can, the encode stays
in-process. If it cannot, the runtime spawns `oxrsys-encoder-helper`, a native `arm64` process found
next to the runtime dylib or at `streaming.encoder_helper_path`, and sends it the compose
IOSurfaces once as Mach send rights. Each frame then crosses the process boundary as a slot index
on a Unix socket, and Annex-B NAL units come back the same way. The helper encodes the negotiated
H.265 Main, H.265 Main10, or H.264 Main profile. There is no AV1 encoder in VideoToolbox, so AV1
never uses the helper. `"true"` and `"false"` force the helper on or off for debugging.

The in-process session stays open behind the helper. If the helper cannot start, does not get a
hardware encoder, or dies mid-stream, the runtime logs where it failed, reclaims frames that were in
flight, and keeps streaming from the in-process session. Under Rosetta that session is the software
encoder. Because the software HEVC encoder would signal full range for the BGRA compose surface,
software sessions encode a BT.709 video-range 4:2:0 conversion, so a fallback does not change the
stream's color contract. The helper's design and IPC are described in
[`runtime/encoder_helper/README.md`](../runtime/encoder_helper/README.md).

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
