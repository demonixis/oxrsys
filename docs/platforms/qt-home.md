# Qt Home

The Qt Home app lives in `clients/Qt/oxrsys-home`. It is Linux-first, with active Windows support for runtime install, app launching, and optional global OpenXR registration.

Current responsibilities:

- launch compatible apps with `XR_RUNTIME_JSON` and capture stdout/stderr logs
- persist manually added launcher apps under the platform config directory
- scan and accept dropped Linux `.desktop` files, executables, macOS `.app` bundles, and Windows
  `.exe`/`.bat`/`.cmd`/`.lnk` launchers for Godot/Unity candidates
- create terminal launch scripts for app cards on macOS and Linux without changing the runtime
  manifest selection model
- edit the shared runtime TOML keys for streaming, logging, encoder preset, and transport
- detect adb devices and configure Quest USB reverse mappings on ports `9944`, `9945`, and `9946`
- report macOS WiFi readiness through `networksetup`; Linux keeps the lightweight transport message
- run WiFi readiness, ADB status, device listing, reverse mapping reads, and reverse mapping
  configuration on a serialized worker thread so slow `adb` or `networksetup` calls do not block
  the UI
- show runtime activity and streaming stats from `runtime_status.json`
- install and register the user OpenXR runtime on Linux through `${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json`
- install the Windows runtime under `%LOCALAPPDATA%\OXRSys\runtime\current`, launch apps with
  `XR_RUNTIME_JSON`, and optionally register `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` through UAC
- launch apps with either the installed runtime manifest or the manually selected manifest
- open the shared Qt simulator widget from the Developer tab in a reusable `1280x720` window,
  including H.265 video preview when FFmpeg is available and mouse-driven synthetic head tracking

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

The Settings tab separates registration from launch selection. On Linux, `Update Registration`
writes `${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json` to the selected manifest.
On Windows, `Update Registration` writes `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` through
an explicit UAC prompt, stores the previous runtime in `QSettings`, and restores it on unregister
only if ActiveRuntime still points to the OXRSys manifest. The `Use installed runtime for launches`
checkbox controls whether Home-launched apps prefer the installed copy under the platform data
directory or use the manifest selected in the registration field.

The Streaming tab autosaves TOML edits after a short debounce. `Default` restores the structured
streaming, general runtime-enabled, and logging keys to their built-in defaults and writes the file
immediately. `Reveal Runtime Logs` opens the platform state directory that contains
`oxrsys-runtime.log`, `oxrsys-headset.log`, and `runtime_status.json` when those files exist.
The bitrate slider uses the shared runtime range, `1` to `200` Mbps. The Qt simulator sends
`ClientConnect.maxBitrateMbps = 0`, so it does not add a client-side bitrate cap and the runtime
status `max_bitrate_mbps` follows the server config when the simulator connects.

Transport readiness work is asynchronous. Qt Home shows checking/configuring status while the
worker is running, ignores stale results after ADB path, selected serial, or transport changes, and
keeps the previous main transport selection if USB validation fails.

Platform behavior:

- Linux is the complete target for OpenXR registration, `.desktop` launch, USB ADB, config, state, and launcher persistence.
- macOS can build and use the Qt launcher with `.app` bundles and executables, drag-and-drop
  additions, app-card terminal launch via generated `.command` files, macOS WiFi readiness checks,
  and improved ADB guidance. Runtime registration stays owned by the SwiftUI Home app for now.
- Linux app-card terminal launch tries `x-terminal-emulator`, `gnome-terminal`, `konsole`, then
  `xterm`. No new terminal dependency is required.
- Windows scans Start Menu `.lnk` files via the Shell COM API, accepts manual `.exe`, `.bat`,
  `.cmd`, and `.lnk` additions, installs the bundled runtime locally, and launches apps with
  `XR_RUNTIME_JSON` without admin. App-card terminal actions remain hidden on Windows.

Windows notes:

- Build with `-DFFMPEG_ROOT=<path>` when FFmpeg is not discoverable through pkg-config. The runtime
  requires FFmpeg headers/import libraries; the Qt simulator uses the same root opportunistically.
  For `windows-arm64`, use ARM64 FFmpeg and Qt packages and run from an ARM64-capable Visual Studio
  developer environment or matching IDE preset activation.
- Global registration is machine-wide HKLM state. Qt Home uses `ShellExecuteEx(..., "runas")` for
  register/unregister and waits for the elevated helper to finish before refreshing the status.
- Windows runtime launches can use Vulkan, D3D11, or D3D12 OpenXR applications. Video streaming is
  currently limited to common RGBA/BGRA 8-bit Vulkan or DXGI color swapchain formats; unsupported
  formats are logged and dropped without changing the OpenXR wire protocol.

If USB mode reports missing ADB on macOS, install `adb-enhanced` with Homebrew:

```bash
brew install adb-enhanced
```

The error message lists the candidate paths that were checked. The Quest USB ADB panel also has
`Select ADB` and `Auto Detect` actions. A selected custom path is stored in Qt Home `QSettings`,
is not shared with the SwiftUI Home, and must be executable and pass `adb version`. If it becomes
invalid, Qt Home reports that selected path and does not silently fall back until `Auto Detect` clears
the setting.
