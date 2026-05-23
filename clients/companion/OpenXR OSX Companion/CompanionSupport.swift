// SPDX-License-Identifier: MPL-2.0

import AppKit
import Foundation
import UniformTypeIdentifiers

enum EncoderPreset: String, CaseIterable, Identifiable {
    case quality
    case balanced
    case speed

    var id: String { rawValue }
}

struct CompanionPaths {
    static let appSupportDirectory = NSString(string: "~/Library/Application Support/OpenXR-OSX").expandingTildeInPath
    static let configFilePath = (appSupportDirectory as NSString).appendingPathComponent("openxr_osx.toml")
    static let launcherAppsPath = (appSupportDirectory as NSString).appendingPathComponent("launcher_apps.json")
    static let installedRuntimeDirectory = (appSupportDirectory as NSString).appendingPathComponent("Runtime/current")
    static let installedRuntimeManifestPath = (installedRuntimeDirectory as NSString).appendingPathComponent("openxr_osx.json")
    static let terminalScriptsDirectory = (appSupportDirectory as NSString).appendingPathComponent("TerminalLaunchers")
    static let activeRuntimeDirectory = NSString(string: "~/.config/openxr/1").expandingTildeInPath
    static let activeRuntimePath = (activeRuntimeDirectory as NSString).appendingPathComponent("active_runtime.json")
    static let launchAgentsDirectory = NSString(string: "~/Library/LaunchAgents").expandingTildeInPath
    static let launchAgentPath = (launchAgentsDirectory as NSString).appendingPathComponent("com.openxr_osx.runtime_env.plist")

    static var bundledRuntimeDirectoryURL: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("OpenXRRuntime", isDirectory: true)
    }
}

struct RuntimeRegistrationStatus {
    var activeRuntimeExists = false
    var activeRuntimeTarget: String?
}

enum ShellCommandError: LocalizedError {
    case commandFailed(command: String, stderr: String, exitCode: Int32)

    var errorDescription: String? {
        switch self {
        case let .commandFailed(command, stderr, exitCode):
            let detail = stderr.isEmpty ? "command failed" : stderr
            return "\(command) exited with code \(exitCode): \(detail)"
        }
    }
}

enum Shell {
    @discardableResult
    static func run(_ launchPath: String, _ arguments: [String]) throws -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: launchPath)
        process.arguments = arguments

        let outputPipe = Pipe()
        let errorPipe = Pipe()
        process.standardOutput = outputPipe
        process.standardError = errorPipe

        try process.run()
        process.waitUntilExit()

        let output = String(data: outputPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let error = String(data: errorPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        guard process.terminationStatus == 0 else {
            throw ShellCommandError.commandFailed(
                command: ([launchPath] + arguments).joined(separator: " "),
                stderr: error.trimmingCharacters(in: .whitespacesAndNewlines),
                exitCode: process.terminationStatus
            )
        }
        return output.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

enum SourceDefaults {
    static func defaultRuntimeManifestPath(sourceFilePath: String = #filePath) -> String {
        let sourceURL = URL(fileURLWithPath: sourceFilePath)
        let repoRoot = sourceURL
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        return repoRoot.appendingPathComponent("build/runtime/openxr_osx.json").path
    }
}

func revealInFinder(_ path: String) {
    NSWorkspace.shared.selectFile(path, inFileViewerRootedAtPath: "")
}

func chooseJsonFile(startingAt path: String?) -> String? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.allowsMultipleSelection = false
    panel.allowedContentTypes = [.json]
    panel.prompt = "Choose Runtime JSON"
    if let path, !path.isEmpty {
        panel.directoryURL = URL(fileURLWithPath: (path as NSString).deletingLastPathComponent)
        panel.nameFieldStringValue = (path as NSString).lastPathComponent
    }
    return panel.runModal() == .OK ? panel.url?.path : nil
}

func chooseAppBundle(startingAt path: String?) -> URL? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.allowsMultipleSelection = false
    panel.allowedContentTypes = [.applicationBundle]
    panel.prompt = "Add App"
    if let path, !path.isEmpty {
        panel.directoryURL = URL(fileURLWithPath: (path as NSString).deletingLastPathComponent)
        panel.nameFieldStringValue = (path as NSString).lastPathComponent
    } else {
        panel.directoryURL = URL(fileURLWithPath: "/Applications", isDirectory: true)
    }
    return panel.runModal() == .OK ? panel.url : nil
}

func normalizedPath(_ path: String) -> String {
    URL(fileURLWithPath: path).standardizedFileURL.path
}
