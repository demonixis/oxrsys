# Architecture

## Overview

OXRSys Runtime is a cross-platform OpenXR runtime in progress. macOS is the mature path, Linux has Vulkan + FFmpeg streaming and a first OpenGL GLX backend, and Windows has Vulkan plus Direct3D 11/12 runtime backends. Shared platform, config, status, codec, and socket helpers are kept portable so platform-specific backends can be added without spreading OS calls through the runtime. The runtime is discovered by the OpenXR loader through the generated `oxrsys-runtime.json` manifest.

## Repository Layout

- `runtime/`: runtime library, graphics integration, input, configuration, streaming server, tracking receiver, and video encoder.
- `common/protocol/include/oxrsys/protocol/`: canonical C++ protocol and FEC wire layout.
- `clients/Android/android-vr/`: Quest/Pico-oriented Android VR client for decode, display, and tracking return.
- `clients/Apple/`: Xcode workspace, native SwiftUI Home app, unified Apple simulator/viewer, visionOS viewer, and shared Swift packages.
- `clients/Qt/`: Qt Home app, Qt simulator app, and reusable Qt simulator widget.
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

`Session::EndFrame()` must remain non-blocking. The streaming path uses `StreamingFrameQueue`, a latest-frame-only queue that replaces any not-yet-encoded frame and immediately releases the replaced `FrameSource` resources. `FrameSource` owns backend-native image references and per-image sync tokens so async encoders can safely outlive the OpenXR frame submission.

## Runtime Boundaries

The runtime keeps these internal boundaries explicit:

- `RuntimePlatform`: config/state roots, module directory detection, and process ids.
- `RuntimeSockets`: UDP/TCP socket creation, options, timeout, close, and best-effort send/receive wrappers.
- `GraphicsContext`: typed Metal, Vulkan, OpenGL, D3D11, and D3D12 session context passed from OpenXR graphics bindings into sessions, swapchains, streaming, and encoders.
- `SwapchainCommon`: shared OpenXR swapchain state, acquire/wait/release ordering, and backend dispatch; graphics API files own only resource creation, image enumeration, destruction, and release-time snapshots.
- `FrameSource`: the pair of backend-native frame resources owned by the latest-frame queue until the encoder consumes or replaces them.
- `VulkanDispatch`: Vulkan function dispatch resolved from app-provided or already-loaded process entry points without linking the runtime to the Vulkan loader.

## Graphics Integration

### Metal

Metal is the native Apple rendering path. Applications provide an `MTLDevice` through `XR_KHR_metal_enable`, and swapchain textures are backed by native Metal resources.

For dynamic Metal swapchains, `xrReleaseSwapchainImage` snapshots the released slot into a staging texture using the app-provided `MTLCommandQueue`. The snapshot signals a `MTLSharedEvent`, and the VideoToolbox encode blit waits on that event GPU-side before reading the staging texture. This prevents the streaming encoder from reading a swapchain slot after the app has released and reused it, while keeping `Session::EndFrame()` CPU-non-blocking. Staging slots are leased through `FrameSource`; if no slot is safe to reuse, the runtime skips that streaming frame instead of falling back to an unsafe live-slot read.

### Vulkan

Vulkan support is exposed through `XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2`. The runtime does not link directly against Vulkan. Instead, it resolves Vulkan functions through the application-provided loader path to avoid dual-loader and dual-MoltenVK issues.

The v2 path stores the app's `pfnGetInstanceProcAddr`. The v1 path first reuses that dispatch if available, then looks for `vkGetInstanceProcAddr` only in already-loaded process modules: `dlsym(RTLD_DEFAULT, ...)` on POSIX and `GetModuleHandleW(L"vulkan-1.dll")` plus `GetProcAddress` on Windows. It intentionally does not load a Vulkan loader itself.

Vulkan swapchains allocate runtime-owned `VkImage` objects with transfer-source usage for color formats. When a color image is released, the runtime submits an image-to-buffer copy into a host-visible staging buffer and stores one `FrameImageSource` per array layer. The FFmpeg encoder waits on that source fence outside `Session::EndFrame()`, maps the staging buffer, converts RGBA/BGRA through `swscale`, and encodes H.264 or H.265. If readback resources or snapshot leases are unavailable, the streaming frame is dropped instead of blocking frame submission.

