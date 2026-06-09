# Simulator

## Purpose

The simulator is the local debug path inside the unified viewer app. It lets you validate runtime behavior, frame submission, input plumbing, and rendering without a headset.

## App Layout

The reusable SwiftUI simulator implementation lives in
`clients/Apple/common/OXRSysSimulator/` and exposes `OXRSysSimulatorView`. The standalone Apple app in
`clients/Apple/oxrsys-simulator/` is a thin wrapper around that shared view.

The Qt simulator shared code lives in `clients/Qt/oxrsys-simulator-shared` and is reused by:

- `clients/Qt/oxrsys-simulator`: standalone simulator shell
- `clients/Qt/oxrsys-home`: Developer tab launcher for a dedicated simulator window

The Apple viewer exposes two viewing modes:

- `Simulator`: mono preview with simulation controls
- `StereoView`: stereo side-by-side presentation for headset-style viewing on iOS

On macOS, the app is primarily used in `Simulator` mode. On iOS, the same target can switch between `Simulator` and `StereoView` from the in-app settings sheet. The macOS Home can also open `OXRSysSimulatorView` from its Developer tab when Developer Mode is enabled.

The Qt simulator is a single `Simulator` mode. It can run as the standalone
`oxrsys-simulator` app or from the Qt Home Developer tab, which opens or reuses a
dedicated `1280x720` simulator window.

## How Simulator Mode Works

The viewer connects to the runtime as a streaming client, using the same UDP protocol as the iOS and visionOS players.

`Simulator` mode:

- Discovers the runtime via UDP broadcast (port 9943)
- Receives encoded video frames and decodes them locally
- Captures keyboard and mouse input and sends simulated tracking data to the runtime
- On macOS, can optionally use a selected webcam for approximate face-derived head tracking and
  Vision hand tracking
- Displays a single-eye preview across the full screen

The Qt simulator uses the same UDP discovery, video, control, and tracking ports. With FFmpeg
development libraries available at build time, the Qt widget decodes the H.265 stream into its
preview surface. That surface is also the interaction target for click, drag, scroll, keyboard focus,
and mouse capture. If no decoded frame is available yet, it shows a synthetic pose preview with a
`Waiting for video` status. If FFmpeg was not enabled, it shows `Video preview unavailable: FFmpeg
support was not enabled` and keeps synthetic tracking available.

The Qt video path uses an internal UDP frame assembler with duplicate-packet filtering, partial-frame
timeouts, existing XOR FEC recovery, dropped-frame counters, and keyframe requests after repeated
loss or decode failures. After a successful decode, the Qt client sends the existing latency report
with receive-to-submit, decode, compositor `0`, and total client latency fields.

The settings sheet also lets you:

- switch between `Simulator` and `StereoView` where supported
- toggle streaming stats
- on macOS, enable webcam tracking, choose the camera, choose whether detected hands are sent as
  hands or simulated controllers, choose the tracking space, tune camera/hand/head offsets, tune
  position and rotation interpolation, movement deadzones, head rotation limits, tune controller
  rotation offsets, and enable face-derived head tracking
- tune the stereo IPD offset
- request a keyframe
- reset device pose in `StereoView` on iOS or recenter webcam head tracking on macOS

### macOS Webcam Tracking

The Apple simulator and the macOS Home integrated simulator share the same webcam tracking path.
The feature is disabled by default and requests camera permission only when enabled from the
simulator settings sheet.

The live webcam path uses Apple frameworks only:

- `AVFoundation` enumerates and captures from the selected camera or a two-camera rig.
- `Vision` detects hand landmarks and face landmarks.
- Face landmarks drive an approximate desktop head pose relative to the first detected or recentered
  face sample.
- The app runtime does not link OpenCV. The optional OpenCV calibration script only writes a JSON
  rig profile that the simulator imports.

The webcam path treats the captured image as the raw camera viewpoint. Capture output is not
mirrored before Vision processing, and preview windows draw raw Vision-normalized points over that
image. Before streaming, the simulator maps those raw camera observations once into the selected
OpenXR tracking space. The default `User-facing Webcam` mode mirrors camera X and resolves hand
side into the user's VR side, so a physical right hand appears as the right hand even when it is on
the left side of the raw camera image. `Raw Camera` mode preserves camera-space X and handedness for
unusual rigs that already account for handedness. Hand and controller positions stay in that
camera-derived tracking space and are not rotated around the face-derived head yaw; turning the head
must not move hands that Vision reports as stationary. Calibrated rig positions also use the rig
profile tracking-space basis directly.

