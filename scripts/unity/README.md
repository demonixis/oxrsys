# OXRSys Unity

`net.demonixis.oxrsys-unity` is an editor-only Unity Package Manager package for projects that
target OXRSys.

It provides:

- `Tools/OpenXR` menu entries to force an OXRSys runtime JSON for the current Unity editor process.
- A macOS Player postprocessor that copies Unity's OpenXR loader to the bundle path loaded by
  `UnityOpenXR.dylib` and ad-hoc signs the `.app` again after the copy.

## Installation

Add the package from Unity Package Manager with this Git URL:

```text
https://github.com/demonixis/OpenXR-OSX.git?path=/scripts/unity
```

For a locked release, append an existing tag or full commit hash after the package path:

```text
https://github.com/demonixis/OpenXR-OSX.git?path=/scripts/unity#<tag-or-full-commit-hash>
```

You can also edit the Unity project manifest directly:

```json
{
  "dependencies": {
    "net.demonixis.oxrsys-unity": "https://github.com/demonixis/OpenXR-OSX.git?path=/scripts/unity"
  }
}
```

For local package development, use Package Manager's "Add package from disk..." flow and select
`scripts/unity/package.json`.

## Runtime Selector

Build the OXRSys runtime so `build/runtime/oxrsys-runtime.json` exists, then use one of the menu
entries under `Tools/OpenXR`:

- `Use OXRSys Runtime`
- `Choose Custom Runtime Json...`
- `Clear Forced Runtime`
- `Log Active Runtime`

`Use OXRSys Runtime` applies `XR_RUNTIME_JSON` when it points to an existing manifest. If it is not
set, the helper asks you to choose the generated `oxrsys-runtime.json`.

## macOS Player Loader Postprocessor

Keep the package installed before exporting macOS Unity Players that use `com.unity.xr.openxr`.
Unity's OpenXR package can place the macOS loader in an architecture subdirectory such as
`Contents/PlugIns/ARM64/libopenxr_loader.dylib`, while `UnityOpenXR.dylib` loads
`Contents/PlugIns/openxr_loader.dylib`. Without the top-level loader, the editor can work while the
exported Player logs `Failed to load openxr runtime loader.` and never starts XR.

The postprocessor copies
`Packages/com.unity.xr.openxr/RuntimeLoaders/osx/libopenxr_loader.dylib` to
`Contents/PlugIns/openxr_loader.dylib`, then ad-hoc signs the app because adding a file after Unity
signs the bundle invalidates the seal.

The postprocessor only fixes the Unity Player bundle layout. The exported Player still needs runtime
selection through `XR_RUNTIME_JSON`, OXRSys Home, or `scripts/oxrsys_runtime_default.sh`.
