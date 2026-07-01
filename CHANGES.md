# Changes

This file tracks user-facing, integration-facing, and runtime-relevant changes for OXRSys.

## 1.3.0 - TBD

### Added

- Added runtime video codec selection with `streaming.video_codec = "h265"`, `"h264"`, or `"auto"`, plus matching SwiftUI Home and Qt Home controls.
- Added conservative codec capability negotiation through `ClientConnect.supportedCodecs`, keeping legacy clients H.265-only while allowing H.264-capable clients to opt in.
- Added H.264 decode support to the Android VR client and shared Apple streaming path, with Android, Apple simulator, and visionOS clients advertising H.264/H.265 while keeping H.265 preferred.
- Added a codec-aware VideoToolbox decoder for Apple clients, including H.264 SPS/PPS and H.265 VPS/SPS/PPS parameter-set handling.
- Added an `OXRSYS_VIDEO_ENCODER` CMake option so macOS can use the default VideoToolbox path or an explicit FFmpeg encoder build for codec/pipeline validation.
- Added a first Linux OpenGL GLX backend through `XR_KHR_opengl_enable`, including OpenGL swapchain image enumeration and bounded FBO/PBO readback into the shared FFmpeg encode path.
- Added Windows Direct3D 11 and Direct3D 12 runtime backends through `XR_KHR_D3D11_enable` and `XR_KHR_D3D12_enable`, including DXGI swapchain images, bounded readback snapshots, FFmpeg conversion, and loader-backed WARP tests.
- Added loader-backed tests for host graphics extension exposure, including Vulkan everywhere, OpenGL only on Linux builds, and D3D11/D3D12 only on Windows builds.
- Added protocol v1.2 stream reconfiguration (`StreamConfigUpdate/Ack`) for reliable USB TCP, dynamic encoded-resolution profiles for `abr_mode = "full"`, global passthrough config with app-driven OpenXR alpha blend/source-alpha detection, headset passthrough support/readiness status, occlusion/spatial config gates, a reserved optional spatial TCP channel on `9948`, and matching SwiftUI/Qt Home controls and status display.
- Added a native USB ADB backend to SwiftUI Home so Quest USB reverse setup can run without Android Studio, the Android SDK, Homebrew, or an `adb` executable.
- Added world-space rotational reprojection to the visionOS viewer, reprojecting each streamed frame from its runtime render pose into the live head pose every vsync so the view stays locked to the world while turning.

### Changed

- Split non-Apple swapchain implementation by backend so Vulkan, Linux OpenGL, D3D11, and D3D12 resources live in separate files behind explicit platform/API guards.
- Promoted Linux Vulkan/FFmpeg runtime support from scaffolding to Vulkan swapchains, release-time staging readback, H.264/H.265 encode, and backend readback metadata shared by the existing FFmpeg encoder path.
- Updated Qt simulator video preview with H.264/H.265 decode selection.
- Updated the Quest/PICO shell to keep passthrough active only when global passthrough is enabled and the headset reports `XR_FB_passthrough` support, key app-requested alpha-blend/source-alpha video backgrounds for current AR demo scenes, and keep the black-key fallback when passthrough is active but no alpha flags have arrived yet.
- Updated SwiftUI Home and Qt Home setup flows with first-launch runtime registration guidance, automatic USB reverse configuration when USB is selected, packaged-runtime manifest preference, and native ADB host-server protocol support before falling back to an external `adb` executable.
- Updated documentation for video codec selection, Linux Vulkan/OpenGL streaming, protocol v1.2, passthrough/MR, native ADB setup, and visionOS reprojection.

### Fixed

- Fixed a Unity editor crash on session shutdown by invalidating stale VideoToolbox encode callbacks before the streaming server is destroyed and by catching callback exceptions inside the encoder.
- Fixed the visionOS viewer black screen and doubled AR view by sharing one ARKit world-tracking session between the tracking manager and the immersive renderer, and clearing the drawable depth buffer so the visionOS compositor has a surface to reproject.
- Fixed visionOS eye projection by sending the device's real per-eye FOV and IPD to the runtime, so it renders a matching frustum instead of the symmetric fallback that made the projection look wrong.
- Fixed Vision Pro head-rotation jitter at the source by tagging each streamed frame with the exact head pose captured at `xrLocateViews`, and by dropping out-of-order or duplicate UDP tracking packets before finite-difference prediction.

### Known Limits

- Vulkan, OpenGL, and D3D desktop streaming still need regular manual validation on real Linux/Windows hardware and with MoltenVK apps; Windows OpenGL/WGL remains separate follow-up work.

## 1.2.0 - 2026-06-19

### Added

