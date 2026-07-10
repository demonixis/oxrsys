# Qt Home

The Qt Home app lives in `clients/Qt/oxrsys-home`. It is Linux-first, with portable launcher paths for macOS and Windows.

Current responsibilities:

- launch compatible apps with `XR_RUNTIME_JSON` and capture stdout/stderr logs
- persist manually added launcher apps under the platform config directory
- scan and accept dropped Linux `.desktop` files, executables, and macOS `.app` bundles for
  Godot/Unity candidates
- create terminal launch scripts for app cards on macOS and Linux without changing the runtime
  manifest selection model
- edit the shared runtime TOML keys for streaming, logging, video codec, encoder preset,
  transport, refresh rate, foveation, upscaling, ABR dynamic resolution, mixed reality, occlusion,
  spatial toggles, and reserved headset-audio configuration
- detect adb devices and configure Quest USB reverse mappings on ports `9944`, `9945`, `9946`, and `9948`
- report macOS WiFi readiness through `networksetup`; Linux keeps the lightweight transport message
- run WiFi readiness, ADB status, device listing, reverse mapping reads, and reverse mapping
  configuration on a serialized worker thread so slow `adb` or `networksetup` calls do not block
  the UI
- show runtime activity and streaming stats from `runtime_status.json`
- register the selected OpenXR runtime on Linux through `${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json`
- launch apps with the manually selected runtime manifest
- open the shared Qt simulator widget from the Developer tab in a reusable `1280x720` window,
  including H.265/H.264 video preview when FFmpeg is available and mouse-driven synthetic head
  tracking

Build with the top-level CMake project:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_BUILD_QT_FRONTENDS=ON
cmake --build build --target oxrsys-home
ctest --test-dir build --output-on-failure
```

On Linux, `OXRSYS_BUILD_QT_FRONTENDS=AUTO` enables the Qt apps when Qt6 Core/Widgets/Network are found.
On macOS, the Qt finder also checks Qt Online Installer prefixes under `~/Qt/<version>/<kit>`,
for example `~/Qt/6.10.2/macos`, in addition to Homebrew, MacPorts, `QTDIR`, and `Qt6_DIR`.

The simulator shared target also has internal tests for UDP frame assembly, FEC recovery, partial
frame drops, ignored render-pose packets, tracking flags, and shift-modified controller movement.

The Settings tab owns runtime selection, registration, and ADB setup. `Update Registration` writes
`${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json` to the selected manifest. Home-launched
apps always use the same selected manifest path; Qt Home does not install, embed, or prefer another
runtime copy.

The Streaming tab autosaves TOML edits after a short debounce. `Default` restores the structured
streaming, general runtime-enabled, and logging keys to their built-in defaults and writes the file
immediately. `Reveal Runtime Logs` opens the platform state directory that contains
`oxrsys-runtime.log`, `oxrsys-headset.log`, and `runtime_status.json` when those files exist.
The bitrate slider uses the shared runtime range, `1` to `200` Mbps. Runtime status reports
`configured_bitrate_mbps` for the Home/server value and `max_bitrate_mbps` for the effective cap after
client negotiation. The Qt simulator sends `ClientConnect.maxBitrateMbps = 0`, so it does not add a
client-side bitrate cap and both values should match when the simulator connects.

The Streaming tab also exposes the shared refresh-rate choices `60`, `72`, `80`, `90`, and `120`
Hz, video codec choices `h265`, `h264`, and `auto`, encoder presets `speed`, `balanced`, and
`quality`, negotiated `encoder_10bit` HEVC Main10 streaming for capable H.265 clients, and
server-side foveated encoding presets `off`, `light`, `medium`, and `high`. It also
exposes `abr_mode` with `off`, `bitrate`, and `full`, `dynamic_resolution_min_scale`, passthrough
enablement, occlusion mode, and the `[spatial]` feature toggles. The runtime stats view shows when a
requested foveated encoding preset is inactive, including the `resolution_scale < 1` guard.
The runtime stats view separates the global passthrough setting from headset support: `unsupported`
means the selected client did not advertise `CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH` after
querying its local OpenXR runtime.
In `abr_mode = "full"`, compatible headset clients can receive `StreamConfigUpdate/Ack` messages on
the reliable USB TCP control path so the runtime changes encoded stream resolution without resizing
the OpenXR application's swapchains. WiFi clients remain bitrate-only for live changes in this
version. The spatial controls and `9948` reverse mapping remain reserved scaffolding until a backend
is attached.
The Headset Client section owns headset-side options:
`auto` leaves Quest/PICO `XR_FB_foveation` unmanaged by Home, while `off`, `light`, `medium`,
and `high` explicitly override the viewer swapchains. Quest shader upscaling and the reserved
headset-audio toggle live there as well. `client_reprojection` controls Quest/PICO missing-frame
smoothing with `off`, `pose`, and `pose_warp`. Audio is not reported active by the runtime until a
real capture/playback stream is implemented.

Transport readiness work is asynchronous. Qt Home shows checking/configuring status while the
worker is running, ignores stale results after ADB mode, ADB path, selected serial, or transport
changes, and keeps the previous main transport selection if USB validation fails.

Platform behavior:

- Linux is the complete target for OpenXR registration, `.desktop` launch, USB ADB, config, state, and launcher persistence.
- macOS can build and use the Qt launcher with `.app` bundles and executables, drag-and-drop
  additions, app-card terminal launch via generated `.command` files, macOS WiFi readiness checks,
  and improved ADB guidance. Runtime registration stays owned by the SwiftUI Home app for now.
- Linux app-card terminal launch tries `x-terminal-emulator`, `gnome-terminal`, `konsole`, then
  `xterm`. No new terminal dependency is required.
- Windows can build the launcher scaffold later, but runtime registration and app-card
  terminal actions are intentionally not implemented yet.

Selecting USB in the header validates the selected or single authorized Quest device and configures
missing reverse mappings automatically before persisting `streaming.transport = "usb_adb"`. If
multiple authorized devices are present, pick one in the Settings ADB panel first.

In `Internal` ADB mode, Qt Home first talks directly to a running local ADB server on
`127.0.0.1:5037`. This avoids launching an `adb` executable for reverse setup when another Android
tool has already started the server. If no server responds, Qt Home falls back to SDK, Homebrew, and
`PATH` executable discovery. The SDK-free native USB ADB backend currently belongs to the macOS
SwiftUI Home app; Qt Home keeps the portable server/executable path. In `Custom` mode, Qt Home tries
only the configured executable path; `Auto Detect` pre-fills that path from the detected external
`adb` executable when one is available.

If USB mode reports missing ADB in Qt Home on macOS and no ADB server is already running, install
the optional `adb-enhanced` fallback with Homebrew:

```bash
brew install adb-enhanced
```

The error message lists the candidate paths that were checked. The Settings ADB panel has `Browse`
and `Auto Detect` actions. A selected custom path is stored in Qt Home `QSettings`, is not shared
with the SwiftUI Home, and must be executable and pass `adb version`. If it becomes empty or invalid,
Qt Home reports that selected path and does not silently fall back to Internal mode.
