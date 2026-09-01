# Changes

This file tracks user-facing, integration-facing, and runtime-relevant changes for OXRSys.

## 1.3.0 - TBD

### Added

- Added first-class macOS `arm64` and `x86_64` CI lanes plus universal release packaging with
  architecture validation for the runtime and OXRSys Home.
- Added a macOS/iOS simulator build lane covering the Cardboard-style stereo viewer and ARKit
  tracking integration, alongside the visionOS and Android client builds.
- Added direct IPv4/hostname discovery to the visionOS client. It sends a bounded one-byte unicast
  request on the control port and connects only after the runtime returns a complete
  `ServerAnnounce`, providing a fallback when broadcast discovery cannot use Apple's restricted
  multicast entitlement.
- Added a visionOS "Emulate controllers" toggle so controller-only PCVR games are playable without physical spatial controllers: hand-tracking gestures synthesize VR controllers (index pinch → trigger, middle/ring pinch → face buttons, three-finger curl → grip, wrist → 6DOF pose), and when an Xbox-style gamepad is connected the hand pose plus gamepad buttons/sticks/triggers emulate Meta Touch controllers (compatibility mode takes priority). Emulated controllers are corrected to the Meta/Touch orientation and flow through the existing tracking path.
- Added headset contrast-adaptive sharpening: a `client_sharpening` (0.0-1.0) server setting is carried to the client in the announce, and the visionOS client applies a near-free luma-only contrast-adaptive sharpen in source (video) space — four extra luma taps in the same pass, no second render pass and no added latency — with a matching SwiftUI Home slider.
- Added foveated-stream decode to the visionOS client: it now advertises `CLIENT_CAPABILITY_FOVEATED_ENCODING` and inverse-warps the server's AADT layout in the fragment shader using a closed-form inverse of the server warp (exact to fp32, replacing per-pixel bisection), so `foveated_encoding_preset` takes effect on Vision Pro (previously the client did not advertise support, so the server sent non-foveated video).
- Added per-device render-resolution presets: `render_device = "quest2" | "quest3" | "avp"` selects the per-eye render resolution the runtime advertises to the app (1440x1584 / 1512x1680 / 3024x3360), with matching SwiftUI Home controls. The default (`quest3`) matches the previous fixed 1512x1680; use the existing `resolution_scale` to trim how much of it is encoded and streamed.
- Added negotiated 10-bit H.265 streaming: the visionOS client advertises HEVC Main10 decode support, the runtime requests Main10 only when `streaming.encoder_10bit = true`, the selected codec is H.265, and the connected client supports it, and SwiftUI Home exposes the setting. H.264 and legacy clients remain on the 8-bit path.
- Improved the visionOS control window with explicit discovery and connection states, optional automatic immersive entry, immersive re-entry and disconnect actions, configurable window visibility while immersed, and visible-hands control.
- Added runtime video codec selection with `streaming.video_codec = "h265"`, `"h264"`, or `"auto"`, plus matching SwiftUI Home controls.
- Added conservative codec capability negotiation through `ClientConnect.supportedCodecs`, keeping legacy clients H.265-only while allowing H.264-capable clients to opt in.
- Added H.264 decode support to the Android VR client and shared Apple streaming path, with Android, Apple simulator, and visionOS clients advertising H.264/H.265 while keeping H.265 preferred.
- Added a codec-aware VideoToolbox decoder for Apple clients, including H.264 SPS/PPS and H.265 VPS/SPS/PPS parameter-set handling.
- Added protocol v1.2 stream reconfiguration (`StreamConfigUpdate/Ack`) for reliable USB TCP, dynamic encoded-resolution profiles for `abr_mode = "full"`, global passthrough config with app-driven OpenXR alpha blend/source-alpha detection, headset passthrough support/readiness status, occlusion/spatial config gates, a reserved optional spatial TCP channel on `9948`, and matching SwiftUI Home controls and status display.
- Added runtime status fields for configured bitrate versus effective client-capped bitrate, and for requested/active foveated encoding state so Home can show when a preset is inactive because of `resolution_scale` or client support.
- Added a native USB ADB backend to SwiftUI Home so Quest USB reverse setup can run without Android Studio, the Android SDK, Homebrew, or an `adb` executable.
- Added Settings-based Internal/Custom ADB selection to SwiftUI Home, including editable custom executable paths and auto-detected external `adb` prefills.
- Added bounded world-space reprojection to the visionOS viewer, applying exact rotation and
  translation from each frame's runtime render pose to the live head pose against the shared 2 m
  reprojection plane.

### Changed

- Focused the host runtime exclusively on macOS with Metal, Vulkan/MoltenVK, and VideoToolbox, while
  retaining Quest/Pico, visionOS, and macOS/iOS streaming clients.
- Flattened client sources into `clients/{home,simulator,visionos,android-vr,shared}` and made the
  SwiftUI Home app the single desktop frontend.
