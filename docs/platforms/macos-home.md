# macOS Home App

## Scope

The native macOS Home app lives in `clients/Apple/oxrsys-home/` and provides three default tabs:

- `Apps`: scans for compatible apps, manages manually added apps, launches them with
  `XR_RUNTIME_JSON`, and captures stdout/stderr logs.
- `Settings`: registers the selected OpenXR runtime manifest for the current user and exposes
  Home preferences.
- `Streaming`: autosaves edits to `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`.

When `Developer Mode` is enabled from the `Settings` tab, the main window also shows a `Developer`
tab. Its first tool opens the integrated OXRSys Simulator in a single Home-owned window.

The main window also shows the current runtime activity: idle or streaming, the active transport
when streaming, the connected device family, the OpenXR application name reported by the runtime,
and a WiFi/USB transport readiness control.

The Home app now targets direct notarized distribution instead of App Store/TestFlight sandboxing.
The launcher and registration workflow need access to `/Applications`, app executables, Terminal
launch scripts, `~/.config/openxr/1/active_runtime.json`, `~/Library/LaunchAgents`, and
`launchctl`.

## Build

```bash
xcodebuild -project "clients/Apple/oxrsys-home/OXRSys Home.xcodeproj" \
  -scheme "OXRSys Home" \
  -configuration Debug \
  build
```

The Debug build disables Xcode code signing so the command works without a personal development
team. Home does not embed or install a runtime; the user selects the OpenXR runtime JSON to use.

The Home app links the shared `OXRSysSimulator` Swift package so the Developer tab can host the
simulator without launching a separate app bundle.

## Local Package Folder

Use `scripts/macos_build_package.sh` from the repository root to build the runtime and Home app,
then copy them into `build/OXRSys-macOS/`. That folder keeps `OXRSys Home.app` next to a complete
`runtime/` directory containing the runtime dylib, manifest, and TOML config.

## Direct Distribution Package

Use `scripts/macos_sign_notarize.sh` from the repository root for Developer ID signing and optional
notarization. It can sign the app and runtime from `build/OXRSys-macOS/`, then creates one zip
archive containing the app and `runtime/` directory. With `--notarize`, it submits that archive to
Apple using the provided Apple Developer account email and app-specific password, staples the Home
app after acceptance, and rebuilds the zip so the app in the archive carries the stapled ticket.

