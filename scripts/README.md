# Scripts

## macOS Build Package

`scripts/macos_build_package.sh` builds the macOS runtime and `OXRSys Home.app`, then assembles a
single local package folder at `build/OXRSys-macOS/` by default. The folder contains the Home app
and a `runtime/` directory with `liboxrsys-runtime.dylib`, `oxrsys-runtime.json`, and
`oxrsys-runtime.toml`. The manifest copy inside the package is rewritten to load the packaged dylib
with a relative path.

Run it from the repository root:

```bash
./scripts/macos_build_package.sh
```

The default configuration is `Debug`; use `--configuration Release` for a release package, or
`--output-dir` to choose a different package folder.

## macOS Signing And Notarization

`scripts/macos_sign_notarize.sh` signs the built macOS runtime dylib and `OXRSys Home.app`, then
creates one distribution archive under `build/dist/` by default. The archive contains the Home app
and a `runtime/` directory with `liboxrsys-runtime.dylib`, `oxrsys-runtime.json`, and
`oxrsys-runtime.toml`. The manifest copy inside the archive is rewritten to load the packaged dylib
with a relative path.

Pass `--notarize` with `--apple-id`, `--password`, and preferably `--team-id` to submit the same
archive through `xcrun notarytool`. After acceptance, the script staples the ticket to
`OXRSys Home.app` and rebuilds the archive so the distributed zip contains the stapled app. Run
`scripts/macos_sign_notarize.sh --help` for the full option list and command examples.

## Unity

`scripts/unity/` is a Unity Package Manager package named `net.demonixis.oxrsys-unity` for
projects that target OXRSys. Prefer installing it through Unity Package Manager instead of copying
files into `Assets/Editor`.

- `OXRSysRuntimeAutoSelector.cs` forces the OpenXR runtime JSON for the current Unity editor process.
- `OXRSysMacOpenXRLoaderPostprocessor.cs` fixes macOS Player exports by copying Unity's
  OpenXR loader to the bundle path that `UnityOpenXR.dylib` loads at runtime.

### Runtime Selector

- sets `XR_RUNTIME_JSON`
- sets `XR_SELECTED_RUNTIME_JSON`
- sets `OTHER_XR_RUNTIME_JSON`
- remembers the selected path in `EditorPrefs`
- uses `XR_RUNTIME_JSON` as the initial runtime path when it is already set

### macOS Player Loader Postprocessor

Unity's OpenXR package may include the macOS loader in an architecture subdirectory such as
`Contents/PlugIns/ARM64/libopenxr_loader.dylib`, while the runtime plugin loads
`Contents/PlugIns/openxr_loader.dylib`. When that top-level file is missing, a macOS Player can
work in the editor but fail in the exported app with `Failed to load openxr runtime loader.`

`OXRSysMacOpenXRLoaderPostprocessor.cs` runs after macOS builds, copies
`Packages/com.unity.xr.openxr/RuntimeLoaders/osx/libopenxr_loader.dylib` to
`Contents/PlugIns/openxr_loader.dylib`, and ad-hoc signs the app again because adding a file after
Unity signs the bundle invalidates the seal.

### Installation

Add the package from Unity Package Manager with this Git URL:

```text
https://github.com/demonixis/OpenXR-OSX.git?path=/scripts/unity
```

For local development from this checkout, use Package Manager's "Add package from disk..." flow and
select `scripts/unity/package.json`.

See [`unity/README.md`](unity/README.md) for manifest examples and release pinning.

### How To Use It

1. Add `net.demonixis.oxrsys-unity` to the Unity project.
2. Build the runtime so `build/runtime/oxrsys-runtime.json` exists.
3. Open the Unity editor.
4. Use one of the menu entries under `Tools/OpenXR`:
   - `Use OXRSys Runtime`
   - `Choose Custom Runtime Json...`
   - `Clear Forced Runtime`
   - `Log Active Runtime`
5. For macOS Player exports, keep the package installed before building the `.app`.

`Use OXRSys Runtime` applies `XR_RUNTIME_JSON` when it points to an existing manifest. If it is not set, the helper asks you to choose the generated `oxrsys-runtime.json`.

### When To Use It

Use the runtime selector when you want the Unity editor to target the local runtime JSON without
relying on a shell environment or the user-wide runtime registration helper.

Use the macOS loader postprocessor for exported Unity Players that use `com.unity.xr.openxr`.
The postprocessor does not select an OpenXR runtime by itself; launch the app with
`XR_RUNTIME_JSON`, OXRSys Home, or the user-wide runtime registration helper.

For system-wide registration outside Unity, use `scripts/oxrsys_runtime_default.sh` instead.