- The visionOS client now measures real decode-to-photon latency (renderer pickup wait + in-flight queue + compositor present) per displayed frame and reports it in place of the previous one-refresh compositor guess, so the runtime's pose-prediction horizon covers the actual client display path; the displayed-frame-age field is now populated too.
- The visionOS client now reports measured head linear/angular velocity (differenced from consecutive ARKit samples with light smoothing) in the tracking packet, activating the runtime's preferred client-velocity path for bounded pose prediction instead of its noisier finite differencing of received UDP poses — frames arrive rendered closer to the actual head position.
- Extended visionOS reprojection from rotation-only to a full 6-DOF planar timewarp: the echoed render-pose position (previously discarded) is now kept, and the fragment shader compensates head translation against the shared 2 m reprojection plane for the entire render-to-display latency — up/down/sway no longer lags the full round-trip. Includes the per-eye rotation-induced offset (IPD lever arm) and a clamped delta so a bad pose match cannot distort the warp.
- Replaced the deprecated `LayerRenderer.Drawable.View.tangents` API (visionOS 2.0) with frustum tangents derived from `computeProjection`, keeping the exact (left, right, up, down) magnitudes used by the reprojection shader and the FOV sent to the runtime.
- Marked the visionOS decode/render helper state types `nonisolated` so their off-main access (decode callback, render actor, UDP threads) compiles cleanly under the target's MainActor default isolation, silencing the Swift concurrency warnings. Behavior is unchanged — the types were already lock-guarded `@unchecked Sendable`.
- Reduced visionOS present latency by ~1 frame by lowering the immersive renderer's in-flight buffer count from 3 to 2; the CompositorServices frame clock is the pacer, so the third buffer only added latency for a video blit.
- Reduced visionOS decode latency by preferring the VideoToolbox hardware decoder and enabling real-time decode, and by splitting received NAL units in place instead of copying each whole frame into an array on the decode path.
- Corrected visionOS streamed-video color conversion by defining a BT.709 SDR encoder contract and expanding VideoToolbox limited-range YCbCr with exact 8-bit and 10-bit code ranges before RGB conversion, restoring proper black levels and color balance without changing stream bandwidth.

- Updated the Quest/PICO shell to keep passthrough active only when global passthrough is enabled and the headset reports `XR_FB_passthrough` support, while keeping app alpha-blend passthrough behind the explicit `app_alpha_blend_passthrough` opt-in instead of the normal passthrough toggle. Protocol alpha/source-alpha frames can reveal the passthrough underlay; black-key alpha is limited to an explicit compatibility fallback.
- Updated SwiftUI Home setup flows with first-launch runtime registration guidance, automatic USB reverse configuration when USB is selected, packaged-runtime manifest preference, and native ADB host-server protocol support before falling back to an external `adb` executable.
- Updated documentation for the macOS-only runtime, dual host architectures, protocol v1.2,
  passthrough/MR, native ADB setup, simulator, and visionOS reprojection.

### Fixed

- Fixed visionOS spatial controller buttons going dead after reconnecting while tracking kept working: button state was read from the `GCController` captured when the accessory was created, which goes stale when a controller drops and reconnects. Inputs are now read from the currently connected controller for that hand.
- Fixed streamed controllers appearing frozen at the world origin when the client leaves the aim pose unset: the runtime only treated an all-zero quaternion as "no aim pose", but the Swift packet defaults aim rotation to identity, so it used a zeroed aim pose instead of falling back to the grip pose. Identity-rotation-at-origin is now also treated as unset.
- Fixed the visionOS client never sending a controller aim pose; spatial controllers that publish a distinct aim location (PSVR2 Sense) now report it, and controllers without one continue to fall back to the grip pose.
- Fixed controller interaction-profile reporting so standard OpenXR apps (e.g. Unity with the Oculus Touch controller profile) receive controller input from Meta Quest / PICO headsets. `xrGetCurrentInteractionProfile` now returns the most-specific profile from the runtime's compatibility list that the application actually suggested bindings for (per the OpenXR spec), instead of a device-specific profile the app never bound — the latter made clients treat the controllers as absent even though input was already being routed. Falls back to `XR_NULL_PATH` when the app bound none.
- Contained decode-error corruption on the Apple streaming clients: after a decode failure the decoder drops inter frames and re-requests a keyframe until an IRAP (H.265) or IDR (H.264) arrives, so packet loss shows a brief clean freeze instead of propagating green/blocky corruption.
- Fixed a potential visionOS black screen when the server streams 8-bit H.265 while the client requests a 10-bit decode surface, by falling back to an 8-bit output surface when 10-bit session creation is rejected; the renderer already selects its color conversion from the buffer's actual pixel format.
- Fixed SwiftUI Home USB ADB readiness oscillation by moving USB refresh/setup off the view update path, ignoring stale ADB results after source or device changes, preserving verified reverse ports across transient mapping-read failures, and running persisted USB startup reverse setup only once.
- Fixed Quest shader upscaling sampling so edge-aware neighbor taps stay inside the visible decoded region for each eye instead of sampling the opposite eye, decoder padding, or cropped pixels.
- Fixed Quest passthrough alpha handling so black/dark VR content is no longer treated as transparent by default; only protocol alpha frames use shader alpha unless an explicit compatibility fallback is added.
- Fixed Quest refresh-rate reporting before `ClientConnect` by reading the active display rate after the async Meta refresh request and logging requested versus negotiated rates on both client and server.
- Fixed Quest Android release sideload stability by making `assembleRelease` a debug-signed,
  debuggable stable APK with conservative native flags, and by adding `assembleOptimizedRelease` for
  diagnosing the old optimized/non-debuggable profile.
