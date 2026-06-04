# Qt Home

The Qt Home app lives in `clients/Qt/oxrsys-home`. It is Linux-first, with portable launcher paths for macOS and Windows.

Current responsibilities:

- launch compatible apps with `XR_RUNTIME_JSON` and capture stdout/stderr logs
- persist manually added launcher apps under the platform config directory
- scan and accept dropped Linux `.desktop` files, executables, and macOS `.app` bundles for
  Godot/Unity candidates
- create terminal launch scripts for app cards on macOS and Linux without changing the runtime
  manifest selection model
- edit the shared runtime TOML keys for streaming, logging, encoder preset, and transport
- detect adb devices and configure Quest USB reverse mappings on ports `9944`, `9945`, and `9946`
- report macOS WiFi readiness through `networksetup`; Linux keeps the lightweight transport message
- show runtime activity and streaming stats from `runtime_status.json`
- register the selected OpenXR runtime on Linux through `${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json`
- launch apps with the manually selected runtime manifest
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

The Settings tab owns runtime selection and registration. `Update Registration` writes
`${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json` to the selected manifest. Home-launched
apps always use the same selected manifest path; Qt Home does not install, embed, or prefer another
runtime copy.

The Streaming tab autosaves TOML edits after a short debounce. `Default` restores the structured
streaming, general runtime-enabled, and logging keys to their built-in defaults and writes the file
immediately. `Reveal Runtime Logs` opens the platform state directory that contains
`oxrsys-runtime.log`, `oxrsys-headset.log`, and `runtime_status.json` when those files exist.

Platform behavior:

- Linux is the complete target for OpenXR registration, `.desktop` launch, USB ADB, config, state, and launcher persistence.
- macOS can build and use the Qt launcher with `.app` bundles and executables, drag-and-drop
  additions, app-card terminal launch via generated `.command` files, macOS WiFi readiness checks,
  and improved ADB guidance. Runtime registration stays owned by the SwiftUI Home app for now.
- Linux app-card terminal launch tries `x-terminal-emulator`, `gnome-terminal`, `konsole`, then
  `xterm`. No new terminal dependency is required.
- Windows can build the launcher scaffold later, but runtime registration and app-card
  terminal actions are intentionally not implemented yet.

If USB mode reports missing ADB on macOS, install `adb-enhanced` with Homebrew:

```bash
brew install adb-enhanced
```

The error message lists the candidate paths that were checked. The Quest USB ADB panel also has
`Select ADB` and `Auto Detect` actions. A selected custom path is stored in Qt Home `QSettings`,
is not shared with the SwiftUI Home, and must be executable and pass `adb version`. If it becomes
invalid, Qt Home reports that selected path and does not silently fall back until `Auto Detect` clears
the setting.
