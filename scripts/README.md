# Scripts

## Home Packaging

`scripts/package_home.sh` builds the runtime, builds the macOS Home without Xcode signing,
copies the runtime files into `OXRSys Home.app/Contents/Resources/OXRSysRuntime`, and
optionally signs the app when `CODE_SIGN_IDENTITY` is set.

```bash
scripts/package_home.sh
CODE_SIGN_IDENTITY="Developer ID Application: Example Team" scripts/package_home.sh
```

## Unity

`scripts/unity/Editor/OXRSysRuntimeAutoSelector.cs` is a Unity Editor helper that forces the OpenXR runtime JSON for the current Unity editor process.

### What It Does

- sets `XR_RUNTIME_JSON`
- sets `XR_SELECTED_RUNTIME_JSON`
- sets `OTHER_XR_RUNTIME_JSON`
- remembers the selected path in `EditorPrefs`
- uses `XR_RUNTIME_JSON` as the initial runtime path when it is already set

### How To Use It

1. Copy `scripts/unity/Editor/OXRSysRuntimeAutoSelector.cs` into the `Assets/Editor/` folder of your Unity project.
2. Build the runtime so `build/runtime/oxrsys-runtime.json` exists.
3. Open the Unity editor.
4. Use one of the menu entries under `Tools/OpenXR`:
   - `Use OXRSys Runtime`
   - `Choose Custom Runtime Json...`
   - `Clear Forced Runtime`
   - `Log Active Runtime`

`Use OXRSys Runtime` applies `XR_RUNTIME_JSON` when it points to an existing manifest. If it is not set, the helper asks you to choose the generated `oxrsys-runtime.json`.

### When To Use It

Use this helper when you want Unity to target the local runtime JSON without relying on a shell environment or the user-wide runtime registration helper.

For system-wide registration outside Unity, use `scripts/oxrsys_runtime_default.sh` instead.
