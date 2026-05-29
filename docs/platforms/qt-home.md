# Qt Home

The Qt Home app lives in `clients/Qt/oxrsys-home`. It is Linux-first, with portable launcher paths for macOS and Windows.

Current responsibilities:

- launch compatible apps with `XR_RUNTIME_JSON` and capture stdout/stderr logs
- persist manually added launcher apps under the platform config directory
- scan Linux `.desktop` files and macOS `.app` bundles for Godot/Unity candidates
- edit the shared runtime TOML keys for streaming, logging, encoder preset, and transport
- detect adb devices and configure Quest USB reverse mappings on ports `9944`, `9945`, and `9946`
- show runtime activity and streaming stats from `runtime_status.json`
- install and register the user OpenXR runtime on Linux through `${XDG_CONFIG_HOME:-~/.config}/openxr/1/active_runtime.json`
- host the shared Qt simulator widget from the Developer tab

Build with the top-level CMake project:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_BUILD_QT_FRONTENDS=ON
cmake --build build --target oxrsys-home
ctest --test-dir build --output-on-failure
```

On Linux, `OXRSYS_BUILD_QT_FRONTENDS=AUTO` enables the Qt apps when Qt6 Core/Widgets/Network are found.

Platform behavior:

- Linux is the complete target for runtime installation, OpenXR registration, `.desktop` launch, USB ADB, config, state, and launcher persistence.
- macOS can build and use the Qt launcher with `.app` bundles and executables. Runtime registration and installation stay owned by the SwiftUI Home app for now.
- Windows can build the launcher scaffold later, but runtime install and registration are intentionally not implemented yet.
