// SPDX-License-Identifier: MPL-2.0

using System;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace OXRSys.Editor
{
    [InitializeOnLoad]
    internal static class OXRSysRuntimeAutoSelector
    {
        private const string SelectedRuntimeEnvKey = "XR_SELECTED_RUNTIME_JSON";
        private const string OtherRuntimeEnvKey = "OTHER_XR_RUNTIME_JSON";
        private const string RuntimeEnvKey = "XR_RUNTIME_JSON";
        private const string EditorPrefKey = "OXRSys.OpenXR.CustomRuntimeJson";

        static OXRSysRuntimeAutoSelector()
        {
            EditorApplication.delayCall += ApplyConfiguredRuntime;
        }

        [MenuItem("Tools/OpenXR/Use OXRSys Runtime")]
        private static void UseDefaultRuntime()
        {
            string runtimePath = Environment.GetEnvironmentVariable(RuntimeEnvKey);
            if (string.IsNullOrEmpty(runtimePath) || !File.Exists(runtimePath))
            {
                runtimePath = EditorUtility.OpenFilePanel(
                    "Select OXRSys Runtime json",
                    Application.dataPath,
                    "json");
            }

            if (string.IsNullOrEmpty(runtimePath))
            {
                Debug.LogWarning("No OXRSys runtime json selected. Build the runtime or set XR_RUNTIME_JSON first.");
                return;
            }

            EditorPrefs.SetString(EditorPrefKey, runtimePath);
            ApplyConfiguredRuntime(true);
        }

        [MenuItem("Tools/OpenXR/Choose Custom Runtime Json...")]
        private static void ChooseCustomRuntime()
        {
            string currentPath = GetConfiguredRuntimePath();
            string directory = string.IsNullOrEmpty(currentPath) ? Application.dataPath : Path.GetDirectoryName(currentPath);
            string selectedPath = EditorUtility.OpenFilePanel("Select OXRSys Runtime json", directory, "json");

            if (string.IsNullOrEmpty(selectedPath))
            {
                return;
            }

            EditorPrefs.SetString(EditorPrefKey, selectedPath);
            ApplyConfiguredRuntime(true);
        }

        [MenuItem("Tools/OpenXR/Clear Forced Runtime")]
        private static void ClearForcedRuntime()
        {
            EditorPrefs.DeleteKey(EditorPrefKey);
            Environment.SetEnvironmentVariable(SelectedRuntimeEnvKey, string.Empty);
            Environment.SetEnvironmentVariable(OtherRuntimeEnvKey, string.Empty);
            Environment.SetEnvironmentVariable(RuntimeEnvKey, string.Empty);
            Debug.Log("OXRSys runtime override cleared for this Unity editor session.");
        }

        [MenuItem("Tools/OpenXR/Log Active Runtime")]
        private static void LogActiveRuntime()
        {
            string configuredPath = GetConfiguredRuntimePath();
            Debug.Log(
                $"Configured OXRSys runtime: {configuredPath}\n" +
                $"XR_SELECTED_RUNTIME_JSON={Environment.GetEnvironmentVariable(SelectedRuntimeEnvKey)}\n" +
                $"OTHER_XR_RUNTIME_JSON={Environment.GetEnvironmentVariable(OtherRuntimeEnvKey)}\n" +
                $"XR_RUNTIME_JSON={Environment.GetEnvironmentVariable(RuntimeEnvKey)}");
        }

        private static string GetConfiguredRuntimePath()
        {
            string configuredPath = EditorPrefs.GetString(EditorPrefKey, string.Empty);
            if (!string.IsNullOrEmpty(configuredPath))
            {
                return configuredPath;
            }

            return Environment.GetEnvironmentVariable(RuntimeEnvKey) ?? string.Empty;
        }

        private static void ApplyConfiguredRuntime()
        {
            ApplyConfiguredRuntime(false);
        }

        private static void ApplyConfiguredRuntime(bool verbose)
        {
            string runtimePath = GetConfiguredRuntimePath();
            if (string.IsNullOrEmpty(runtimePath) || !File.Exists(runtimePath))
            {
                if (verbose)
                {
                    Debug.LogWarning("OXRSys runtime json not found. Choose a runtime json or set XR_RUNTIME_JSON.");
                }

                return;
            }

            // Unity's OpenXR runtime selector uses these process-level environment variables.
            Environment.SetEnvironmentVariable(OtherRuntimeEnvKey, runtimePath);
            Environment.SetEnvironmentVariable(SelectedRuntimeEnvKey, runtimePath);
            Environment.SetEnvironmentVariable(RuntimeEnvKey, runtimePath);

            if (verbose)
            {
                Debug.Log($"OXRSys runtime forced to: {runtimePath}");
            }
        }
    }
}
