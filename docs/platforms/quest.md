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

USB diagnostics use Android's official `UsbManager` host-device intents and filters. The app requests app-level USB permission only when Android exposes a real USB device to the headset. ADB reverse streaming itself does not require or produce that app permission dialog; it may instead trigger the headset's USB debugging authorization prompt when the Mac is first authorized for ADB.

## Display Refresh

The runtime announces the preferred display refresh selected in Home/config. The Android client
requests that value through `XR_FB_display_refresh_rate` when the extension is available, then reports
the active headset rate back in `ClientConnect.refreshRateHz`. Home exposes `60`, `72`, `80`, `90`,
and `120` Hz.

The repository still keeps a build-configured fallback used before a server is discovered. The
default value is `72`, and you can override it per build with a Gradle property:

```bash
./gradlew assembleDebug -PoxrsysAndroidDisplayRefreshRateHz=72
```

The property is passed through Gradle into CMake as
`OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ`. Set it to a headset-supported rate such as `72`,
`80`, `90`, or `120`. If the headset runtime does not advertise the server-requested rate, the client
logs the mismatch and keeps the current headset refresh.

## Runtime Interaction

The Android client:

- tries USB ADB reverse TCP first, then falls back to local-network UDP discovery when USB is unavailable
- returns to discovery/retry automatically when the runtime or OpenXR app session stops
- connects and advertises codec, active refresh rate, streaming capabilities, and the headset OpenXR `systemName`
- requests the server-announced display refresh rate when `XR_FB_display_refresh_rate` is available
- receives encoded video frames and matches render-pose metadata to each decoded frame before projection submission
- sends head, controller, and optional hand-tracking data back to the runtime
- reports latency measurements
- requests keyframes when recovery is needed

When supported by the headset, the client can enable `XR_FB_foveation` from the server-announced
client foveation preset. `off` disables the profile, while `light`, `medium`, and `high` map to the
runtime's FB foveation levels with dynamic foveation enabled.

The video blit shader supports two server-announced post-processing modes:

- foveated-encoding decompression for the runtime's ALVR-style AADT encoded stream
- optional edge-aware shader upscaling with ALVR defaults: edge threshold `4.0`, sharpness `2.0`,
  and an intended source factor of `1.5`

The shader path does not depend on the proprietary Snapdragon SDK. It keeps the plain bilinear path
when the server does not announce upscaling or foveated encoding.

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

Controller poses are valid only when the client sets `TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE` or `TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE`. The Android client now requires both an active grip-pose action and a valid `xrLocateSpace` result before setting those flags. Trigger, squeeze, thumbstick, and button values are also consumed only when their action state reports `isActive`.

Hand tracking uses separate hand-active flags and remains available while controller tracking is active. The runtime keeps the current interaction profile controller-first for each hand, then falls back to `ext/hand_interaction_ext` when the controller becomes inactive. `xrSyncActions` still evaluates hand-interaction bindings while a controller is active, so hand-only apps can run, but if the same action is bound to both controller and hand-interaction profiles the controller source wins while it is active. When a controller flag is missing, the runtime leaves that hand's controller actions inactive and keeps the last valid controller pose internally instead of consuming zeroed packet fields.

## Log Validation Matrix

For Quest 1, Quest 2, Quest 3, PICO Neo3/PICO 3, and PICO 4, collect `adb logcat` while streaming and confirm. When `logging.quest_logcat` is enabled from Home, the runtime writes the filtered headset log to the platform state directory (`~/Library/Application Support/OXRSys/oxrsys-headset.log` on macOS). Before starting capture, the runtime clears headset logcat best-effort with a timeout and continues even if that clear fails. The equivalent manual capture is:

```bash
adb logcat -c
adb logcat -v time -s 'OXRSys-Android:*' 'OXRSys-Network:*' 'OXRSys-Decoder:*' | tee "$HOME/Desktop/oxrsys-quest-logcat.txt"
```

Confirm:

- `OpenXR system: name=...` identifies the headset model family
- controller bindings are accepted for at least one expected profile
- `xrSyncActions` succeeds and logs non-null profiles for active controllers
- controller locate logs transition to active with `poseActive=1`, a non-null profile, valid locate flags, and controller-active packet flags while controllers are visible
- runtime logs show nonzero controller poses and the expected canonical profile
- hand tracking logs include `locateResult`, `isActive`, `validJoints`, `usable`, and `missing`; they transition active and set hand-active flags when the headset reports usable joints
- trigger values change independently from squeeze/grab values
- releasing controllers clears controller-active packet flags while hand-active flags can remain set

## USB ADB Transport

The USB path is optimized for sideloaded Quest development. The macOS Home can detect an authorized Quest through `adb devices -l`, clear stale reverse mappings, and apply:

```bash
adb -s <serial> reverse tcp:9944 tcp:9944
adb -s <serial> reverse tcp:9945 tcp:9945
adb -s <serial> reverse tcp:9946 tcp:9946
```

With `streaming.transport = "auto"`, the Quest app connects to `127.0.0.1:9946` first. If the ADB reverse control channel answers, the client receives `ServerAnnounce`, opens TCP video and tracking channels, and sends `ClientConnect`. If USB is unavailable, it falls back to WiFi UDP discovery while continuing to retry USB periodically so launch order is not critical. When the runtime closes the USB control/video sockets or video stalls after an app exits, the Quest client resets connection state and returns to the same retry loop without requiring the Android app to be relaunched. With `streaming.transport = "usb_adb"`, the runtime disables WiFi discovery fallback.

The runtime configures accepted USB TCP sockets with `TCP_NODELAY`, `SO_NOSIGPIPE` where available, and a bounded send timeout. If a TCP video send fails or times out, the runtime disables the stale TCP video dispatch path so the encode callback can keep releasing frames and the Android client can reconnect through its existing retry loop.

USB TCP sends full H.265 NAL records and render-pose records, so UDP FEC and NACK recovery are disabled on this path.

## Current Status

- Real `XR_EXT_hand_tracking` joints are fed from the Android client into the runtime.
- Quest and PICO controller profiles are suggested on the Android client, and the runtime gates controller poses and controller actions with explicit active flags while keeping hand tracking available through separate hand-interaction bindings.
- USB ADB reverse TCP streaming is available alongside WiFi UDP streaming.
- Refresh rate is selected by the server/Home, requested by the client, and negotiated back from the active headset rate.
- Latency reporting and keyframe requests are wired into the control path.
- The client applies frame-exact render poses for projection submission so headset compositor reprojection has the pose used to render the displayed frame.
- Dynamic client foveation, shader upscaling, and foveated-encoding decompression are present as evolving paths and should be validated regularly on hardware.
- Headset speaker audio has protocol fields reserved, but the Quest client does not yet play a runtime audio stream.

## Known Limits

- Regular on-headset validation is still required.
- The current path is optimized for low-latency iteration, not for wide-network robustness.
- Rotation smoothness depends on render-pose match rate staying near 100%; misses should be investigated alongside dropped frames, NACKs, and keyframe requests.
- Regular PICO validation is newer than Quest validation and should be kept in the log matrix when controller or hand tracking changes.
