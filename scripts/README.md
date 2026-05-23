# Scripts

## Companion Packaging

`scripts/package_companion.sh` builds the runtime, builds the macOS companion without Xcode signing,
copies the runtime files into `OpenXR OSX Companion.app/Contents/Resources/OpenXRRuntime`, and
optionally signs the app when `CODE_SIGN_IDENTITY` is set.

```bash
scripts/package_companion.sh
CODE_SIGN_IDENTITY="Developer ID Application: Example Team" scripts/package_companion.sh
```

## Unity

`scripts/unity/Editor/OpenXRRuntimeAutoSelector.cs` is a Unity Editor helper that forces the OpenXR runtime JSON for the current Unity editor process.

### What It Does

- sets `XR_RUNTIME_JSON`
- sets `XR_SELECTED_RUNTIME_JSON`
- sets `OTHER_XR_RUNTIME_JSON`
- remembers the selected path in `EditorPrefs`
- uses `XR_RUNTIME_JSON` as the initial runtime path when it is already set

### How To Use It

1. Copy `scripts/unity/Editor/OpenXRRuntimeAutoSelector.cs` into the `Assets/Editor/` folder of your Unity project.
2. Build the runtime so `build/runtime/openxr_osx.json` exists.
3. Open the Unity editor.
4. Use one of the menu entries under `Tools/OpenXR`:
   - `Use OpenXR OSX Runtime`
   - `Choose Custom Runtime Json...`
   - `Clear Forced Runtime`
   - `Log Active Runtime`

`Use OpenXR OSX Runtime` applies `XR_RUNTIME_JSON` when it points to an existing manifest. If it is not set, the helper asks you to choose the generated `openxr_osx.json`.

### When To Use It

Use this helper when you want Unity to target the local runtime JSON without relying on a shell environment or the user-wide runtime registration helper.

For system-wide registration outside Unity, use `scripts/openxr_runtime_default.sh` instead.
