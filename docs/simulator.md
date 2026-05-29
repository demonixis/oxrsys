# Simulator

## Purpose

The simulator is the local debug path inside the unified viewer app. It lets you validate runtime behavior, frame submission, input plumbing, and rendering without a headset.

## App Layout

The reusable SwiftUI simulator implementation lives in
`clients/Apple/common/OXRSysSimulator/` and exposes `OXRSysSimulatorView`. The standalone Apple app in
`clients/Apple/oxrsys-simulator/` is a thin wrapper around that shared view.

The Qt simulator shared code lives in `clients/Qt/oxrsys-simulator-shared` and is reused by:

- `clients/Qt/oxrsys-simulator`: standalone simulator shell
- `clients/Qt/oxrsys-home`: embedded Developer tab simulator

The viewer exposes two viewing modes:

- `Simulator`: mono preview with simulation controls
- `StereoView`: stereo side-by-side presentation for headset-style viewing on iOS

On macOS, the app is primarily used in `Simulator` mode. On iOS, the same target can switch between `Simulator` and `StereoView` from the in-app settings sheet. The macOS Home can also open `OXRSysSimulatorView` from its Developer tab when Developer Mode is enabled.

## How Simulator Mode Works

The viewer connects to the runtime as a streaming client, using the same UDP protocol as the iOS and visionOS players.

`Simulator` mode:

- Discovers the runtime via UDP broadcast (port 9943)
- Receives encoded video frames and decodes them locally
- Captures keyboard and mouse input and sends simulated tracking data to the runtime
- Displays a single-eye preview across the full screen

The settings sheet also lets you:

- switch between `Simulator` and `StereoView` where supported
- toggle streaming stats
- tune the stereo IPD offset
- request a keyframe
- reset device pose in `StereoView` on iOS

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
use the streaming protocol.

## Controls

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

## Limitations

- Pose quality does not match real headset tracking.
- Simulator timing does not replace real streaming latency measurements.
- Optical characteristics, compositor behavior, and headset-specific runtime behavior still require device validation.
- `StereoView` is a side-by-side viewer mode, not a full optical distortion pipeline.
