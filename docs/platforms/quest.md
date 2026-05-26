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
cd clients/oxrsys-android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

`clients/oxrsys-android/local.properties` must point to the local Android SDK.

## Permissions And Features

Quest hand tracking requires the Android manifest to declare:

- `com.oculus.permission.HAND_TRACKING`
- optional feature `oculus.software.handtracking`

If these entries are missing, the runtime can still operate, but headset-side hand joints will not be available.

USB diagnostics use Android's official `UsbManager` host/accessory intents and filters. The app requests app-level USB permission when Android exposes a real USB device or accessory to the headset. ADB reverse streaming itself does not require or produce that app permission dialog; it may instead trigger the headset's USB debugging authorization prompt when the Mac is first authorized for ADB.

## Preferred Display Refresh

The Quest client's preferred display refresh request is build-configured. The default repository value
is `72`, and you can override it per build with a Gradle property:

```bash
./gradlew assembleDebug -PoxrsysAndroidDisplayRefreshRateHz=72
```

The property is passed through Gradle into CMake as
`OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ`. Set it to a headset-supported rate such as `72`,
`80`, `90`, or `120`. If the runtime does not advertise the requested rate, the client logs the
mismatch and keeps the current headset refresh.

## Runtime Interaction

The Quest client:

- tries USB ADB reverse TCP first, then falls back to local-network UDP discovery when USB is unavailable
- returns to discovery/retry automatically when the runtime or OpenXR app session stops
- connects and advertises codec and refresh-rate preferences
- requests the build-configured display refresh rate when `XR_FB_display_refresh_rate` is available
- receives encoded video frames and matches render-pose metadata to each decoded frame before projection submission
- sends head, controller, and optional hand-tracking data back to the runtime
- reports latency measurements
- requests keyframes when recovery is needed

When supported by the headset, the client also enables a first-pass `XR_FB_foveation` path.

## USB ADB Transport

The USB path is optimized for sideloaded Quest development. The macOS Home can detect an authorized Quest through `adb devices -l`, clear stale reverse mappings, and apply:

```bash
adb -s <serial> reverse tcp:9944 tcp:9944
adb -s <serial> reverse tcp:9945 tcp:9945
adb -s <serial> reverse tcp:9946 tcp:9946
```

With `streaming.transport = "auto"`, the Quest app connects to `127.0.0.1:9946` first. If the ADB reverse control channel answers, the client receives `ServerAnnounce`, opens TCP video and tracking channels, and sends `ClientConnect`. If USB is unavailable, it falls back to WiFi UDP discovery while continuing to retry USB periodically so launch order is not critical. When the runtime closes the USB control/video sockets or video stalls after an app exits, the Quest client resets connection state and returns to the same retry loop without requiring the Android app to be relaunched. With `streaming.transport = "usb_adb"`, the runtime disables WiFi discovery fallback.

USB TCP sends full H.265 NAL records and render-pose records, so UDP FEC and NACK recovery are disabled on this path.

## Current Status

- Real `XR_EXT_hand_tracking` joints are fed from the Android client into the runtime.
- USB ADB reverse TCP streaming is available alongside WiFi UDP streaming.
- Refresh rate is negotiated from the client.
- Latency reporting and keyframe requests are wired into the control path.
- The client applies frame-exact render poses for projection submission so headset compositor reprojection has the pose used to render the displayed frame.
- Dynamic foveation support is present as a first pass and should still be treated as an evolving path.

## Known Limits

- Regular on-headset validation is still required.
- The current path is optimized for low-latency iteration, not for wide-network robustness.
- Rotation smoothness depends on render-pose match rate staying near 100%; misses should be investigated alongside dropped frames, NACKs, and keyframe requests.
- Quest support is ahead of other mobile client targets in the repository.
