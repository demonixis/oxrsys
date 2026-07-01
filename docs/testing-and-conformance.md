# Testing And Conformance

## Scope

This document covers the runtime test suite, Home support tests, and the optional OpenXR-CTS lane.

## Runtime Tests

Build and run host-native runtime checks with:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The default test layers are:

- `oxrsys_runtime_tests`
- `oxrsys_runtime_status_tests`
- `oxrsys_runtime_loader_extension_tests`
- `oxrsys_runtime_api_tests`
- `oxrsys_runtime_d3d_tests` on Windows

`oxrsys_runtime_loader_extension_tests` runs through the OpenXR loader on host builds and verifies
host graphics extension exposure. `oxrsys_runtime_api_tests` is Apple-only in this pass because it
exercises the loader-backed Metal path. Linux builds still run the runtime, config, input, protocol,
status, and loader extension tests. Windows builds additionally run loader-backed D3D11/D3D12 WARP
session and swapchain tests.

## Home Tests

The macOS Home has a small Swift test runner for bundle inspection, launcher persistence
merging, server config serialization, preference persistence, and Terminal command
quoting:

```bash
swiftc -parse-as-library \
  "clients/Apple/oxrsys-home/OXRSys Home/HomeSupport.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/OXRSysServerConfig.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/HomeLauncher.swift" \
  "clients/Apple/oxrsys-home/OXRSys Home/HomePreferences.swift" \
  tests/HomeLauncherTests.swift \
  -o /tmp/oxrsys_home_launcher_tests && /tmp/oxrsys_home_launcher_tests
```

The Qt Home core tests are part of the top-level CTest run when `OXRSYS_BUILD_QT_FRONTENDS` is enabled and Qt6 is available.

## CTS Lane

Enable and run the optional OpenXR-CTS lane with:

```bash
cmake -B build_cts -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOXRSYS_ENABLE_CTS=ON
cmake --build build_cts --target openxr_cts_run
```

Reports:

- `build_cts/reports/openxr-cts/baseline.txt`
- `build_cts/reports/openxr-cts/automated_${plugin}.xml`

On macOS, the CTS external project receives the `metal` and `metallib` paths found by `xcrun`.
If Xcode or the Metal Toolchain was installed after the first CTS configure, reconfigure
`build_cts` so the nested CTS cache does not keep stale `MetalTools_*_NOTFOUND` entries.

## Current Baseline

As of March 17, 2026, the pinned non-interactive baseline is:

- 63 passed
- 36 skipped
- 0 failed

## Merge Expectations

Before considering a change ready:

- run the macOS build and tests
- run the Linux/Qt build on a Linux host when touching Linux runtime or Qt frontend code
- manually validate Vulkan streaming on Linux or MoltenVK when touching Vulkan readback or FFmpeg
  conversion, including H.264 and H.265
- manually validate OpenGL GLX streaming on Linux when touching `XR_KHR_opengl_enable`,
  OpenGL swapchains, or PBO readback
- manually validate D3D11 and D3D12 streaming on Windows hardware or WARP when touching Direct3D
  swapchains, readback, FFmpeg conversion, or Windows graphics extension exposure
- run the Home Swift test runner when changing the Home launcher, preferences, or server config helpers
- run the Android build if Android code changed
- run the CTS lane when runtime API, extension behavior, swapchain handling, action handling, or conformance-sensitive behavior changed

## Documentation Updates

Update this file when:

- test commands change
- test targets change
- CTS reports move
- the tracked CTS baseline changes