Webcam tracking has two output modes:

- `Hands`: sends `TRACKING_FLAG_LEFT_HAND_ACTIVE` and `TRACKING_FLAG_RIGHT_HAND_ACTIVE` with the
  existing 26-joint payload, and does not mark controllers active. Camera-facing hand side
  resolution happens before streaming so the VR view shows the hand on the user's matching side.
- `Controllers from Hands`: derives controller grip poses from detected wrists/palms, sends
  controller active flags instead of hand active flags, maps index pinch to trigger/select, and maps
  middle/ring/pinky curl to grip/squeeze with lower pinky weight to tolerate weak pinky tracking.
  Controller orientation uses a stable simulator controller basis with only a small bounded
  screen-roll correction from the hand landmarks. It does not directly consume the raw hand
  quaternion reconstructed from Vision landmarks.

Keyboard and mouse head pose remain the fallback when webcam head tracking is disabled or no face is
detected. When webcam tracking is enabled, hand/controller activity comes from the camera rather than
the keyboard controller fallback.

The macOS source mode controls how camera observations are fused:

- `Single Camera`: captures one selected camera and preserves the original markerless webcam
  behavior. Hand depth is estimated from the apparent size of several hand landmark segments, then
  smoothed and clamped to reject large single-frame jumps.
- `Multi-Camera Best View`: captures up to two cameras and picks the freshest, highest-confidence
  face or hand observation per body part. This improves occlusion resilience but still uses the
  single-camera scale-based depth estimate for each selected camera observation.
- `Calibrated Multi-Camera`: imports a rig profile with camera intrinsics and camera-to-tracking
  transforms. When both calibrated cameras see the same face center or hand joints, the simulator
  triangulates their 3D position; otherwise it falls back to best-view fusion. This is the path that
  provides meaningful markerless depth.

The simulator limits live capture to two cameras by default to keep Vision processing bounded. If a
second camera cannot be started, the status text reports a single-camera fallback and tracking
continues.

The calibration controls are available before connecting to the runtime:

- `Mode` selects `Single Camera`, `Multi-Camera Best View`, or `Calibrated Multi-Camera`.
- `Capture` selects the AVFoundation capture preset used before Vision processing. `Low` and
  `Medium` reduce per-camera processing cost and can improve refresh rate when running two cameras;
  `720p` and `1080p` trade more CPU/GPU work for more input detail. If a camera does not support
  the requested preset, the simulator falls back to the next lower supported preset. The default is
  `Medium`, matching the previous capture path.
- `Camera Facing` selects the X-axis and handedness mapping. `User-facing Webcam` is the default
  for desktop webcams that face the user; `Raw Camera` preserves raw image X for calibrated or
  unusual camera setups that already account for handedness.
- `Primary Camera` selects the camera used in single-camera mode and the first rig camera in
  multi-camera modes.
- `Secondary Camera` selects the second rig camera. `Auto Secondary Camera` uses the first available
  camera that is not the primary camera.
- `Open Previews` opens one macOS window per selected/active camera. Each window shows the live
  raw camera image with the detected face center and hand landmark points overlaid in the same raw
  coordinate system that Vision produced. Hand colors identify the corrected left/right streaming
  side, not raw image-left/image-right. The preview badge also shows current estimated `L`/`R` hand
  depth when available. Preview conversion is enabled only while those windows are open.
- `Import Calibration` loads a JSON rig profile generated by `scripts/calibrate_webcam_rig.py`.
- `Tracking Space` selects how the webcam Y coordinate is interpreted. `Local Floor` is the default
  and keeps neutral head height near the configured camera Y. `Local` subtracts the configured
  camera Y so neutral head height is near zero. `Stage` currently uses the same meter coordinates as
  `Local Floor`.
- `Camera Y` is the configured webcam height in meters.
- `Head Offset` applies a meter offset to the face-derived head position after tracking-space
  conversion. Use it when the markerless face pose is consistently too high, low, near, or far.
- `Move Deadzone` ignores small position changes before interpolation so markerless jitter does not
  move the simulated head or hands.