On macOS, Vulkan is intended for MoltenVK applications and validation builds with `-DOXRSYS_VIDEO_ENCODER=FFMPEG`; the native Metal + VideoToolbox path remains the default Apple streaming path. `VK_EXT_metal_objects` interop remains optional and is not required for the Vulkan FFmpeg readback path.

### OpenGL

OpenGL support is Linux-first through `XR_KHR_opengl_enable` and `XrGraphicsBindingOpenGLXlibKHR`. The runtime creates `XrSwapchainImageOpenGLKHR` textures for common color and depth internal formats, snapshots released color layers through an FBO into per-layer PBO readback slots, and resolves only fences that are already signaled. Ready PBO data is copied into `FrameImageSource` CPU snapshots for the same FFmpeg RGBA/BGRA to YUV path used by Vulkan.

macOS does not advertise OpenGL because `XR_KHR_opengl_enable` has no standard CGL binding. Windows OpenGL/WGL is intentionally left for a later backend milestone.

### Direct3D

Direct3D support is Windows-only through `XR_KHR_D3D11_enable` and `XR_KHR_D3D12_enable`. The runtime accepts app-owned D3D11 devices and D3D12 device/queue bindings, creates runtime-owned DXGI swapchain textures/resources, and exposes them through `XrSwapchainImageD3D11KHR` or `XrSwapchainImageD3D12KHR`.

D3D11 snapshots copy the released array layer into a staging texture. D3D12 snapshots enqueue a copy into a readback buffer and signal a fence. The FFmpeg encoder waits/maps outside `Session::EndFrame()`, converts DXGI RGBA/BGRA readback data through the same `swscale` path as Vulkan/OpenGL, and encodes H.264 or H.265.

## Input And Actions

The input system is profile-aware. The runtime currently supports:

- `KHR simple_controller`
- `oculus/touch_controller`
- Meta Quest Touch and Touch Plus controller profiles
- PICO Neo3 and PICO 4 controller profiles
- `ext/hand_interaction_ext`

The runtime also supports:

- `XR_EXT_hand_tracking`
- `XR_EXT_conformance_automation`
- `XR_EXT_debug_utils`
- `xrLocateSpacesKHR` as an alias for OpenXR 1.1 `xrLocateSpaces`

Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.

## Configuration

Runtime configuration is loaded from:

- macOS: `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`
- Linux: `${XDG_CONFIG_HOME:-~/.config}/oxrsys/oxrsys-runtime.toml`
- Windows: `%APPDATA%/OXRSys/oxrsys-runtime.toml`
- fallback: `build/runtime/oxrsys-runtime.toml`

Runtime status and logs are written to the platform state directory. On Linux this is `${XDG_STATE_HOME:-~/.local/state}/oxrsys`; on Windows it currently shares `%APPDATA%/OXRSys`.

For terminal-launched applications, use `XR_RUNTIME_JSON`. On macOS, `scripts/oxrsys_runtime_default.sh` can register `build/runtime/oxrsys-runtime.json` as the user default runtime and restore `XR_RUNTIME_JSON` through a LaunchAgent.

The native Home app in `clients/Apple/oxrsys-home/` manages the macOS workflow. The Qt Home app in `clients/Qt/oxrsys-home/` owns Linux runtime registration and can manually launch apps with the user-selected `XR_RUNTIME_JSON` on other desktop platforms.

The runtime reloads config changes opportunistically when the file timestamp changes. `runtime_enabled` is enforced on subsequent `xrCreateInstance` calls, dynamic streaming values such as keyframe cadence update without a full process restart, while initialization-time resources such as the file logger sink still require a restart. `streaming.video_codec` selects `h265`, `h264`, or `auto`; H.265 remains the default and the runtime only selects H.264 when the connected client advertises support. Headset and simulator clients own their eye FOV and send it through tracking metadata; the runtime config only keeps an internal fallback for clients that omit it.
