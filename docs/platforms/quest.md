# Quest And Pico

## Scope

This document covers the Android headset client used with Meta Quest and PICO-class devices. It focuses on build, install, permissions, and the current runtime interaction model.

## Requirements

- Android SDK and NDK
- Java 17
- `adb`
- Quest or PICO device in developer mode

See [install.md](../install.md) for the recommended package list and `sdkmanager` commands.

## Build And Install

```bash
cd clients/Android/android-vr
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

`clients/Android/android-vr/local.properties` must point to the local Android SDK.

## Permissions And Features

Quest hand tracking requires the Android manifest to declare:

- `com.oculus.permission.HAND_TRACKING`
- optional feature `oculus.software.handtracking`

If these entries are missing, the runtime can still operate, but headset-side hand joints will not be available.
PICO runtimes expose hand tracking through their OpenXR runtime support; validate this per headset with the log matrix below because Android manifest requirements differ from Meta's Quest permission model.

USB diagnostics use Android's official `UsbManager` host/accessory intents and filters. The app requests app-level USB permission when Android exposes a real USB device or accessory to the headset. ADB reverse streaming itself does not require or produce that app permission dialog; it may instead trigger the headset's USB debugging authorization prompt when the Mac is first authorized for ADB.

## Preferred Display Refresh

The Android client's preferred display refresh request is build-configured. The default repository value
is `72`, and you can override it per build with a Gradle property:

```bash
./gradlew assembleDebug -PoxrsysAndroidDisplayRefreshRateHz=72
```

The property is passed through Gradle into CMake as
`OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ`. Set it to a headset-supported rate such as `72`,
`80`, `90`, or `120`. If the runtime does not advertise the requested rate, the client logs the
mismatch and keeps the current headset refresh.

## Runtime Interaction

The Android client:

- tries USB ADB reverse TCP first, then falls back to local-network UDP discovery when USB is unavailable
- returns to discovery/retry automatically when the runtime or OpenXR app session stops
- connects and advertises codec, refresh-rate preferences, and the headset OpenXR `systemName`
- requests the build-configured display refresh rate when `XR_FB_display_refresh_rate` is available
- receives encoded video frames and matches render-pose metadata to each decoded frame before projection submission
- sends head, controller, and optional hand-tracking data back to the runtime
- reports latency measurements
- requests keyframes when recovery is needed

When supported by the headset, the client also enables a first-pass `XR_FB_foveation` path.

## Controller Profiles And Tracking Flags

The client suggests bindings for:

- Oculus Touch legacy
- Meta Quest 1/Rift S Touch
- Meta Quest 2 Touch
- Meta Touch Plus, used by Quest 3-class controllers
- PICO Neo3
- PICO 4

Unsupported profile suggestions are logged and ignored so the active headset runtime can select the profile it actually exposes. The macOS runtime maps the connected `ClientConnect.deviceName` to the matching canonical OpenXR profile and accepts compatible fallback bindings such as Oculus Touch for Quest and Khronos simple controller where appropriate.
When announced by the headset runtime, the Android client also enables `XR_META_touch_controller_plus` and `XR_BD_controller_interaction` before suggesting those profile families.

Expected Quest profile paths are `/interaction_profiles/oculus/touch_controller` or `/interaction_profiles/meta/touch_controller_quest_1_rift_s` for Quest 1, `/interaction_profiles/meta/touch_controller_quest_2` for Quest 2, and `/interaction_profiles/meta/touch_plus_controller` or `/interaction_profiles/meta/touch_controller_plus` for Quest 3-class Touch Plus controllers.

Controller poses are valid only when the client sets `TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE` or `TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE`. When a controller flag is missing, the runtime leaves that hand's controller actions inactive and keeps the last valid pose internally instead of consuming zeroed packet fields. Hand tracking uses separate hand-active flags and can keep hand-interaction actions active while controller tracking is inactive.

## Log Validation Matrix

For Quest 1, Quest 2, Quest 3, PICO Neo3/PICO 3, and PICO 4, collect `adb logcat` while streaming and confirm:

- `OpenXR system: name=...` identifies the headset model family
- controller bindings are accepted for at least one expected profile
- `xrSyncActions` succeeds and logs non-null profiles for active controllers
- controller locate logs transition to active and tracking packets include controller-active flags while controllers are visible
- runtime logs show nonzero controller poses and the expected canonical profile
- hand tracking logs transition active and set hand-active flags when the headset reports usable joints
- trigger values change independently from squeeze/grab values

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
- Quest and PICO controller profiles are suggested on the Android client, and the runtime gates controller poses with explicit active flags.
- USB ADB reverse TCP streaming is available alongside WiFi UDP streaming.
- Refresh rate is negotiated from the client.
- Latency reporting and keyframe requests are wired into the control path.
- The client applies frame-exact render poses for projection submission so headset compositor reprojection has the pose used to render the displayed frame.
- Dynamic foveation support is present as a first pass and should still be treated as an evolving path.

## Known Limits

- Regular on-headset validation is still required.
- The current path is optimized for low-latency iteration, not for wide-network robustness.
- Rotation smoothness depends on render-pose match rate staying near 100%; misses should be investigated alongside dropped frames, NACKs, and keyframe requests.
- Regular PICO validation is newer than Quest validation and should be kept in the log matrix when controller or hand tracking changes.
