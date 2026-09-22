# frame-client

A from-scratch VR streaming client for the **Steam Frame**, fed by the **oxrsys**
runtime/streamer on the Mac (design B). See `../plan.md` § "Frame client build target" for the
full milestone plan (V0–V6) and `../docs/steam-frame.md` for the hardware/runtime facts.

Pure portable C++ + Vulkan + OpenXR. **No windowing** — the runtime owns presentation (hosted
class), so the same source runs against DisplayXR (MoltenVK) on the Mac today and retargets to
the Frame's runtime by only swapping loader discovery.

## Status

- **V0 — hello stereo: DONE (2026-09-20).** Vulkan + OpenXR, two per-eye swapchains submitted as
  an `XrCompositionLayerProjection`, 6DoF head-pose located each frame, and a full
  instance-extension dump at startup (the on-hardware deliverable — confirms the Frame's exact
  Vulkan-enable string once run there). Renders a flat per-eye clear color (left reddish, right
  bluish) to prove the stereo path with no pipeline/shader/geometry. Verified against DisplayXR
  1.6.0: 2 views @ 1512×1964, `vulkan_enable`+`vulkan_enable2` both present.
- **V1 — decode a file: DONE (2026-09-20).** `src/video_decoder.{h,cpp}` (FFmpeg: HEVC → RGBA
  via libavcodec + swscale, loops at EOF) feeds a Vulkan texture (staging→device, layout
  transitions), sampled by a fullscreen-triangle pipeline (`shaders/`) that splits a **side-by-side
  stereo** frame per eye via push-constant UV (left eye [0–0.5], right eye [0.5–1.0]). Test clip
  `assets/stereo_test.hevc` (1920×1080 SBS). Decode path pixel-verified (`FRAME_CLIENT_DUMP=1`
  writes the exact GPU frame). FFmpeg software decode now; Vulkan Video / VAAPI hwaccel is a V6
  swap behind the same `VideoDecoder` seam.
- **V2 — receive live stereo: DONE (2026-09-20).** Ported oxrsys `NetworkReceiver` +
  `TrackingSender` (de-Androidized) into `src/`, using the canonical in-tree protocol headers at
  `common/protocol/include` rather than a vendored copy.
  `stream_connection.{h,cpp}` does discovery → StartReceiving → ClientConnect and feeds NAL units
  to the decoder's streaming path (`OpenStream`/`SubmitNal`/`TakeLatestRGBA`, thread-safe
  latest-frame slot). Verified on the Mac against a live oxrsys server (screencap app rendering
  through oxrsys as the content source): discovered the server, connected, decoded live HEVC
  (2272×1264) with no errors. Run with `FRAME_CLIENT_STREAM=1` (or `--stream`).
- **Tests (ctest):** `frame_client_tests` — decoder produces well-formed SBS-stereo RGBA;
  XOR-FEC recovers a lost packet. Headless, no runtime needed. `ctest --test-dir build`.
- **V3 — pose loop + reprojection: DONE (2026-09-20).** Head pose ships upstream each frame
  (`TrackingSender`) so the server renders for it. For async timewarp, the projection layer is
  submitted with the pose **the server rendered for** (oxrsys's render-pose packets, surfaced via
  `StreamConnection::LatestRenderPose`) — the runtime compositor then reprojects the frame to the
  actual display pose. The last texture is re-presented every vsync (reprojected) when no new
  frame arrived, so dropped/late frames don't judder. Verified live: render poses received and in
  use. (Comfort under real head motion + network jitter is the hardware-gated verdict; explicit
  client-side warp beyond the compositor is a possible V6 add.)
- **V4 — input: DONE (2026-09-20).** `xr_input.{h,cpp}`: an action set (grip poses, trigger,
  squeeze, thumbstick, A/B) bound for the KHR simple controller (universal) and Oculus Touch
  (the Frame emulates it), plus `XR_EXT_hand_tracking` (26 joints, gracefully off where the
  system lacks it). Each frame it fills the oxrsys tracking packet's controller/input/hand
  fields, sent upstream alongside the head pose. Verified live: both controllers located and
  sent (`trackingFlags=0xc`).
- **V5 — gaze + foveation/audio: PARTIAL (2026-09-20).** Eye gaze
  (`XR_EXT_eye_gaze_interaction`) is captured in `xr_input` (action + space + forward-direction
  extraction) — the client-side half of gaze foveation, ready for a gaze-aware server. **oxrsys's
  foveated encoding is fixed-center (no gaze uplink in its protocol), so gaze-driven foveation is
  not applicable to oxrsys today** — documented, not blocked-on-us. Gaze pose is hardware-gated
  (DisplayXR's sim doesn't drive it, like hand tracking). **Audio deferred:** oxrsys streams no
  audio server-side (no Mac capture path), so there's no test path; the receive/playback scaffold
  is left for the Frame phase.
- **V6 — hardware headroom: PARTIAL (2026-09-20), the testable parts done.**
  - **Adaptive bitrate feed:** the client sends `LatencyReport` (decode + compositor + total) over
    the control channel each second; **verified live — oxrsys's ABR consumes it and adjusts
    bitrate** (22→17 Mbps in response to the reported software-decode latency).
  - **Display refresh:** requests the runtime's highest rate via `XR_FB_display_refresh_rate`.
  - Deferred to the Frame: **hardware decode** (Vulkan Video/VAAPI — FFmpeg software now) and
    **foveated decode** (the AADT decompression shader; needs a foveated-encoded stream + is a
    substantial shader). Full per-eye res + 120/144 Hz land with the hardware.
