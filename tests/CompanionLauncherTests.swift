// SPDX-License-Identifier: MPL-2.0

import Foundation

struct CompanionLauncherTestFailure: Error, CustomStringConvertible {
    let description: String
}

@main
struct CompanionLauncherTests {
    static func main() throws {
        try testBundleInspection()
        try testLauncherMergeDeduplicatesManualApps()
        try testRuntimeManifestGeneration()
        try testTerminalScriptQuoting()
        print("Companion launcher tests passed")
    }

    private static func testBundleInspection() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }

        let appURL = root.appendingPathComponent("Godot Test.app", isDirectory: true)
        try makeAppBundle(
            at: appURL,
            bundleIdentifier: "org.godotengine.godot",
            bundleName: "Godot Test",
            executableName: "Godot"
        )

        let app = LauncherAppInspector.inspectApp(at: appURL, source: .automatic, allowUnknown: false)
        try expect(app?.kind == .godot, "Expected Godot bundle to be recognized")
        try expect(app?.name == "Godot Test", "Expected bundle display name")
        try expect(app?.executablePath.hasSuffix("Contents/MacOS/Godot") == true, "Expected executable path")
    }

    private static func testLauncherMergeDeduplicatesManualApps() throws {
        let automatic = LauncherApp(
            name: "Godot",
            bundleIdentifier: "org.godotengine.godot",
            bundlePath: "/Applications/Godot.app",
            executablePath: "/Applications/Godot.app/Contents/MacOS/Godot",
            kind: .godot,
            source: .automatic,
            lastSeen: Date()
        )
        let manual = LauncherApp(
            name: "Godot Custom",
            bundleIdentifier: "org.godotengine.godot",
            bundlePath: "/Applications/Godot.app",
            executablePath: "/Applications/Godot.app/Contents/MacOS/Godot",
            kind: .godot,
            source: .manual,
            lastSeen: Date()
        )
        let hidden = LauncherApp(
            name: "Unity",
            bundleIdentifier: "com.unity3d.UnityEditor5.x",
            bundlePath: "/Applications/Unity/Hub/Editor/6000.0.0f1/Unity.app",
            executablePath: "/Applications/Unity/Hub/Editor/6000.0.0f1/Unity.app/Contents/MacOS/Unity",
            kind: .unity,
            source: .automatic,
            lastSeen: Date()
        )
        let store = LauncherAppsStore(
            manualApps: [manual],
            hiddenAutomaticAppPaths: [hidden.bundlePath]
        )

        let merged = LauncherAppPersistence.merge(automaticApps: [automatic, hidden], store: store)
        try expect(merged.count == 1, "Expected hidden auto app to be removed and duplicate to collapse")
        try expect(merged.first?.name == "Godot Custom", "Expected manual app to win over detected app")
        try expect(merged.first?.source == .manual, "Expected merged app to stay manual")
    }

    private static func testRuntimeManifestGeneration() throws {
        let json = CompanionRuntimeInstaller.runtimeManifestJSON(libraryPath: "/tmp/OpenXR Runtime/libopenxr_osx.dylib")
        let data = Data(json.utf8)
        let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        let runtime = object?["runtime"] as? [String: Any]
        try expect(runtime?["library_path"] as? String == "/tmp/OpenXR Runtime/libopenxr_osx.dylib", "Expected absolute library path")
        try expect(object?["file_format_version"] as? String == "1.0.0", "Expected OpenXR manifest format version")
    }

    private static func testTerminalScriptQuoting() throws {
        let app = LauncherApp(
            name: "Quoted App",
            bundleIdentifier: nil,
            bundlePath: "/Applications/Quoted App.app",
            executablePath: "/Applications/Quoted App.app/Contents/MacOS/it'works",
            kind: .custom,
            source: .manual,
            lastSeen: Date()
        )
        let script = TerminalLaunchScriptBuilder.script(
            app: app,
            runtimeManifestPath: "/tmp/Runtime's/openxr_osx.json"
        )
        try expect(script.contains("export XR_RUNTIME_JSON='/tmp/Runtime'\\''s/openxr_osx.json'"), "Expected quoted runtime path")
        try expect(script.contains("exec '/Applications/Quoted App.app/Contents/MacOS/it'\\''works'"), "Expected quoted executable path")
    }

    private static func makeAppBundle(
        at appURL: URL,
        bundleIdentifier: String,
        bundleName: String,
        executableName: String
    ) throws {
        let contentsURL = appURL.appendingPathComponent("Contents", isDirectory: true)
        let macOSURL = contentsURL.appendingPathComponent("MacOS", isDirectory: true)
        try FileManager.default.createDirectory(at: macOSURL, withIntermediateDirectories: true)

        let info: [String: Any] = [
            "CFBundleIdentifier": bundleIdentifier,
            "CFBundleName": bundleName,
            "CFBundleExecutable": executableName,
        ]
        let infoData = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
        try infoData.write(to: contentsURL.appendingPathComponent("Info.plist"))

        let executableURL = macOSURL.appendingPathComponent(executableName)
        try Data("#!/bin/zsh\n".utf8).write(to: executableURL)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executableURL.path)
    }

    private static func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("openxr-companion-tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private static func expect(_ condition: @autoclosure () -> Bool, _ message: String) throws {
        if !condition() {
            throw CompanionLauncherTestFailure(description: message)
        }
    }
}