- Fixed a Unity editor crash on session shutdown by invalidating stale VideoToolbox encode callbacks before the streaming server is destroyed and by catching callback exceptions inside the encoder.
- Fixed the visionOS viewer black screen and doubled AR view by sharing one ARKit world-tracking session between the tracking manager and the immersive renderer, and clearing the drawable depth buffer so the visionOS compositor has a surface to reproject.
- Fixed visionOS eye projection by sending the device's real per-eye FOV and IPD to the runtime, so it renders a matching frustum instead of the symmetric fallback that made the projection look wrong.
- Fixed Vision Pro head-rotation jitter at the source by tagging each streamed frame with the exact head pose captured at `xrLocateViews`, and by dropping out-of-order or duplicate UDP tracking packets before finite-difference prediction.

### Removed

- Removed the non-macOS host runtime backends, their build options and tests, and the portable
  desktop frontend. Released history below remains unchanged.

### Known Limits

- Host runtime support is limited to macOS. Physical Intel Mac, Quest/Pico, Vision Pro, and iPhone
  qualification remains a release gate even when automated builds are green.
- Vulkan automation is limited to building the path, loader/dispatch checks, and the generic
  bounded `HostFence` wait contract. Real MoltenVK image export, fence completion, and streaming
  from a Vulkan OpenXR application remain manual release gates.

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
- Updated the Quest video shader to run foveated decompression once per pixel and linearize upscale neighbor taps instead of repeating binary-search warps for each tap.
- Updated the Quest/PICO shell to pause passthrough, stop local shell interactions, and release shell GL resources while streaming video is actively rendered.
- Updated SwiftUI Home and Qt Home with ABR and Quest client reprojection controls plus runtime status display for frame age, ABR state, and reprojection reuse.
- Updated FFmpeg encoder preset mapping so Linux scaffolding maps `speed`, `balanced`, and `quality` to low-latency FFmpeg presets instead of always using `ultrafast`.
- Updated macOS Home for direct distribution workflows, selected-runtime app launching, runtime registration, package-compatible runtime paths, runtime activity display, and shared Developer simulator integration.
- Updated visionOS streaming behavior around the minimal search window, automatic immersive entry on stream connection, head/hand tracking, and first-pass tracked accessory controller data.

### Fixed

- Fixed Metal streaming frame snapshots so the async encoder reads a release-time staging texture instead of a swapchain slot that the app may already have reused.
- Fixed side-by-side swapchain eye packing by honoring `subImage.imageRect` when compositing each eye for video encode (Unreal Engine and similar clients).
- Fixed server-side foveated encoding on Metal by running the AADT pass through a compute shader into a private GPU scratch texture before blitting into the VideoToolbox pixel buffer, avoiding render-encoder validation aborts on the first encoded frame.
- Fixed Quest connection recovery when a server is discovered but no first video frame arrives, returning the client to discovery/retry instead of leaving the standby/loading screen stuck.
- Fixed controller pose handling so streaming packets only update controller poses when the corresponding controller-active flag is present.
- Fixed float action aggregation so bidirectional axes such as thumbsticks preserve negative deflection instead of being clamped by `std::max()`.
- Fixed hand tracking and hand-interaction coexistence so hand bindings remain available while controller bindings keep priority for shared actions.
- Fixed Quest hand tracking ingestion by feeding real `XR_EXT_hand_tracking` joints from the Android client into the runtime.
- Fixed USB ADB reverse TCP reconnect behavior so closed control/video sockets or video stalls return the Android client to discovery/retry without relaunching the client.
- Hardened Quest USB TCP sends with bounded socket behavior and stale video dispatch cleanup so failed sends do not block encoder callbacks or `Session::EndFrame()`.
- Hardened Quest receive hot paths by reusing TCP/UDP reassembly buffers, avoiding per-packet receive timeout updates, and making USB tracking sends best-effort/non-blocking.
- Hardened Quest headset foveation shutdown by detaching the foveation profile from swapchains before destroying it.
- Hardened runtime-managed Quest logcat capture so it remains optional, bounded, and best-effort during startup.
- Fixed render-pose matching on headset clients so decoded frames are submitted with the pose used to render that frame.
- Fixed Quest refresh-rate reporting by trusting the successful `xrRequestDisplayRefreshRateFB` value instead of immediately querying the asynchronous runtime readback.
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
