// SPDX-License-Identifier: MPL-2.0

import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

@MainActor
final class CompanionAppModel: ObservableObject, @unchecked Sendable {
    @Published var runtimeManifestPath: String
    @Published var serverConfig = OpenXRServerConfig()
    @Published var runtimeStatus = RuntimeRegistrationStatus()
    @Published var runtimeInstallStatus = RuntimeInstallStatus(
        installedManifestPath: CompanionPaths.installedRuntimeManifestPath
    )
    @Published var launcherApps: [LauncherApp] = []
    @Published var selectedLogAppID: String?
    @Published var isLogPanelVisible = false
    @Published var appLogs: [String: String] = [:]
    @Published var isDropTargeted = false
    @Published var statusMessage = ""
    @Published var errorMessage: String?

    private let fileManager = FileManager.default
    private let defaults = UserDefaults.standard
    private let runtimeManifestPathKey = "runtimeManifestPath"
    private let launcherScanner = LauncherAppScanner()
    private let runtimeInstaller = CompanionRuntimeInstaller()
    private let maxLogCharacters = 30_000
    private var launcherStore = LauncherAppsStore()
    private var launchedProcesses: [String: Process] = [:]
    private var launchPipes: [String: [Pipe]] = [:]
    private var currentConfigText = OpenXRServerConfig.defaultText
    private var lastKnownConfigModificationDate: Date?
    private var pollTask: Task<Void, Never>?

    init() {
        runtimeManifestPath = defaults.string(forKey: runtimeManifestPathKey) ?? SourceDefaults.defaultRuntimeManifestPath()
        loadAll()
        if defaults.string(forKey: runtimeManifestPathKey) == nil,
           runtimeInstallStatus.installedRuntimeExists,
           runtimeInstallStatus.installedManifestExists {
            runtimeManifestPath = runtimeInstallStatus.installedManifestPath
            defaults.set(runtimeManifestPath, forKey: runtimeManifestPathKey)
        }
        startPolling()
    }

    deinit {
        pollTask?.cancel()
        for process in launchedProcesses.values where process.isRunning {
            process.terminate()
        }
    }

    var configFilePath: String {
        CompanionPaths.configFilePath
    }

    var activeRuntimePath: String {
        CompanionPaths.activeRuntimePath
    }

    var activeLaunchRuntimeManifestPath: String {
        if runtimeInstallStatus.installedRuntimeExists,
           runtimeInstallStatus.installedManifestExists {
            return runtimeInstallStatus.installedManifestPath
        }
        if fileManager.fileExists(atPath: runtimeManifestPath) {
            return runtimeManifestPath
        }
        return SourceDefaults.defaultRuntimeManifestPath()
    }

    var isRuntimeRegistered: Bool {
        runtimeStatus.activeRuntimeExists
    }

    var isSelectedRuntimeRegistered: Bool {
        guard let target = runtimeStatus.activeRuntimeTarget else {
            return false
        }
        return normalizedPath(target) == normalizedPath(runtimeManifestPath)
    }

    var registrationButtonTitle: String {
        if isSelectedRuntimeRegistered {
            return "Disable OpenXR Registration"
        }
        if isRuntimeRegistered {
            return "Update OpenXR Registration"
        }
        return "Enable OpenXR Registration"
    }

    var runtimeInstallButtonTitle: String {
        if !runtimeInstallStatus.bundledRuntimeExists {
            return "No Bundled Runtime"
        }
        if !runtimeInstallStatus.installedRuntimeExists {
            return "Install and Register Runtime"
        }
        if runtimeInstallStatus.installedRuntimeNeedsUpdate {
            return "Update and Register Runtime"
        }
        return "Reinstall and Register Runtime"
    }

    var canInstallBundledRuntime: Bool {
        runtimeInstallStatus.bundledRuntimeExists
    }

    func loadAll() {
        loadConfigFromDisk()
        refreshRuntimeStatus()
        refreshRuntimeInstallStatus()
        reloadLauncherApps()
    }

