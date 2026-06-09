// SPDX-License-Identifier: MPL-2.0

import AppKit
import Foundation

enum CompatibleAppKind: String, Codable, CaseIterable, Identifiable {
    case godot
    case unity
    case custom

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .godot:
            return "Godot"
        case .unity:
            return "Unity"
        case .custom:
            return "Custom"
        }
    }
}

enum LauncherAppSource: String, Codable {
    case automatic
    case manual

    var displayName: String {
        switch self {
        case .automatic:
            return "Detected"
        case .manual:
            return "Manual"
        }
    }
}

struct LauncherApp: Codable, Equatable, Identifiable {
    var id: String { normalizedPath(bundlePath) }

    let name: String
    let bundleIdentifier: String?
    let bundlePath: String
    let executablePath: String
    let kind: CompatibleAppKind
    var source: LauncherAppSource
    var lastSeen: Date
}

struct LauncherAppsStore: Codable, Equatable {
    var manualApps: [LauncherApp] = []
    var hiddenAutomaticAppPaths: [String] = []
}

enum LauncherAppError: LocalizedError {
    case invalidBundle(String)
    case missingRuntimeManifest(String)

    var errorDescription: String? {
        switch self {
        case let .invalidBundle(path):
            return "No launchable macOS app bundle was found at \(path)"
        case let .missingRuntimeManifest(path):
            return "Runtime JSON not found at \(path)"
        }
    }
}

enum LauncherAppInspector {
    static func inspectApp(at url: URL, source: LauncherAppSource, allowUnknown: Bool) -> LauncherApp? {
        let bundleURL = url.standardizedFileURL
        guard bundleURL.pathExtension.lowercased() == "app" else {
            return nil
        }

        let infoURL = bundleURL.appendingPathComponent("Contents/Info.plist")
        guard let info = loadInfoPlist(at: infoURL),
              let executableName = info["CFBundleExecutable"] as? String,
              !executableName.isEmpty else {
            return nil
        }

        let executableURL = bundleURL
            .appendingPathComponent("Contents/MacOS")
            .appendingPathComponent(executableName)
        guard FileManager.default.fileExists(atPath: executableURL.path) else {
            return nil
        }

        let name = displayName(from: info, fallbackURL: bundleURL)
        let bundleIdentifier = info["CFBundleIdentifier"] as? String
        let kind = knownKind(
            name: name,
            bundleIdentifier: bundleIdentifier,
            bundlePath: bundleURL.path,
            executableName: executableName
        )

        guard allowUnknown || kind != nil else {
            return nil
        }

        return LauncherApp(
            name: name,
            bundleIdentifier: bundleIdentifier,
            bundlePath: bundleURL.path,
            executablePath: executableURL.path,
            kind: kind ?? .custom,
            source: source,
            lastSeen: Date()
        )
    }

    private static func loadInfoPlist(at url: URL) -> [String: Any]? {
        guard let data = try? Data(contentsOf: url),
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let info = plist as? [String: Any] else {
            return nil
        }
        return info
    }

    private static func displayName(from info: [String: Any], fallbackURL: URL) -> String {
        for key in ["CFBundleDisplayName", "CFBundleName", "CFBundleExecutable"] {
            if let value = info[key] as? String, !value.isEmpty {
                return value
            }
        }
        return fallbackURL.deletingPathExtension().lastPathComponent
    }

    private static func knownKind(
        name: String,
        bundleIdentifier: String?,
        bundlePath: String,
        executableName: String
    ) -> CompatibleAppKind? {
        let lowerName = name.lowercased()
        let lowerIdentifier = bundleIdentifier?.lowercased() ?? ""
        let lowerPath = bundlePath.lowercased()
        let lowerExecutable = executableName.lowercased()

        if lowerIdentifier.contains("godotengine.godot") ||
            lowerName.contains("godot") ||
            lowerExecutable.contains("godot") {
            return .godot
        }

        if lowerIdentifier.contains("unity") ||
            lowerName == "unity" ||
            lowerExecutable == "unity" ||
            lowerPath.contains("/unity/hub/editor/") {
            return .unity
        }

        return nil
    }
}

struct LauncherAppScanner {
    var fileManager = FileManager.default

    func scan() -> [LauncherApp] {
        var appsByPath: [String: LauncherApp] = [:]
        for url in candidateURLs() {
            guard let app = LauncherAppInspector.inspectApp(at: url, source: .automatic, allowUnknown: false) else {
                continue
            }
            appsByPath[normalizedPath(app.bundlePath)] = app
        }

        return appsByPath.values.sorted { lhs, rhs in
            if lhs.kind != rhs.kind {
                return lhs.kind.displayName < rhs.kind.displayName
            }
            return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
        }
    }

    private func candidateURLs() -> [URL] {
        var candidates: [URL] = []
        for root in applicationRoots() {
            candidates.append(contentsOf: directAppBundles(in: root))
        }
        candidates.append(contentsOf: unityHubEditorBundles())
        return candidates
    }

    private func applicationRoots() -> [URL] {
        [
            URL(fileURLWithPath: "/Applications", isDirectory: true),
            URL(fileURLWithPath: NSString(string: "~/Applications").expandingTildeInPath, isDirectory: true),
        ]
    }

    private func directAppBundles(in root: URL) -> [URL] {
        guard let children = try? fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }
        return children.filter { $0.pathExtension.lowercased() == "app" }
    }

    private func unityHubEditorBundles() -> [URL] {
        let editorRoot = URL(fileURLWithPath: "/Applications/Unity/Hub/Editor", isDirectory: true)
        guard let versions = try? fileManager.contentsOfDirectory(
            at: editorRoot,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        return versions.map { $0.appendingPathComponent("Unity.app") }
    }
}

enum LauncherAppPersistence {
    static func load(from path: String) -> LauncherAppsStore {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
            return LauncherAppsStore()
        }

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return (try? decoder.decode(LauncherAppsStore.self, from: data)) ?? LauncherAppsStore()
    }

    static func save(_ store: LauncherAppsStore, to path: String) throws {
        let url = URL(fileURLWithPath: path)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(store)
        try data.write(to: url, options: .atomic)
    }

    static func merge(automaticApps: [LauncherApp], store: LauncherAppsStore) -> [LauncherApp] {
        let hiddenPaths = Set(store.hiddenAutomaticAppPaths.map(normalizedPath))
        var appsByPath: [String: LauncherApp] = [:]

        for app in automaticApps where !hiddenPaths.contains(normalizedPath(app.bundlePath)) {
            appsByPath[normalizedPath(app.bundlePath)] = app
        }

        for app in store.manualApps {
            var manualApp = app
            manualApp.source = .manual
            appsByPath[normalizedPath(manualApp.bundlePath)] = manualApp
        }

        return appsByPath.values.sorted { lhs, rhs in
            if lhs.source != rhs.source {
                return lhs.source == .manual
            }
            if lhs.kind != rhs.kind {
                return lhs.kind.displayName < rhs.kind.displayName
            }
            return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
        }
    }
}

enum TerminalLaunchScriptBuilder {
    static func script(app: LauncherApp, runtimeManifestPath: String) -> String {
        let workingDirectory = URL(fileURLWithPath: app.bundlePath).deletingLastPathComponent().path
        return """
        #!/bin/zsh
        export XR_RUNTIME_JSON=\(shellQuoted(runtimeManifestPath))
        cd \(shellQuoted(workingDirectory))
        exec \(shellQuoted(app.executablePath))
        """
    }

    static func shellQuoted(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }
}
