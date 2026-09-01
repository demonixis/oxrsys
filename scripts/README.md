# Scripts

## macOS Package Builder

`macos_build_package.sh` builds the macOS runtime and SwiftUI Home app, verifies their architecture
slices, and assembles a relocatable directory at `build/OXRSys-macOS/` by default:

```text
OXRSys Home.app
runtime/liboxrsys-runtime.dylib
runtime/oxrsys-runtime.json
runtime/oxrsys-runtime.toml
```

The package manifest is rewritten to use `./liboxrsys-runtime.dylib`.

```bash
./scripts/macos_build_package.sh
./scripts/macos_build_package.sh \
  --configuration Release \
  --architectures universal \
  --output-dir build/OXRSys-macOS-Release
```

`--architectures` accepts `native`, `arm64`, `x86_64`, or `universal`. Debug defaults to `native`;
Release defaults to `universal`. The same architecture selection is passed to CMake and Xcode, and
the runtime dylib plus Home executable are verified with `lipo` before packaging.

Use `--skip-runtime-build` or `--skip-home-build` only when the reused output was built for the same
requested architectures. The verification still rejects missing slices.

## Signing And Notarization

`macos_sign_notarize.sh` signs the runtime and Home app, creates a combined zip, optionally submits
it through `xcrun notarytool`, staples the accepted Home ticket, and rebuilds the archive.

Build and sign a universal Release:

```bash
./scripts/macos_sign_notarize.sh \
  --build-runtime \
  --build-home \
  --architectures universal \
  --identity "Developer ID Application: Example Team (ABCDE12345)"
```

Add notarization:

```bash
./scripts/macos_sign_notarize.sh \
  --build-runtime \
  --build-home \
  --architectures universal \
  --identity "Developer ID Application: Example Team (ABCDE12345)" \
  --notarize \
  --team-id ABCDE12345 \
  --apple-id developer@example.com \
  --password "xxxx-xxxx-xxxx-xxxx"
```

Never store the signing identity credentials or app-specific password in tracked files. Run either
script with `--help` for path overrides and the full option list.

## Runtime Registration

`oxrsys_runtime_default.sh` updates the current user's macOS OpenXR loader registration. OXRSys Home
is preferred for interactive selection and compatible-app launching because it can show current
runtime status and registration guidance.

## Unity Package

`unity/` is the `net.demonixis.oxrsys-unity` Unity Package Manager package.

- `OXRSysRuntimeAutoSelector.cs` selects the runtime for the current Unity editor process.
- `OXRSysMacOpenXRLoaderPostprocessor.cs` copies Unity's loader to the path expected by exported
  macOS Players and re-signs the modified bundle ad hoc.

Install through Unity Package Manager with:

```text
https://github.com/demonixis/OpenXR-OSX.git?path=/scripts/unity
```

For local development, choose “Add package from disk…” and select `scripts/unity/package.json`.
See [`unity/README.md`](unity/README.md) for manifest examples and release pinning.