    func loadConfigFromDisk() {
        do {
            if fileManager.fileExists(atPath: configFilePath) {
                currentConfigText = try String(contentsOfFile: configFilePath, encoding: .utf8)
                lastKnownConfigModificationDate = configModificationDate()
            } else {
                currentConfigText = OpenXRServerConfig.defaultText
                lastKnownConfigModificationDate = nil
            }
            serverConfig = OpenXRServerConfig.parse(from: currentConfigText)
        } catch {
            errorMessage = "Failed to load config: \(error.localizedDescription)"
        }
    }

    func saveStructuredConfig() {
        do {
            let directory = (configFilePath as NSString).deletingLastPathComponent
            try fileManager.createDirectory(atPath: directory, withIntermediateDirectories: true, attributes: nil)
            let text = serverConfig.merged(into: currentConfigText)
            try text.write(toFile: configFilePath, atomically: true, encoding: .utf8)
            currentConfigText = text
            lastKnownConfigModificationDate = configModificationDate()
            statusMessage = "Saved runtime configuration."
        } catch {
            errorMessage = "Failed to save config: \(error.localizedDescription)"
        }
    }

    func resetToDisk() {
        loadConfigFromDisk()
        statusMessage = "Reloaded configuration from disk."
    }

    func chooseRuntimeManifest() {
        if let selected = chooseJsonFile(startingAt: runtimeManifestPath) {
            runtimeManifestPath = selected
            defaults.set(selected, forKey: runtimeManifestPathKey)
            statusMessage = "Updated runtime manifest path."
        }
    }

    func useInstalledRuntimeManifest() {
        runtimeManifestPath = runtimeInstallStatus.installedManifestPath
        defaults.set(runtimeManifestPath, forKey: runtimeManifestPathKey)
        statusMessage = "Selected the installed runtime manifest."
    }

    func refreshRuntimeStatus() {
        var status = RuntimeRegistrationStatus()
        status.activeRuntimeExists = activeRuntimeItemExists()
        if status.activeRuntimeExists {
            status.activeRuntimeTarget = destinationOfSymbolicLink(atPath: activeRuntimePath) ?? activeRuntimePath
        }
        runtimeStatus = status
    }

    func refreshRuntimeInstallStatus() {
        runtimeInstallStatus = runtimeInstaller.status()
    }

    func installBundledRuntimeAndRegister() {
        do {
            let manifestPath = try runtimeInstaller.installBundledRuntime()
            runtimeManifestPath = manifestPath
            defaults.set(manifestPath, forKey: runtimeManifestPathKey)
            loadConfigFromDisk()
            refreshRuntimeInstallStatus()
            try registerRuntimeManifest(manifestPath)
            refreshRuntimeStatus()
            statusMessage = "Installed and registered the bundled OpenXR runtime."
        } catch {
            errorMessage = "Failed to install runtime: \(error.localizedDescription)"
        }
    }

    func toggleRuntimeRegistration() {
        if isSelectedRuntimeRegistered {
            unregisterRuntime()
        } else {
            registerRuntime()
        }
    }

    func registerRuntime() {
        defaults.set(runtimeManifestPath, forKey: runtimeManifestPathKey)

        do {
            let wasRuntimeRegistered = activeRuntimeItemExists()
            try registerRuntimeManifest(runtimeManifestPath)
            refreshRuntimeStatus()
            statusMessage = wasRuntimeRegistered ? "Updated the OpenXR runtime registration." : "Registered the OpenXR runtime."
        } catch {
            errorMessage = "Failed to register runtime: \(error.localizedDescription)"
        }
    }

