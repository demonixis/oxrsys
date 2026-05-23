# Quest

## Scope

This document covers the Android headset client used with Meta Quest-class devices. It focuses on build, install, permissions, and the current runtime interaction model.

## Requirements

- Android SDK and NDK
- Java 17
- `adb`
- Quest device in developer mode

See [install.md](../install.md) for the recommended package list and `sdkmanager` commands.

## Build And Install

```bash
cd clients/android-openxr
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

`clients/android-openxr/local.properties` must point to the local Android SDK.

## Permissions And Features

Quest hand tracking requires the Android manifest to declare:

- `com.oculus.permission.HAND_TRACKING`
- optional feature `oculus.software.handtracking`

If these entries are missing, the runtime can still operate, but headset-side hand joints will not be available.

The Quest client's preferred display refresh request is build-configured. The default repository value
is `72`, and you can override it per build with a Gradle property:

```bash
./gradlew assembleDebug -PopenxrClientDisplayRefreshRateHz=72
```

The property is passed through Gradle into CMake as
`OPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ`. Set it to a headset-supported rate such as `72`,
`80`, `90`, or `120`. If the runtime does not advertise the requested rate, the client logs the
mismatch and keeps the current headset refresh.

## Runtime Interaction

The Quest client:

- discovers the runtime on the local network
- connects and advertises codec and refresh-rate preferences
- requests the build-configured display refresh rate when `XR_FB_display_refresh_rate` is available
- receives encoded video frames and matches render-pose metadata to each decoded frame before projection submission
- sends head, controller, and optional hand-tracking data back to the runtime
- reports latency measurements
- requests keyframes when recovery is needed

When supported by the headset, the client also enables a first-pass `XR_FB_foveation` path.

## Current Status

- Real `XR_EXT_hand_tracking` joints are fed from the Android client into the runtime.
- Refresh rate is negotiated from the client.
- Latency reporting and keyframe requests are wired into the control path.
- The client applies frame-exact render poses for projection submission so headset compositor reprojection has the pose used to render the displayed frame.
- Dynamic foveation support is present as a first pass and should still be treated as an evolving path.

## Known Limits

- Regular on-headset validation is still required.
- The current path is optimized for low-latency iteration, not for wide-network robustness.
- Rotation smoothness depends on render-pose match rate staying near 100%; misses should be investigated alongside dropped frames, NACKs, and keyframe requests.
- Quest support is ahead of other mobile client targets in the repository.
