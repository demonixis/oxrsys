# iOS Viewer

## Purpose

This page documents the iOS 17+ workflow inside the unified viewer target: a lightweight remote
stereo viewer rather than a full standalone OpenXR headset target.

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

## Privacy and Physical-Device Discovery Gate

The generated iOS app metadata explains camera access for ARKit tracking and local-network access
for runtime discovery. OXRSys listens for its server announcement with IPv4 UDP broadcast rather
than Bonjour, so `NSBonjourServices` is not required.

Apple's [multicast entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.networking.multicast)
requires the restricted `com.apple.developer.networking.multicast` entitlement for apps that send
or receive IP broadcast on iOS. Do not add that entitlement with an unprovisioned value: the
signing team must first obtain the capability from Apple and include it in the app's provisioning
profile. The generic-device build only validates compilation. Physical discovery remains gated on a
properly provisioned build, Local Network permission, and an on-device UDP broadcast discovery test.

## Design Priorities

- simple install and launch flow
- low-friction experimentation
- clear separation from headset-class features
- realistic scope for a future prototype
- shared implementation in `clients/shared/OXRSysSimulator/` to avoid duplicate client stacks

## Current Workflow

- Build the unified target in `clients/simulator/`
- Connect to the runtime from the app landing page
- Open the settings sheet and switch to `StereoView`
- Use the stats overlay and IPD controls as needed
- Use pose reset when AR tracking drift becomes noticeable

## Build

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

## Status

`Supported client variant`. CI compiles the simulator and generic-device destinations. Cardboard
projection and ARKit tracking remain physical-iPhone release gates.
