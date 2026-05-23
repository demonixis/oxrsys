# macOS Companion App

## Scope

The native macOS companion app lives in `clients/companion/` and provides three tabs:

- `Apps`: scans for compatible apps, manages manually added apps, launches them with
  `XR_RUNTIME_JSON`, and captures stdout/stderr logs.
- `Runtime`: installs a bundled runtime into the user's Application Support directory and registers
  the selected OpenXR runtime manifest for the current user.
- `Streaming`: edits `~/Library/Application Support/OpenXR-OSX/openxr_osx.toml`.

The companion now targets direct notarized distribution instead of App Store/TestFlight sandboxing.
The full launcher and installer need access to `/Applications`, app executables, Terminal launch
scripts, `~/.config/openxr/1/active_runtime.json`, `~/Library/LaunchAgents`, and `launchctl`.

## Build

```bash
xcodebuild -project "clients/companion/OpenXR OSX Companion.xcodeproj" \
  -scheme "OpenXR OSX Companion" \
  -configuration Debug \
  build
```

The Debug build disables Xcode code signing so the command works without a personal development
team. It does not embed a runtime by default.

## Direct Distribution Package

Use the packaging script when producing an app bundle that can install its own runtime:

```bash
scripts/package_companion.sh
```

The script builds the CMake runtime, builds the companion, copies these files into
`OpenXR OSX Companion.app/Contents/Resources/OpenXRRuntime`, and optionally signs the app:

- `libopenxr_osx.dylib`
- `openxr_osx.toml`
- `openxr_osx.json`

Set `CODE_SIGN_IDENTITY="Developer ID Application: ..."` to sign with Hardened Runtime options.
Submit the signed app to Apple's notarization flow outside this script.

## Apps Launcher

On startup and when `Rescan` is pressed, the companion scans:

- `/Applications/*.app`
- `~/Applications/*.app`
- `/Applications/Unity/Hub/Editor/*/Unity.app`

Godot and Unity are recognized from bundle metadata, executable names, and known install paths.
Other compatible apps can be added through drag-and-drop or the `Add App` button. Manual apps and
hidden auto-detected apps are stored in:

```text
~/Library/Application Support/OpenXR-OSX/launcher_apps.json
```

The launcher resolves `Contents/MacOS/<CFBundleExecutable>` and starts it with the inherited
environment plus `XR_RUNTIME_JSON`. The launch manifest preference is:

1. installed runtime manifest
2. selected runtime manifest
3. `build/runtime/openxr_osx.json` for development builds

The logs panel is collapsed by default. Click the log button on an app card or manually expand the
panel to select an app and inspect captured stdout/stderr.

`Terminal` creates a `.command` script under
`~/Library/Application Support/OpenXR-OSX/TerminalLaunchers/` and opens it with Terminal for
debugging.

## Runtime Installation And Registration

The packaged companion installs the bundled runtime into:

```text
~/Library/Application Support/OpenXR-OSX/Runtime/current/
```

It generates an installed `openxr_osx.json` whose `library_path` points to the copied dylib. The
user TOML is seeded only when `~/Library/Application Support/OpenXR-OSX/openxr_osx.toml` does not
already exist. Installation and updates are explicit button actions; the app does not silently
replace an installed runtime.

Runtime registration mirrors `scripts/openxr_runtime_default.sh`:

- `Enable OpenXR Registration` points `~/.config/openxr/1/active_runtime.json` to the selected JSON
- `Update OpenXR Registration` replaces an existing active runtime file or symlink
- it writes `~/Library/LaunchAgents/com.openxr_osx.runtime_env.plist`
- it refreshes `XR_RUNTIME_JSON` in the current GUI session through `launchctl`
- `Disable OpenXR Registration` removes the active runtime link and LaunchAgent

## Streaming Config

The structured editor covers the current runtime keys:

- `general.runtime_enabled`
- `streaming.bitrate_mbps`
- `streaming.fov_degrees`
- `streaming.resolution_scale`
- `streaming.keyframe_interval_sec`
- `streaming.encoder_preset`
- `logging.file_logging`
- `logging.quest_logcat`

The runtime reloads config file changes opportunistically:

- `runtime_enabled` is applied to subsequent `xrCreateInstance` calls
- `fov_degrees` is picked up on subsequent view location work
- `keyframe_interval_sec` is picked up by the encode loop without restarting the process
- `quest_logcat` can start or stop adb capture after a config save
- `bitrate_mbps`, `resolution_scale`, and `encoder_preset` apply when streaming or the encoder is recreated
- file logger sink setup still requires a restart