- Added Linux-first Qt frontends under `clients/Qt/`, including Qt Home, a standalone Qt simulator, and a reusable simulator widget.
- Added Qt Home support for compatible app launching, selected-runtime registration on Linux, runtime TOML editing, runtime activity/status display, custom ADB selection, USB reverse mapping setup, and asynchronous transport readiness checks.
- Added Qt simulator video preview with FFmpeg when available, tracking-only fallback when FFmpeg is unavailable, mouse-driven synthetic head tracking, frame-loss/FEC status, and keyframe recovery requests.
- Added Linux Vulkan/FFmpeg runtime scaffolding, portable platform helpers, portable socket helpers, and platform-specific config/state directory support.
- Added first-pass Windows layout and portability scaffolding while keeping the Windows runtime backend non-gating for this release.
- Added canonical shared protocol headers under `common/protocol/include/oxrsys/protocol/`.
- Added centralized product versioning in `config/OXRSysVersion.xcconfig` for CMake, Xcode, and Android consumers.
- Added macOS package and distribution helpers: `scripts/macos_build_package.sh` and `scripts/macos_sign_notarize.sh`.
- Added the `net.demonixis.oxrsys-unity` Unity Package Manager package with editor runtime selection and a macOS Player OpenXR loader postprocessor.
- Added runtime tests for portable platform behavior, streaming frame queue replacement, Vulkan dispatch, expanded input handling, protocol layout, runtime status, and loader-backed API behavior.
- Added server-selected headset refresh controls, foveated encoding presets, headset client foveation override presets, Quest shader upscaling controls, and reserved headset-audio configuration to SwiftUI Home and Qt Home.
- Added protocol v1.1 trailing fields for server feature flags, client capability flags, foveated encoding parameters, client foveation, client upscaling, and reserved headset speaker audio.
- Added ALVR-style AADT foveated encoding math, a Metal encoder preprocessing shader, and Quest shader-side foveated-encoding decompression.
- Added a Quest edge-aware shader upscaling path without requiring the proprietary Snapdragon SDK.
- Added configurable Quest client reprojection modes (`off`, `pose`, `pose_warp`) for short decode/network gaps, with displayed-frame-age and reprojection counters in latency reports and runtime status.
- Added a local Quest/PICO shell that replaces standby/loading color clears with a 3D grid, upright status panel, reset button, optional `XR_FB_passthrough` mode, controller laser interaction, hand laser/pinch interaction, and visible hand-joint markers.
- Added a runtime ABR controller with `off`, `bitrate`, and `full` modes, sliding-window hysteresis, fast bitrate downshift, slow recovery, and profile reporting for future session-safe resolution/foveation/upscaling transitions.

### Changed

- Moved the repository toward the OXRSys cross-platform layout, including `clients/Android/android-vr/`, `clients/Apple/common/`, and `clients/Qt/`.
- Changed the runtime graphics plumbing to use typed `GraphicsContext` and `FrameSource` data across sessions, swapchains, streaming, and encoders.
- Kept Vulkan loader usage app-owned: the runtime resolves Vulkan entry points from the application-provided dispatch path or already-loaded process symbols without directly linking or loading the Vulkan loader.
- Reworked streaming frame submission around a latest-frame-only queue so replacing a pending frame releases its backend resources.
- Expanded runtime configuration reload behavior for dynamic streaming values while keeping initialization-time resources restart-bound.
- Raised the shared streaming bitrate range to `1` through `200` Mbps and allowed clients to send `ClientConnect.maxBitrateMbps = 0` to defer to the server-configured bitrate.
- Updated Apple and Qt simulator clients to avoid imposing their own bitrate cap.
- Updated Apple and Qt simulator clients to own simulator vertical FOV and send it through tracking eye-FOV metadata instead of exposing it through Home runtime config.
- Updated headset client foveation to default to `auto`, moved headset-side options into dedicated Home sections, and made Quest/PICO `XR_FB_foveation` apply only when Home sends an explicit override.
- Updated the streaming protocol to carry render-pose metadata per frame and to store the final FEC group packet payload size in the existing video header padding.
- Updated Quest/PICO controller profile handling to stay profile-aware instead of falling back globally to `KHR simple_controller`.
- Reworked Android VR client transport handling to prefer USB ADB reverse TCP when available, fall back to WiFi UDP discovery, request the build-configured display refresh rate before discovery, and advertise the headset OpenXR system name.
- Updated the Android VR client to request the server-announced refresh rate after discovery, report the active headset rate, advertise streaming capabilities, and apply server-selected client foveation/upscaling options.
- Updated runtime video dispatch so encoded frames pass through a bounded sender queue before WiFi/USB transport writes, keeping socket backpressure out of encoder callbacks.
- Updated the Quest USB ADB client to defer bitrate limits to the server/Home configuration instead of imposing an extra 100 Mbps cap.
- Updated the Quest decoder path to drain MediaCodec output on a decoder thread instead of the XR frame loop.
- Updated the Quest MediaCodec input sizing to keep bounded headroom for high-bitrate foveated-encoding IDR frames.
- Updated the Quest/PICO shell to pause passthrough, stop local shell interactions, and release shell GL resources while streaming video is actively rendered.
- Updated SwiftUI Home and Qt Home with ABR and Quest client reprojection controls plus runtime status display for frame age, ABR state, and reprojection reuse.
- Updated FFmpeg encoder preset mapping so Linux scaffolding maps `speed`, `balanced`, and `quality` to low-latency FFmpeg presets instead of always using `ultrafast`.
- Updated macOS Home for direct distribution workflows, selected-runtime app launching, runtime registration, package-compatible runtime paths, runtime activity display, and shared Developer simulator integration.
- Updated visionOS streaming behavior around the minimal search window, automatic immersive entry on stream connection, head/hand tracking, and first-pass tracked accessory controller data.