The full command examples live in [build.md](../build.md#macos-release-signing-and-notarization).

## Apps Launcher

On startup and when `Rescan` is pressed, the Home app scans:

- `/Applications/*.app`
- `~/Applications/*.app`
- `/Applications/Unity/Hub/Editor/*/Unity.app`

Godot and Unity are recognized from bundle metadata, executable names, and known install paths.
Other compatible apps can be added through drag-and-drop or the `Add App` button. Manual apps and
hidden auto-detected apps are stored in:

```text
~/Library/Application Support/OXRSys/launcher_apps.json
```

The launcher resolves `Contents/MacOS/<CFBundleExecutable>` and starts it with the inherited
environment plus `XR_RUNTIME_JSON`. It always uses the runtime manifest path selected in
Settings. Development builds pre-fill that field with `build/runtime/oxrsys-runtime.json`; changing
the field changes launch behavior directly.

The logs panel is collapsed by default. Click the log button on an app card or manually expand the
panel to select an app and inspect captured stdout/stderr.

`Terminal` creates a `.command` script under
`~/Library/Application Support/OXRSys/TerminalLaunchers/` and opens it with Terminal for
debugging.

## Developer Mode

`Developer Mode` is stored in `UserDefaults` and is off by default. Enable it from OXRSys Home
`Settings` tab to reveal the `Developer` tab.

The `Open Simulator` button opens the shared simulator view in a single SwiftUI window with a
default size of `1280x720`. Repeated clicks focus or reuse that window. Closing the window disconnects
the simulator client through the same cleanup path as the standalone simulator app. Turning Developer
Mode off hides the tab but does not force-close an already-open simulator window.

The `Runtime Stats` section shows compact live streaming telemetry when the runtime is actively
streaming. It keeps the last 60 one-second samples in memory and draws lightweight SwiftUI `Canvas`
graphs for pipeline latency and encode latency. The section is read-only and uses metrics already
computed by the runtime streaming path.

## Runtime Activity Status

The runtime writes a compact status file for the Home app:

```text
~/Library/Application Support/OXRSys/runtime_status.json
```

The file is updated when an OpenXR app creates or destroys an instance, when the streaming server
starts or stops, and when a headset client connects or disconnects. The Home app polls it once per
second and shows:

- state: `Idle`, `Streaming (WiFi)`, or `Streaming (USB)`
- device: `Quest`, `Pico`, `Simulator`, `Vision Pro`, or `Unknown`
- profile app: the OpenXR application name from `XrInstanceCreateInfo`, with the Home-launched
  app as a fallback before the runtime has written status

The status file includes the runtime process id. If that process is no longer alive, the Home app
treats the status as idle so stale streaming state does not survive a crashed app.

When streaming, the runtime may also include a `streaming_stats` object. It is updated at most once
per second from the existing encode telemetry callback and is omitted when the runtime returns to
idle. The fields are:

- `sample_unix_ms`
- `refresh_rate_hz`, `current_bitrate_mbps`, `max_bitrate_mbps`
- `render_width`, `render_height`, `encoded_width`, `encoded_height`
- `encoder_preset`, `foveated_encoding_preset`, `client_foveation_preset`,
  `client_upscaling`, `client_reprojection_mode`, `abr_mode`, `abr_state`,
  `abr_profile`, `headset_audio`
- `latency_ms.server_pipeline`, `latency_ms.client_pipeline`,
  `latency_ms.client_receive_to_submit`, `latency_ms.client_decode`,
  `latency_ms.client_compositor`, `latency_ms.prediction_horizon`,
  `latency_ms.displayed_frame_age`
- `encode_ms.queue_avg`, `encode_ms.queue_p95`, `encode_ms.gpu_avg`, `encode_ms.gpu_p95`,
  `encode_ms.submit_avg`, `encode_ms.submit_p95`, `encode_ms.callback_avg`,
  `encode_ms.callback_p95`, `encode_ms.total_avg`, `encode_ms.total_p95`
- `counters.encoded_frames_total`, `counters.encoder_dropped_frames_total`,
  `counters.replaced_frames_delta`, `counters.keyframe_requests_delta`,
  `counters.pending_depth_max`, `counters.reprojected_frames_delta`,
  `counters.stale_frame_reuses_delta`, `counters.render_pose_fallbacks_delta`

The same header area includes a WiFi/USB selector. Selecting WiFi writes `streaming.transport = "wifi"`
and shows whether the Mac WiFi interface is powered on. Selecting USB writes
`streaming.transport = "usb_adb"` only when `adb` is available. If `adb` is missing, the Home app
keeps the current transport and shows install guidance for `adb-enhanced`, including the Homebrew
command `brew install adb-enhanced`. When `adb` is present, USB mode checks the
selected ADB device for reverse mappings on `9944`, `9945`, and `9946`. If any USB reverse mapping
is missing, the header shows an action state and exposes a `Configure` button; once all ports are
mapped, the button is hidden.

The Quest USB ADB section can also store a custom `adb` executable path in the SwiftUI Home
`UserDefaults`. A selected custom path is tried before SDK, Homebrew, and `PATH` candidates and must
be executable and pass `adb version`. If the custom path becomes invalid, Home reports that path and
does not silently fall back; use `Auto Detect` to clear it and resume automatic detection.

## Runtime Registration

Runtime registration mirrors `scripts/oxrsys_runtime_default.sh`:

- `Enable OpenXR Registration` points `~/.config/openxr/1/active_runtime.json` to the selected JSON
- `Update OpenXR Registration` replaces an existing active runtime file or symlink
- it writes `~/Library/LaunchAgents/net.demonixis.oxrsys.runtime-env.plist`
- it refreshes `XR_RUNTIME_JSON` in the current GUI session through `launchctl`
- `Disable OpenXR Registration` removes the active runtime link and LaunchAgent

## Streaming Config

The structured editor covers the current runtime keys:

- `general.runtime_enabled`
- `streaming.bitrate_mbps`
- `streaming.resolution_scale`
- `streaming.refresh_rate_hz`
- `streaming.keyframe_interval_sec`
- `streaming.encoder_preset`
- `streaming.transport`
- `streaming.foveated_encoding_preset`
- `streaming.client_foveation_preset`
- `streaming.client_upscaling`
- `streaming.client_reprojection`
- `streaming.abr_mode`
- `streaming.headset_audio`
- `logging.file_logging`
- `logging.quest_logcat`

The bitrate control accepts the shared runtime range, `1` to `200` Mbps. Apple
and Qt simulator clients do not add their own bitrate ceiling, so the runtime
status `max_bitrate_mbps` should reflect the configured value when those
clients connect.

The refresh control writes one of `60`, `72`, `80`, `90`, or `120` Hz. The
runtime announces that value, and Quest clients request it through
`XR_FB_display_refresh_rate` before reporting the active display rate back.

Simulator FOV is configured in the simulator window/settings sheet. Home does
not write `streaming.fov_degrees`; the runtime only keeps that key as a legacy
fallback for clients that do not send `TrackingPacket.eyeFov`.

`foveated_encoding_preset` controls the server-side ALVR-style AADT video
compression path on supported server/client combinations.

The Headset Client section owns client-side headset options. `client_foveation_preset = "auto"`
does not send an `XR_FB_foveation` override to the headset client; `off`, `light`, `medium`,
and `high` explicitly override the Quest/PICO viewer swapchains. This is separate from
foveated encoding and does not change the desktop OpenXR application's rendering work.
`client_upscaling` enables the Quest shader upscaling path. `headset_audio` is reserved in
config and protocol, but the runtime does not advertise audio as active until a real
capture/playback path is attached.

`client_reprojection` controls short missing-frame smoothing on the Quest client. The default
`pose` reuses a recent decoded texture with the matched server render pose; `pose_warp` additionally
allows a small GLES image-space orientation correction when safety checks pass. `off` disables the
stale-frame reprojection path.

`abr_mode` controls server-side adaptive bitrate. `bitrate` adjusts bitrate using latency, displayed
frame age, drops, keyframe requests, and reprojection pressure. `full` currently exposes profile
selection in runtime status for session-safe resolution/foveation/upscaling transitions.

Changes in the Streaming tab are saved automatically after a short debounce. `Reload From Disk`
discards unsaved UI edits and reparses the TOML. `Default` restores the structured streaming,
general runtime-enabled, and logging keys to their built-in defaults and writes the file
immediately. `Reveal Config` opens the TOML location, and `Reveal Runtime Logs` opens
`~/Library/Application Support/OXRSys/`, which contains `oxrsys-runtime.log`,
`oxrsys-headset.log`, and `runtime_status.json` when those files exist.

The runtime reloads config file changes opportunistically:

- `runtime_enabled` is applied to subsequent `xrCreateInstance` calls
- `keyframe_interval_sec` is picked up by the encode loop without restarting the process
- `quest_logcat` can start or stop adb capture after the autosaved config is written; the runtime
  clears headset logcat best-effort with a timeout before capture and continues if that clear fails
- `bitrate_mbps`, `resolution_scale`, `refresh_rate_hz`, `encoder_preset`, `transport`,
  `foveated_encoding_preset`, `client_foveation_preset`, `client_upscaling`,
  `client_reprojection`, `abr_mode`, and `headset_audio` apply when streaming or the
  encoder/client connection is recreated
- file logger sink setup still requires a restart

The Quest USB ADB section detects authorized `adb` devices, applies reverse mappings for ports `9944`, `9945`, and `9946`, then verifies them with `adb reverse --list`. This prepares the USB TCP transport; it is separate from Android `UsbManager` app permission prompts.