    func unregisterRuntime() {
        do {
            if activeRuntimeItemExists() {
                try fileManager.removeItem(atPath: activeRuntimePath)
            }
            _ = try? Shell.run("/bin/launchctl", ["unsetenv", "XR_RUNTIME_JSON"])
            _ = try? Shell.run("/bin/launchctl", ["unload", CompanionPaths.launchAgentPath])
            if fileManager.fileExists(atPath: CompanionPaths.launchAgentPath) {
                try fileManager.removeItem(atPath: CompanionPaths.launchAgentPath)
            }

            refreshRuntimeStatus()
            statusMessage = "Unregistered the OpenXR runtime."
        } catch {
            errorMessage = "Failed to unregister runtime: \(error.localizedDescription)"
        }
    }

    func reloadLauncherApps() {
        launcherStore = LauncherAppPersistence.load(from: CompanionPaths.launcherAppsPath)
        let automaticApps = launcherScanner.scan()
        launcherApps = LauncherAppPersistence.merge(automaticApps: automaticApps, store: launcherStore)

        if let selectedLogAppID,
           !launcherApps.contains(where: { $0.id == selectedLogAppID }) {
            self.selectedLogAppID = nil
            isLogPanelVisible = false
        }
    }

    func chooseLauncherApp() {
        if let url = chooseAppBundle(startingAt: launcherApps.first?.bundlePath) {
            addLauncherApp(at: url)
        }
    }

