# Steam Frame client (draft)

A from-scratch VR streaming client for Valve's **Steam Frame**, paired with the OXRSys
runtime/streamer. Pure portable C++ + Vulkan + OpenXR, hosted-class (the runtime owns
presentation), so the same source builds against a Metal/Vulkan OpenXR runtime on macOS for
development today and retargets to the Frame's runtime by swapping loader discovery.

Status: **feature-complete for everything verifiable without the hardware.** Developed and tested
on macOS against a live OXRSys server (an OpenXR app rendering through the runtime as the content
source), with a local OpenXR compositor standing in for the headset. Opened as a **draft** because
on-device bring-up needs the Frame (see "Remaining" below).

## What's built (verified on macOS)

- **Stereo OpenXR + Vulkan** — two per-eye swapchains submitted as a projection layer, 6DoF head
  pose, startup instance-extension dump (the on-hardware graphics-binding confirmation).
- **Decode** — FFmpeg HEVC/H.264 → RGBA (`video_decoder`), Vulkan texture upload, fullscreen
  sampling pipeline. Side-by-side stereo split per eye.
- **Live receive** — links the in-tree `oxrsys/protocol` lib and reuses the transport
  (`NetworkReceiver` / `TrackingSender`, de-Androidized from `clients/android-vr`);
  `stream_connection` does discovery → receive → ClientConnect and feeds NAL units to the decoder.
- **Motion-to-photon loop** — head/controller/hand pose sent upstream; the projection layer is
  submitted with the server's render pose so the compositor async-timewarps, and the last frame is
  re-presented (reprojected) when a new one hasn't arrived.
- **Input** — action set bound for KHR-simple + Oculus-Touch controllers, plus
  `XR_EXT_hand_tracking` (26 joints) and `XR_EXT_eye_gaze_interaction`.
- **Foveated decode** — un-warps OXRSys's foveated-encoded video (ports the `compressAxis` forward
  map + a binary-search inverse; eye-size ratio derived from the shared `Foveation.h`). Verified:
  with server FFE=medium the encoded frame shrank ~21 % and the read-back eye stayed geometrically
  correct; FFE-off is pixel-identical.
- **Adaptive bitrate** — sends `LatencyReport` (decode + compositor + total) each second; the
  server's ABR consumes it. **Loss recovery** — requests a keyframe on a decode error.
- Headless tests (ctest): decoder produces well-formed stereo RGBA; XOR-FEC recovers a lost packet.

## Remaining (needs the Frame)

- Build for **Linux ARM64** on the "Steam Linux Runtime 3.0 ARM64 (Sniper)"; deploy via the SteamOS
  Devkit Client.
- Switch the graphics binding to **`XR_KHR_vulkan_enable2`** (the Frame's recommended path;
  foveation extensions are Vulkan-gated). The V0 extension dump confirms the exact string on-device.
- **Hardware decode** — swap FFmpeg software decode for Vulkan Video (Adreno) or V4L2/VAAPI
  (software decode is ~110 ms/frame on the Mac; the bottleneck the ABR test surfaces).
- Verify **real gaze / hand / controller** data (all plumbed, unverified on a simulator) and the
  loader / `active_runtime` discovery path on-device.
- Judge **reprojection comfort** under real head motion + network jitter.

## Build (macOS dev)

```bash
cmake -B build -G Ninja \
  -DOPENXR_INCLUDE_DIR=<openxr-sdk>/include -DOPENXR_LOADER_DIR=<openxr-sdk>/lib
cmake --build build
ctest --test-dir build            # headless decoder + FEC tests
```
Needs Vulkan (MoltenVK on macOS), FFmpeg, glslangValidator, and an OpenXR loader + headers.
Run with `FRAME_CLIENT_STREAM=1` and `XR_RUNTIME_JSON` pointed at an OXRSys runtime.

## Notes / follow-ups

- `NetworkReceiver` / `TrackingSender` are de-Androidized copies of the `clients/android-vr`
  transport (only the log macros differ). Factoring the shared transport into `clients/shared/` so
  both clients consume one copy is a natural follow-up.
- The graphics/decode layers are macOS-dev today (MoltenVK + FFmpeg); the retarget items above are
  the Linux/Frame-native equivalents.