- **Mac-side ceiling reached:** V0–V4 done, V5/V6 done except the pieces that inherently need the
  Frame (hardware decode, real gaze/hands, foveated-decode, comfort verdict).

### Cleanups + robustness (branch `mac-cleanups`, 2026-09-21)
- **VIEW-space head pose** sent upstream (was the left-eye pose — a half-IPD skew).
- **Honest refresh-rate log** (set vs. not-supported), matching the runtime now returning
  `FUNCTION_UNSUPPORTED` when it can't change rate.
- **Swapchain-image readback** (`FRAME_CLIENT_DUMP` → `/tmp/frame_client_eye0.rgba`): reads the
  rendered eye back for a gold-standard visual, working around DisplayXR's atlas hook missing our
  two-swapchain layout. Confirmed the full screen→eye pipeline visually.
- **Keyframe recovery**: on a decode error (lost reference frame) the client sends a
  `RequestKeyframe` (rate-limited) to resync. Fires only on real errors (none on clean loopback);
  full verification needs induced loss on hardware.
- **Foveated decode** (AADT un-warp): the client reads the server's announced foveation preset +
  params, derives the eye-size ratio via the shared `Foveation.h`, and the fragment shader ports
  oxrsys's `compressAxis` + binary-search inverse to un-warp each eye before sampling. Verified by
  enabling server FFE (medium) — encoded frame shrank ~21% (2272×1264 → 1792×896) and the read-back
  eye is geometrically correct; FFE-off is pixel-identical to before.
- Still deferred (no Mac test path / marginal): audio (oxrsys streams none on the Mac),
  server→client control (BitrateUpdate / dynamic-resolution reconfig).

### Foveation reconstruction hardened (branch `frame-client`, 2026-09-21)
- **Headless accuracy test across every preset** (`tests/test_foveation.cpp`): drives the warp
  through the shared `Foveation.h` layout math at the Frame's real per-eye 2160px, and measures the
  reconstruction the shader relies on (`compressAxis(decompressAxis(u)) == u`). Only Medium had ever
  been checked (visually); Light and High were unverified.
- **Fix it surfaced:** the shader's binary-search inverse ran 10 iterations, which the periphery
  amplified by up to `edgeRatio` (~7 at High) into **~5.7px of reconstruction error**, growing with
  preset strength. Bumped to 16 iterations (shader + the test's `foveation_warp.h` oracle, kept in
  sync) → worst case now **<0.1px** across Light/Medium/High. Still trivial per-pixel cost.

**Mac-doable work is complete.** Everything the Mac can build and verify is done; what remains is
hardware-gated (aarch64/Sniper build, hardware decode, real gaze/hands, comfort verdict).

## Run V2 (live stream from oxrsys)

```bash
# 1) content source: an oxrsys app rendering through the oxrsys runtime
env XR_RUNTIME_JSON=../repos/oxrsys/build/runtime/oxrsys-runtime.json <some-oxrsys-app>
# 2) our client, presenting via DisplayXR, receiving the stream
DXR=../repos/displayxr-runtime/_package/DisplayXR-macOS
env FRAME_CLIENT_STREAM=1 XR_RUNTIME_JSON="$DXR/openxr_displayxr.json" \
    DYLD_LIBRARY_PATH="$DXR/lib" XRT_PLUGIN_SEARCH_PATH="$DXR/lib/displayxr/plugins" \
    ./build/frame_client
```

## Run V1 (decode a file)

```bash
DXR=../repos/displayxr-runtime/_package/DisplayXR-macOS
env XR_RUNTIME_JSON="$DXR/openxr_displayxr.json" DYLD_LIBRARY_PATH="$DXR/lib" \
    XRT_PLUGIN_SEARCH_PATH="$DXR/lib/displayxr/plugins" SIM_DISPLAY_OUTPUT=anaglyph \
    ./build/frame_client ./assets/stereo_test.hevc
```
Video path: arg 1, else `$FRAME_CLIENT_VIDEO`, else `assets/stereo_test.hevc`.
`FRAME_CLIENT_DUMP=1` writes the first decoded frame to `/tmp/frame_client_decoded.rgba`.

## Build (Mac dev, against DisplayXR)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/openxr-install-1.1.63
cmake --build build
```

`DXR_ROOT` (default `../repos/displayxr-runtime`) supplies the vendored OpenXR headers;
the loader is found via `CMAKE_PREFIX_PATH` / `OPENXR_LOADER_DIR` / the DisplayXR dev install.

## Run (against DisplayXR)

```bash
DXR=../repos/displayxr-runtime/_package/DisplayXR-macOS
env XR_RUNTIME_JSON="$DXR/openxr_displayxr.json" \
    DYLD_LIBRARY_PATH="$DXR/lib" \
    XRT_PLUGIN_SEARCH_PATH="$DXR/lib/displayxr/plugins" \
    SIM_DISPLAY_OUTPUT=anaglyph \
    ./build/frame_client
```

Against oxrsys instead: point `XR_RUNTIME_JSON` at `../repos/oxrsys/build/runtime/oxrsys-runtime.json`.

## Retargeting to the Frame (later)

- Graphics binding: V0 uses `XR_KHR_vulkan_enable` (v1), universally supported. The Frame also
  offers `enable2` (recommended, foveation-gated) — switch when targeting hardware.
- Loader: bundle one from OpenXR-SDK or use the Frame's system loader; confirm the aarch64
  `active_runtime` path on-device (see `../docs/steam-frame.md`).
- No Objective-C / Cocoa / MoltenVK-portability code is load-bearing here — the portability
  bits are guarded and simply no-op on Linux.