    func handleAppDrop(_ providers: [NSItemProvider]) -> Bool {
        var accepted = false
        for provider in providers where provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            accepted = true
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { [weak self] item, _ in
                guard let url = Self.url(fromDroppedItem: item) else {
                    return
                }
                Task { @MainActor [weak self] in
                    self?.addLauncherApp(at: url)
                }
            }
        }
        return accepted
    }

    func addLauncherApp(at url: URL) {
        guard var app = LauncherAppInspector.inspectApp(at: url, source: .manual, allowUnknown: true) else {
            errorMessage = LauncherAppError.invalidBundle(url.path).localizedDescription
            return
        }

        app.source = .manual
        let appPath = normalizedPath(app.bundlePath)
        launcherStore.manualApps.removeAll { normalizedPath($0.bundlePath) == appPath }
        launcherStore.manualApps.append(app)
        launcherStore.hiddenAutomaticAppPaths.removeAll { normalizedPath($0) == appPath }

        do {
            try LauncherAppPersistence.save(launcherStore, to: CompanionPaths.launcherAppsPath)
            reloadLauncherApps()
            statusMessage = "Added \(app.name) to the launcher."
        } catch {
            errorMessage = "Failed to save launcher app: \(error.localizedDescription)"
        }
    }

    func removeLauncherApp(_ app: LauncherApp) {
        stopApp(app)
        let appPath = normalizedPath(app.bundlePath)

        if app.source == .manual {
            launcherStore.manualApps.removeAll { normalizedPath($0.bundlePath) == appPath }
        } else if !launcherStore.hiddenAutomaticAppPaths.map(normalizedPath).contains(appPath) {
            launcherStore.hiddenAutomaticAppPaths.append(app.bundlePath)
        }

        do {
            try LauncherAppPersistence.save(launcherStore, to: CompanionPaths.launcherAppsPath)
            reloadLauncherApps()
            statusMessage = "Removed \(app.name) from the launcher."
        } catch {
            errorMessage = "Failed to update launcher apps: \(error.localizedDescription)"
        }
    }

    func isAppRunning(_ app: LauncherApp) -> Bool {
        launchedProcesses[app.id]?.isRunning == true
    }

    func launchApp(_ app: LauncherApp) {
        if isAppRunning(app) {
            return
        }

        do {
            let manifestPath = try runtimeManifestForLaunch()
            guard fileManager.fileExists(atPath: app.executablePath) else {
                throw LauncherAppError.invalidBundle(app.bundlePath)
            }

            let appID = app.id
            let outputPipe = Pipe()
            let errorPipe = Pipe()
            let process = Process()
            process.executableURL = URL(fileURLWithPath: app.executablePath)
            process.currentDirectoryURL = URL(fileURLWithPath: app.bundlePath).deletingLastPathComponent()
            var environment = ProcessInfo.processInfo.environment
            environment["XR_RUNTIME_JSON"] = manifestPath
            process.environment = environment
            process.standardOutput = outputPipe
            process.standardError = errorPipe

            outputPipe.fileHandleForReading.readabilityHandler = logHandler(for: appID)
            errorPipe.fileHandleForReading.readabilityHandler = logHandler(for: appID)
            process.terminationHandler = { [weak self] process in
                Task { @MainActor [weak self] in
                    self?.finishLaunchedApp(appID: appID, status: process.terminationStatus)
                }
            }

            launchedProcesses[appID] = process
            launchPipes[appID] = [outputPipe, errorPipe]
            appendLog("Launching \(app.name)\nXR_RUNTIME_JSON=\(manifestPath)\n\n", for: appID)

            do {
                try process.run()
            } catch {
                cleanupLaunchState(for: appID)
                throw error
            }

            statusMessage = "Launched \(app.name)."
        } catch {
            errorMessage = "Failed to launch \(app.name): \(error.localizedDescription)"
        }
    }

    func stopApp(_ app: LauncherApp) {
        guard let process = launchedProcesses[app.id], process.isRunning else {
            return
        }
        process.terminate()
        statusMessage = "Stopping \(app.name)."
    }

    func runAppInTerminal(_ app: LauncherApp) {
        do {
            let manifestPath = try runtimeManifestForLaunch()
            guard fileManager.fileExists(atPath: app.executablePath) else {
                throw LauncherAppError.invalidBundle(app.bundlePath)
            }

            let scriptsDirectory = URL(fileURLWithPath: CompanionPaths.terminalScriptsDirectory, isDirectory: true)
            try fileManager.createDirectory(at: scriptsDirectory, withIntermediateDirectories: true)
            let scriptURL = scriptsDirectory.appendingPathComponent("\(terminalSafeName(app.name)).command")
            let script = TerminalLaunchScriptBuilder.script(app: app, runtimeManifestPath: manifestPath)
            try script.write(to: scriptURL, atomically: true, encoding: .utf8)
            try fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: scriptURL.path)

            guard NSWorkspace.shared.open(scriptURL) else {
                throw NSError(
                    domain: "OpenXROSXCompanion",
                    code: 3,
                    userInfo: [NSLocalizedDescriptionKey: "Terminal did not open \(scriptURL.path)"]
                )
            }
            appendLog("Opened Terminal launcher at \(scriptURL.path)\n", for: app.id)
            statusMessage = "Opened \(app.name) in Terminal."
        } catch {
            errorMessage = "Failed to open \(app.name) in Terminal: \(error.localizedDescription)"
        }
    }

    func clearLog(for app: LauncherApp) {
        appLogs[app.id] = ""
    }

    func showLogs(for app: LauncherApp) {
        selectedLogAppID = app.id
        isLogPanelVisible = true
    }

    func setLogPanelVisible(_ isVisible: Bool) {
        isLogPanelVisible = isVisible
        if isVisible, selectedLogAppID == nil {
            selectedLogAppID = launcherApps.first?.id
        }
    }

    private func registerRuntimeManifest(_ path: String) throws {
        let manifestPath = normalizedPath(path)
        guard fileManager.fileExists(atPath: manifestPath) else {
            throw NSError(
                domain: "OpenXROSXCompanion",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "Runtime JSON not found at \(manifestPath)"]
            )
        }

        try fileManager.createDirectory(
            atPath: CompanionPaths.activeRuntimeDirectory,
            withIntermediateDirectories: true,
            attributes: nil
        )
        if activeRuntimeItemExists() {
            try fileManager.removeItem(atPath: activeRuntimePath)
        }
        try fileManager.createSymbolicLink(atPath: activeRuntimePath, withDestinationPath: manifestPath)

        try fileManager.createDirectory(
            atPath: CompanionPaths.launchAgentsDirectory,
            withIntermediateDirectories: true,
            attributes: nil
        )
        let launchAgent = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>Label</key>
            <string>com.openxr_osx.runtime_env</string>
            <key>ProgramArguments</key>
            <array>
                <string>/bin/launchctl</string>
                <string>setenv</string>
                <string>XR_RUNTIME_JSON</string>
                <string>\(manifestPath)</string>
            </array>
            <key>RunAtLoad</key>
            <true/>
        </dict>
        </plist>
        """
        try launchAgent.write(toFile: CompanionPaths.launchAgentPath, atomically: true, encoding: .utf8)

        _ = try? Shell.run("/bin/launchctl", ["unload", CompanionPaths.launchAgentPath])
        _ = try Shell.run("/bin/launchctl", ["load", CompanionPaths.launchAgentPath])
        _ = try Shell.run("/bin/launchctl", ["setenv", "XR_RUNTIME_JSON", manifestPath])
    }

    private func runtimeManifestForLaunch() throws -> String {
        let path = activeLaunchRuntimeManifestPath
        guard fileManager.fileExists(atPath: path) else {
            throw LauncherAppError.missingRuntimeManifest(path)
        }
        return normalizedPath(path)
    }

    private nonisolated func logHandler(for appID: String) -> @Sendable (FileHandle) -> Void {
        { @Sendable [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty,
                  let text = String(data: data, encoding: .utf8) else {
                return
            }
            Task { @MainActor [weak self] in
                self?.appendLog(text, for: appID)
            }
        }
    }

    private func appendLog(_ text: String, for appID: String) {
        let combined = (appLogs[appID] ?? "") + text
        if combined.count > maxLogCharacters {
            appLogs[appID] = String(combined.suffix(maxLogCharacters))
        } else {
            appLogs[appID] = combined
        }
    }

    private func finishLaunchedApp(appID: String, status: Int32) {
        cleanupLaunchState(for: appID)
        appendLog("\nProcess exited with code \(status).\n", for: appID)
    }

    private func cleanupLaunchState(for appID: String) {
        if let pipes = launchPipes[appID] {
            for pipe in pipes {
                pipe.fileHandleForReading.readabilityHandler = nil
            }
        }
        launchPipes[appID] = nil
        launchedProcesses[appID] = nil
    }

    private func destinationOfSymbolicLink(atPath path: String) -> String? {
        guard let destination = try? fileManager.destinationOfSymbolicLink(atPath: path) else {
            return nil
        }
        if destination.hasPrefix("/") {
            return destination
        }
        let baseURL = URL(fileURLWithPath: path).deletingLastPathComponent()
        return baseURL.appendingPathComponent(destination).path
    }

    private func activeRuntimeItemExists() -> Bool {
        fileManager.fileExists(atPath: activeRuntimePath) || destinationOfSymbolicLink(atPath: activeRuntimePath) != nil
    }

    private func configModificationDate() -> Date? {
        let attributes = try? fileManager.attributesOfItem(atPath: configFilePath)
        return attributes?[.modificationDate] as? Date
    }

    private func startPolling() {
        pollTask = Task { [weak self] in
            while let self, !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                guard !Task.isCancelled else { break }
                self.refreshRuntimeStatus()
                self.pollConfigChangesIfNeeded()
            }
        }
    }

    private func pollConfigChangesIfNeeded() {
        let currentDate = configModificationDate()
        guard currentDate != lastKnownConfigModificationDate else {
            return
        }
        loadConfigFromDisk()
    }

    private nonisolated static func url(fromDroppedItem item: Any?) -> URL? {
        if let url = item as? URL {
            return url
        }
        if let data = item as? Data {
            return URL(dataRepresentation: data, relativeTo: nil)
        }
        if let string = item as? String {
            return URL(string: string)
        }
        return nil
    }

    private func terminalSafeName(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let scalars = value.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" }
        let name = String(scalars).trimmingCharacters(in: CharacterSet(charactersIn: "_"))
        return name.isEmpty ? "OpenXR-App" : name
    }
}
