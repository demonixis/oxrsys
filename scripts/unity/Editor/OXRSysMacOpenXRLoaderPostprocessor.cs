// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using System.IO;
using UnityEditor;
using UnityEditor.Callbacks;
using PackageInfo = UnityEditor.PackageManager.PackageInfo;
using UnityDebug = UnityEngine.Debug;

namespace OXRSys.Editor
{
    internal static class OXRSysMacOpenXRLoaderPostprocessor
    {
        private const string LoaderPackageAssetPath = "Packages/com.unity.xr.openxr/RuntimeLoaders/osx/libopenxr_loader.dylib";
        private const string LoaderPackageRelativePath = "RuntimeLoaders/osx/libopenxr_loader.dylib";
        private const string LoaderBundleRelativePath = "Contents/PlugIns/openxr_loader.dylib";

        [PostProcessBuild(100)]
        private static void CopyOpenXRLoader(BuildTarget target, string pathToBuiltProject)
        {
            if (target != BuildTarget.StandaloneOSX)
            {
                return;
            }

            if (string.IsNullOrEmpty(pathToBuiltProject) || !Directory.Exists(pathToBuiltProject))
            {
                UnityDebug.LogWarning($"OXRSys OpenXR loader copy skipped: build output not found at {pathToBuiltProject}");
                return;
            }

            PackageInfo packageInfo = PackageInfo.FindForAssetPath(LoaderPackageAssetPath);
            if (packageInfo == null)
            {
                UnityDebug.LogWarning("OXRSys OpenXR loader copy skipped: com.unity.xr.openxr package was not resolved.");
                return;
            }

            string sourcePath = Path.Combine(packageInfo.resolvedPath, LoaderPackageRelativePath);
            if (!File.Exists(sourcePath))
            {
                UnityDebug.LogWarning($"OXRSys OpenXR loader copy skipped: loader not found at {sourcePath}");
                return;
            }

            string destinationPath = Path.Combine(pathToBuiltProject, LoaderBundleRelativePath);
            string destinationDirectory = Path.GetDirectoryName(destinationPath);
            if (!string.IsNullOrEmpty(destinationDirectory))
            {
                Directory.CreateDirectory(destinationDirectory);
            }

            File.Copy(sourcePath, destinationPath, true);
            UnityDebug.Log($"Copied OXRSys OpenXR loader to {destinationPath}");

            AdHocResignApp(pathToBuiltProject);
        }

        private static void AdHocResignApp(string appPath)
        {
#if UNITY_EDITOR_OSX
            const string codesignPath = "/usr/bin/codesign";
            if (!File.Exists(codesignPath))
            {
                UnityDebug.LogWarning("OXRSys OpenXR loader copied, but codesign was not found. The app bundle may need to be signed manually.");
                return;
            }

            ProcessStartInfo startInfo = new ProcessStartInfo
            {
                FileName = codesignPath,
                Arguments = "--force --deep --sign - " + QuoteArgument(appPath),
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };

            using (Process process = Process.Start(startInfo))
            {
                if (process == null)
                {
                    UnityDebug.LogWarning("OXRSys OpenXR loader copied, but codesign could not be started.");
                    return;
                }

                string output = process.StandardOutput.ReadToEnd();
                string error = process.StandardError.ReadToEnd();
                process.WaitForExit();

                if (process.ExitCode != 0)
                {
                    UnityDebug.LogWarning($"OXRSys OpenXR loader copied, but codesign failed with exit code {process.ExitCode}.\n{output}{error}");
                    return;
                }
            }

            UnityDebug.Log("Ad-hoc signed macOS app after OXRSys OpenXR loader copy.");
#else
            UnityDebug.LogWarning("OXRSys OpenXR loader copied, but automatic codesign only runs from the macOS editor.");
#endif
        }

        private static string QuoteArgument(string value)
        {
            return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        }
    }
}