### Fixed

- Fixed Metal streaming frame snapshots so the async encoder reads a release-time staging texture instead of a swapchain slot that the app may already have reused.
- Fixed server-side foveated encoding on Metal by running the AADT pass through a compute shader into a private GPU scratch texture before blitting into the VideoToolbox pixel buffer, avoiding render-encoder validation aborts on the first encoded frame.
- Fixed Quest connection recovery when a server is discovered but no first video frame arrives, returning the client to discovery/retry instead of leaving the standby/loading screen stuck.
- Fixed controller pose handling so streaming packets only update controller poses when the corresponding controller-active flag is present.
- Fixed hand tracking and hand-interaction coexistence so hand bindings remain available while controller bindings keep priority for shared actions.
- Fixed Quest hand tracking ingestion by feeding real `XR_EXT_hand_tracking` joints from the Android client into the runtime.
- Fixed USB ADB reverse TCP reconnect behavior so closed control/video sockets or video stalls return the Android client to discovery/retry without relaunching the client.
- Hardened Quest USB TCP sends with bounded socket behavior and stale video dispatch cleanup so failed sends do not block encoder callbacks or `Session::EndFrame()`.
- Hardened Quest receive hot paths by reusing TCP/UDP reassembly buffers, avoiding per-packet receive timeout updates, and making USB tracking sends best-effort/non-blocking.
- Hardened Quest headset foveation shutdown by detaching the foveation profile from swapchains before destroying it.
- Hardened runtime-managed Quest logcat capture so it remains optional, bounded, and best-effort during startup.
- Fixed render-pose matching on headset clients so decoded frames are submitted with the pose used to render that frame.
- Filtered known macOS `linkd.autoShortcut` App Intents diagnostics from Home captured app logs.
- Fixed and covered `xrLocateSpacesKHR` as an alias for the OpenXR 1.1 `xrLocateSpaces` entry point.

### Documentation

- Reworked platform documentation for build, install, architecture, protocol, Quest/PICO, macOS Home, Qt Home, simulator, visionOS, and testing/conformance workflows.
- Documented current Linux, Windows-scaffold, macOS package, Unity, USB ADB, protocol, and CTS expectations.

### Known Limits

- Linux video streaming still needs real Vulkan image readback before it can be treated as feature-complete.
- Windows remains layout and portability scaffolding only for this release.
- PICO and headset-specific controller/hand tracking behavior still needs regular hardware validation.
- Headset speaker audio has protocol/config scaffolding but no active runtime capture/playback pipeline yet.

## 1.1.0 - 2026-05-26

### Added

- Added first-pass Quest USB streaming through ADB reverse TCP.
- Added macOS Home workflows for compatible app discovery, app launching with `XR_RUNTIME_JSON`, runtime settings, USB readiness guidance, and active runtime/app status.
- Added Developer Mode in the macOS Home app with integrated simulator access and live streaming statistics.
- Added a shared Apple simulator package used by the standalone simulator and the integrated Home simulator.
- Added a build-configured Android display refresh-rate request path.

### Changed

- Renamed and documented the project as OXRSys.
- Clarified USB streaming setup, runtime launch workflows, and companion/Home app behavior in the README and docs.
- Updated Xcode project metadata and the release version for `1.1.0`.

## v1.0.0 - 2026-05-14

### Added

- Initial OpenXR runtime implementation for macOS with Metal swapchains, runtime manifest generation, configuration loading, streaming server plumbing, input/action handling, hand tracking scaffolding, and loader-backed runtime tests.
- Initial Android OpenXR streaming client with network receive, H.265 decode, tracking return, and Quest-oriented native activity setup.
- Initial Apple simulator, iOS stereo viewer workflow, and first-pass visionOS viewer.
- Initial streaming protocol, FEC codec, latency reporting, control channel, and project documentation.