- `Head Position Interpolation` controls smoothing per position axis. `1.0` follows new samples
  immediately; lower values smooth more strongly. The default is `0.1`.
- `Head Rotation Interpolation` controls yaw, pitch, and roll smoothing independently.
  The default is `0.3`.
- `Rot Deadzone` ignores tiny face-derived rotation changes.
- `Head Rotation Limits` clamps yaw, pitch, and roll before smoothing. Lower roll limits are useful
  when lateral face movement is detected as head roll.
- `Hands Y Offset` shifts detected hands and hand-derived controllers vertically to compensate for
  limited webcam field of view. The default is `0.50m`.
- `Hands Position Interpolation` smooths detected hand joints and wrist/controller positions per
  axis.
- `Rotation Interp` smooths the wrist rotation used for hand payload continuity. Simulated
  controllers use the stable controller basis instead of that raw wrist rotation.
- `Depth Scale` and `Depth Offset` calibrate the single-camera hand depth estimate. Use scale for
  proportional errors and offset for constant near/far bias.
- `Depth Smoothing` controls per-hand Z smoothing before 3D joints are built. Lower values smooth
  more strongly; large single-frame jumps are rejected.
- `Controller Rotation Offset` applies an Euler `X/Y/Z` offset after the stable controller basis.
  Each axis is adjustable from `0` to `360` degrees. It is a fine calibration offset only; the
  default is `90, 0, 0`.

Optional calibrated rig profiles can be generated from paired checkerboard captures:

```bash
python3 -m pip install opencv-python
python3 scripts/calibrate_webcam_rig.py \
  --camera "PRIMARY_AVFOUNDATION_ID=calibration/primary" \
  --camera "SECONDARY_AVFOUNDATION_ID=calibration/secondary" \
  --pattern-size 9x6 \
  --square-size 0.024 \
  --output calibration/webcam-rig.json
```

Both image directories must contain matching file stems, for example `001.png` in each directory for
the same checkerboard pose. The first camera becomes the tracking origin in the generated profile.
The `deviceID` values must match the camera IDs reported by AVFoundation, so keep the simulator's
camera picker open when preparing the profile.

`Simulator` mode is useful for:

- quick local checks
- API debugging
- render loop validation
- input system iteration

It is not a replacement for real headset validation.

## macOS TestFlight Packaging

The macOS app target is App Store-ready for upload with a generated `LSApplicationCategoryType` of
`public.app-category.developer-tools` and a sandbox entitlement file. The sandbox keeps UDP client
and server network entitlements enabled because discovery, video receive, tracking, and control all
use the streaming protocol. The standalone simulator also carries the camera sandbox entitlement and
camera usage description required for optional webcam tracking. macOS Home has its own camera usage
description for the integrated simulator window.

## Apple Simulator Controls

| Input | Action |
| --- | --- |
| Right mouse button | Capture or release relative mouse look |
| Mouse move while captured | Head look |
| Mouse wheel | Move forward or backward |
| `Z Q S D` or `W A S D` | Move head |
| `Left Shift` + `W A S D` | Move left controller |
| `Right Shift` + `W A S D` | Move right controller |
| `Q / E` | Roll head |
| Arrow keys | Alternate head look |
| `F / G` | Left or right grip |
| `Escape` | Menu button |
| `T` | Toggle controller mode or hand tracking mode |

## Qt Simulator Controls

| Input | Action |
| --- | --- |
| Right mouse button in the preview | Capture or release mouse look |
| Mouse move while captured, or left-drag | Head look |
| Mouse wheel | Move forward or backward |
| `Z Q S D` or `W A S D` | Move head |
| `Left Shift` + movement | Move left controller |
| `Right Shift` + movement | Move right controller |
| Arrow keys | Alternate head look |
| `R / E` | Roll head |
| `F / G` | Left or right grip |
| `Escape` | Release mouse capture |

## Limitations

- Pose quality does not match real headset tracking.
- macOS webcam head tracking is markerless and approximate. It is intended for desktop simulator
  debugging, not headset-grade 6DOF tracking.
- Full face-expression tracking and Qt simulator webcam parity are not implemented in this pass.
- Simulator timing does not replace real streaming latency measurements.
- Optical characteristics, compositor behavior, and headset-specific runtime behavior still require device validation.
- `StereoView` is a side-by-side viewer mode, not a full optical distortion pipeline.
