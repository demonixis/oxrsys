# iOS Viewer

## Purpose

This page documents the current iOS workflow inside the unified viewer target: a lightweight remote stereo viewer rather than a full standalone OpenXR headset target.

## Intended Role

On iOS, the unified viewer app can switch between:

- `Simulator`: mono preview with touch joysticks for synthetic movement and look
- `StereoView`: side-by-side stereo rendering driven by ARKit device tracking

The iOS path is still a lightweight remote-viewing workflow for experimentation and fast iteration. It does not replace the Quest path or imply native OpenXR parity on iOS.

## Constraints

- iOS does not provide the same native OpenXR ecosystem as Quest-class devices.
- Sensor access, background behavior, and networking rules are more constrained.
- Display and interaction expectations are closer to a lightweight viewer than to a full headset runtime.
- `StereoView` currently uses side-by-side presentation with adjustable IPD offset, not a full lens-distortion stack.

## Design Priorities

- simple install and launch flow
- low-friction experimentation
- clear separation from headset-class features
- realistic scope for a future prototype
- shared implementation in `clients/common/OXRSysSimulator/` to avoid duplicate client stacks

## Current Workflow

- Build the unified target in `clients/oxrsys-simulator/`
- Connect to the runtime from the app landing page
- Open the settings sheet and switch to `StereoView`
- Use the stats overlay and IPD controls as needed
- Use pose reset when AR tracking drift becomes noticeable

## Build

```bash
xcodebuild -project "clients/oxrsys-simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  build
```

## Status

`Prototype in-tree`. The iOS stereo viewer is now part of the unified viewer target, but it remains an experimentation path rather than a headset-class product.
