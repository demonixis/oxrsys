// SPDX-License-Identifier: MPL-2.0

import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

private struct RuntimeStatsStreamIdentity: Equatable {
    let processID: Int?
    let transport: RuntimeActivityTransport?
    let clientName: String?
}

enum HomeTab: Hashable {
    case apps
    case settings
    case streaming
    case developer
}

private struct UsbTransportSetupResult: Sendable {
    enum Outcome: Sendable {
        case configured
        case needsDeviceSelection
        case failed(message: String, showAdbInstallGuidance: Bool)
    }

    var outcome: Outcome
    var adbStatus: HomeAdbStatus
    var devices: [QuestUsbDevice]
    var selectedSerial: String?
    var reversePorts: Set<Int>
    var statusMessage: String
}

@MainActor
final class HomeAppModel: ObservableObject, @unchecked Sendable {
    @Published var selectedTab: HomeTab = .apps
    @Published var runtimeManifestPath: String
    @Published var serverConfig = OXRSysServerConfig()
    @Published var runtimeStatus = RuntimeRegistrationStatus()
    @Published var launcherApps: [LauncherApp] = []
    @Published var selectedLogAppID: String?
    @Published var isLogPanelVisible = false
    @Published var appLogs: [String: String] = [:]
    @Published var isDropTargeted = false
    @Published var questUsbDevices: [QuestUsbDevice] = []
    @Published var selectedQuestUsbSerial: String?
    @Published var questUsbStatus = "USB ADB transport is not configured."
    @Published var selectedQuestUsbReversePorts: Set<Int> = []
    @Published var wifiStatus = MacWifiStatus.unknown
    @Published var adbStatus = HomeAdbStatus.unknown
    @Published var customAdbPath: String = ""
    @Published var isAdbInstallGuidancePresented = false
    @Published var isRuntimeSetupGuidancePresented = false
    @Published var isUsbSetupInProgress = false
    @Published var runtimeActivity = HomeRuntimeActivity.idle
    @Published private(set) var runtimeStatsHistory: [HomeRuntimeStreamingStats] = []
    @Published private(set) var activeLaunchedAppID: String?
    @Published var statusMessage = ""
    @Published var errorMessage: String?

    private let fileManager = FileManager.default
    private let defaults = UserDefaults.standard
    private let runtimeManifestPathKey = "runtimeManifestPath"
    private let customAdbPathKey = "customAdbPath"
    private let launcherScanner = LauncherAppScanner()
    private let maxLogCharacters = 30_000
    private var launcherStore = LauncherAppsStore()
    private var launchedProcesses: [String: Process] = [:]
    private var launchPipes: [String: [Pipe]] = [:]
    private var configAutosaveTask: Task<Void, Never>?
    private var currentConfigText = OXRSysServerConfig.defaultText
    private var lastKnownConfigModificationDate: Date?
    private var lastTransportHealthRefreshDate = Date.distantPast
    private var mainTransportOverride: HomePrimaryTransport?
    private var pollTask: Task<Void, Never>?
    private var runtimeStatsStreamIdentity: RuntimeStatsStreamIdentity?
    private var hasPresentedRuntimeSetupGuidanceThisLaunch = false
    private let maxRuntimeStatsSamples = 60

    init() {
        runtimeManifestPath = defaults.string(forKey: runtimeManifestPathKey) ?? SourceDefaults.defaultRuntimeManifestPath()
        customAdbPath = defaults.string(forKey: customAdbPathKey) ?? ""
        loadAll()
        startPolling()
    }

    deinit {
        configAutosaveTask?.cancel()
        pollTask?.cancel()
        for process in launchedProcesses.values where process.isRunning {
            process.terminate()
        }
    }

    var configFilePath: String {
        HomePaths.configFilePath
    }

    var activeRuntimePath: String {
        HomePaths.activeRuntimePath
    }

    var activeLaunchRuntimeManifestPath: String {
        normalizedPath(runtimeManifestPath)
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

    var canRegisterSelectedRuntime: Bool {
        fileManager.fileExists(atPath: normalizedPath(runtimeManifestPath))
    }

    var runtimeSetupGuidanceMessage: String {
        if canRegisterSelectedRuntime {
            return """
            OXRSys is not registered as the active OpenXR runtime. Register the selected runtime so compatible apps can find it outside this launcher.
            """
        }
        return """
        OXRSys is not registered as the active OpenXR runtime, and the selected runtime JSON does not exist. Choose the packaged OXRSys runtime JSON, then register it.
        """
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

    var currentProfileAppDisplayName: String {
        if let applicationName = runtimeActivity.applicationName {
            return applicationName
        }
        if let activeLaunchedAppID,
           let app = launcherApps.first(where: { $0.id == activeLaunchedAppID }) {
            return app.name
        }
        return "None"
    }

    var mainTransportSelection: HomePrimaryTransport {
        if let mainTransportOverride {
            return mainTransportOverride
        }
        if runtimeActivity.state == .streaming {
            switch runtimeActivity.transport {
            case .wifi:
                return .wifi
            case .usbAdb:
                return .usbAdb
            case nil:
                break
            }
        }
        switch serverConfig.transport {
        case .wifi:
            return .wifi
        case .usbAdb:
            return .usbAdb
        case .auto:
            return .usbAdb
        }
    }

    var mainTransportReadiness: HomeTransportReadiness {
        switch mainTransportSelection {
        case .wifi:
            return HomeTransportReadiness(
                isReady: wifiStatus.isPoweredOn == true,
                message: wifiStatus.message
            )
        case .usbAdb:
            if isUsbSetupInProgress {
                return HomeTransportReadiness(
                    isReady: false,
                    message: "Configuring USB ADB reverse..."
                )
            }
            guard adbStatus.isAvailable else {
                return HomeTransportReadiness(
                    isReady: false,
                    message: adbStatus.message
                )
            }
            guard let selectedQuestUsbSerial,
                  questUsbDevices.contains(where: { $0.serial == selectedQuestUsbSerial && $0.isUsable }) else {
                return HomeTransportReadiness(
                    isReady: false,
                    message: "No authorized Quest device visible over adb."
                )
            }

            let expectedPorts = Set(QuestUsbBridge.reversePorts)
            if expectedPorts.isSubset(of: selectedQuestUsbReversePorts) {
                let ports = expectedPorts.sorted().map(String.init).joined(separator: ", ")
                return HomeTransportReadiness(
                    isReady: true,
                    message: "USB reverse ready for \(selectedQuestUsbSerial) on \(ports)."
                )
            }

            let missingPorts = expectedPorts.subtracting(selectedQuestUsbReversePorts)
                .sorted()
                .map(String.init)
                .joined(separator: ", ")
            return HomeTransportReadiness(
                isReady: false,
                message: "USB reverse missing port\(missingPorts.contains(",") ? "s" : "") \(missingPorts).",
                canConfigureUsb: true
            )
        }
    }

    var latestRuntimeStats: HomeRuntimeStreamingStats? {
        runtimeStatsHistory.last
    }

    private var effectiveCustomAdbPath: String? {
        let trimmed = customAdbPath.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    func presentRuntimeSetupGuidanceIfNeeded() {
        refreshRuntimeStatus()
        guard !hasPresentedRuntimeSetupGuidanceThisLaunch, !isSelectedRuntimeRegistered else {
            return
        }
        hasPresentedRuntimeSetupGuidanceThisLaunch = true
        selectedTab = .settings
        isRuntimeSetupGuidancePresented = true
    }

    func dismissRuntimeSetupGuidance() {
        isRuntimeSetupGuidancePresented = false
    }

    func registerRuntimeFromGuidance() {
        isRuntimeSetupGuidancePresented = false
        selectedTab = .settings
        registerRuntime()
    }

    func chooseRuntimeManifestFromGuidance() {
        isRuntimeSetupGuidancePresented = false
        selectedTab = .settings
        chooseRuntimeManifest()
        if canRegisterSelectedRuntime {
            registerRuntime()
        }
    }

    func loadAll() {
        loadConfigFromDisk()
        refreshRuntimeStatus()
        refreshRuntimeActivity()
        reloadLauncherApps()
        refreshTransportHealth(force: true)
    }

    func loadConfigFromDisk() {
        do {
            if fileManager.fileExists(atPath: configFilePath) {
                currentConfigText = try String(contentsOfFile: configFilePath, encoding: .utf8)
                lastKnownConfigModificationDate = configModificationDate()
            } else {
                currentConfigText = OXRSysServerConfig.defaultText
                lastKnownConfigModificationDate = nil
            }
            serverConfig = OXRSysServerConfig.parse(from: currentConfigText)
        } catch {
            errorMessage = "Failed to load config: \(error.localizedDescription)"
        }
    }

    func saveStructuredConfig(statusMessage message: String = "Saved runtime configuration.") {
        do {
            let directory = (configFilePath as NSString).deletingLastPathComponent
            try fileManager.createDirectory(atPath: directory, withIntermediateDirectories: true, attributes: nil)
            let text = serverConfig.merged(into: currentConfigText)
            try text.write(toFile: configFilePath, atomically: true, encoding: .utf8)
            currentConfigText = text
            lastKnownConfigModificationDate = configModificationDate()
            statusMessage = message
        } catch {
            errorMessage = "Failed to save config: \(error.localizedDescription)"
        }
    }

    func updateStreamingConfig(_ update: (inout OXRSysServerConfig) -> Void) {
        update(&serverConfig)
        scheduleStructuredConfigAutosave()
    }

    func scheduleStructuredConfigAutosave() {
        configAutosaveTask?.cancel()
        configAutosaveTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(600))
            guard !Task.isCancelled else { return }
            self?.saveStructuredConfig()
        }
    }

    func resetStreamingConfigToDefaults() {
        configAutosaveTask?.cancel()
        serverConfig = OXRSysServerConfig()
        saveStructuredConfig(statusMessage: "Restored default streaming configuration.")
    }

    func revealRuntimeLogsDirectory() {
        do {
            try fileManager.createDirectory(
                atPath: HomePaths.runtimeLogsDirectory,
                withIntermediateDirectories: true,
                attributes: nil
            )
            openFolderInFinder(HomePaths.runtimeLogsDirectory)
        } catch {
            errorMessage = "Failed to reveal runtime logs: \(error.localizedDescription)"
        }
    }

    func revealConfigFile() {
        do {
            if !fileManager.fileExists(atPath: configFilePath) {
                let directory = (configFilePath as NSString).deletingLastPathComponent
                try fileManager.createDirectory(atPath: directory, withIntermediateDirectories: true, attributes: nil)
                let text = serverConfig.merged(into: currentConfigText)
                try text.write(toFile: configFilePath, atomically: true, encoding: .utf8)
                currentConfigText = text
                lastKnownConfigModificationDate = configModificationDate()
                statusMessage = "Created runtime configuration."
            }
            revealInFinder(configFilePath)
        } catch {
            errorMessage = "Failed to reveal config: \(error.localizedDescription)"
        }
    }

    func refreshTransportHealth(force: Bool = false) {
        let now = Date()
        guard force || now.timeIntervalSince(lastTransportHealthRefreshDate) >= 5 else {
            return
        }
        lastTransportHealthRefreshDate = now
        wifiStatus = MacWifiBridge.status()
        if !isUsbSetupInProgress {
            refreshQuestUsbDevices()
        }
    }

    func refreshQuestUsbDevices() {
        guard refreshAdbStatus() else {
            questUsbDevices = []
            selectedQuestUsbSerial = nil
            selectedQuestUsbReversePorts = []
            questUsbStatus = adbStatus.message
            return
        }

        do {
            questUsbDevices = try QuestUsbBridge.devices(customAdbPath: effectiveCustomAdbPath)
            let usableDevices = questUsbDevices.filter(\.isUsable)
            if let selectedQuestUsbSerial,
               !usableDevices.contains(where: { $0.serial == selectedQuestUsbSerial }) {
                self.selectedQuestUsbSerial = usableDevices.count == 1 ? usableDevices.first?.serial : nil
            } else if selectedQuestUsbSerial == nil, usableDevices.count == 1 {
                selectedQuestUsbSerial = usableDevices.first?.serial
            }

            selectedQuestUsbReversePorts = []
            if let selectedQuestUsbSerial {
                selectedQuestUsbReversePorts =
                    (try? QuestUsbBridge.reverseMappings(
                        for: selectedQuestUsbSerial,
                        customAdbPath: effectiveCustomAdbPath
                    )) ?? []
            }

            if questUsbDevices.isEmpty {
                questUsbStatus = "No Quest device reported over USB debugging."
            } else if usableDevices.isEmpty {
                questUsbStatus = "USB debugging sees device(s), but none are authorized for reverse tunneling."
            } else if usableDevices.count > 1, selectedQuestUsbSerial == nil {
                questUsbStatus = "Multiple Quest devices found; select one before configuring USB."
            } else {
                questUsbStatus = "Ready to configure USB ADB reverse for \(usableDevices.count) authorized device\(usableDevices.count == 1 ? "" : "s")."
                if !selectedQuestUsbReversePorts.isEmpty {
                    questUsbStatus += " Active reverse ports: \(selectedQuestUsbReversePorts.sorted().map(String.init).joined(separator: ", "))."
                }
            }
        } catch {
            questUsbDevices = []
            selectedQuestUsbSerial = nil
            selectedQuestUsbReversePorts = []
            questUsbStatus = "adb is unavailable or failed: \(error.localizedDescription)"
        }
    }

    @discardableResult
    private func refreshAdbStatus() -> Bool {
        adbStatus = QuestUsbBridge.status(customPath: effectiveCustomAdbPath)
        return adbStatus.isAvailable
    }

    func configureQuestUsbReverse() {
        startUsbTransportSetup(requireSelectedDevice: true, persistTransportOnSuccess: mainTransportSelection == .usbAdb)
    }

    func setMainTransportSelection(_ selection: HomePrimaryTransport) {
        if selection == .usbAdb {
            mainTransportOverride = .usbAdb
            startUsbTransportSetup(requireSelectedDevice: false, persistTransportOnSuccess: true)
            return
        }

        mainTransportOverride = selection
        serverConfig.transport = selection.configTransport
        saveStructuredConfig()
        refreshTransportHealth(force: true)
    }

    private func startUsbTransportSetup(requireSelectedDevice: Bool, persistTransportOnSuccess: Bool) {
        guard !isUsbSetupInProgress else {
            return
        }

        isUsbSetupInProgress = true
        questUsbStatus = "Configuring USB ADB reverse..."
        statusMessage = "Checking USB ADB transport."

        let customPath = effectiveCustomAdbPath
        let requestedSerial = selectedQuestUsbSerial
        Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Self.collectUsbTransportSetup(
                    customAdbPath: customPath,
                    requestedSerial: requestedSerial,
                    requireSelectedDevice: requireSelectedDevice
                )
            }.value

            guard let self else { return }
            guard self.effectiveCustomAdbPath == customPath else {
                self.isUsbSetupInProgress = false
                return
            }
            self.applyUsbTransportSetupResult(result, persistTransportOnSuccess: persistTransportOnSuccess)
        }
    }

    nonisolated private static func collectUsbTransportSetup(
        customAdbPath: String?,
        requestedSerial: String?,
        requireSelectedDevice: Bool
    ) -> UsbTransportSetupResult {
        let adbStatus = QuestUsbBridge.status(customPath: customAdbPath)
        guard adbStatus.isAvailable else {
            return UsbTransportSetupResult(
                outcome: .failed(
                    message: adbStatus.message,
                    showAdbInstallGuidance: customAdbPath == nil
                ),
                adbStatus: adbStatus,
                devices: [],
                selectedSerial: nil,
                reversePorts: [],
                statusMessage: adbStatus.message
            )
        }

        do {
            let devices = try QuestUsbBridge.devices(customAdbPath: customAdbPath)
            let usableDevices = devices.filter(\.isUsable)
            let selectedSerial: String?
            if let requestedSerial,
               usableDevices.contains(where: { $0.serial == requestedSerial }) {
                selectedSerial = requestedSerial
            } else if !requireSelectedDevice, usableDevices.count == 1 {
                selectedSerial = usableDevices.first?.serial
            } else {
                selectedSerial = nil
            }

            guard let selectedSerial else {
                let message: String
                if devices.isEmpty {
                    message = "No Quest device reported over USB debugging."
                } else if usableDevices.isEmpty {
                    message = "USB debugging sees device(s), but none are authorized for reverse tunneling."
                } else if usableDevices.count > 1 {
                    message = "Multiple Quest devices found; select one before configuring USB."
                } else {
                    message = "Select an authorized Quest device before configuring USB."
                }
                return UsbTransportSetupResult(
                    outcome: .needsDeviceSelection,
                    adbStatus: adbStatus,
                    devices: devices,
                    selectedSerial: nil,
                    reversePorts: [],
                    statusMessage: message
                )
            }

            let existingPorts = try QuestUsbBridge.reverseMappings(
                for: selectedSerial,
                customAdbPath: customAdbPath
            )
            let expectedPorts = Set(QuestUsbBridge.reversePorts)
            let configuredPorts = expectedPorts.isSubset(of: existingPorts)
                ? existingPorts
                : try QuestUsbBridge.configureReverse(
                    for: selectedSerial,
                    customAdbPath: customAdbPath
                )
            let portsText = configuredPorts.sorted().map(String.init).joined(separator: ", ")
            return UsbTransportSetupResult(
                outcome: .configured,
                adbStatus: adbStatus,
                devices: devices,
                selectedSerial: selectedSerial,
                reversePorts: configuredPorts,
                statusMessage: "Verified USB reverse for \(selectedSerial) on ports \(portsText)."
            )
        } catch {
            return UsbTransportSetupResult(
                outcome: .failed(
                    message: "Failed to configure USB reverse: \(error.localizedDescription)",
                    showAdbInstallGuidance: false
                ),
                adbStatus: adbStatus,
                devices: [],
                selectedSerial: requestedSerial,
                reversePorts: [],
                statusMessage: "Failed to configure USB reverse: \(error.localizedDescription)"
            )
        }
    }

    private func applyUsbTransportSetupResult(
        _ result: UsbTransportSetupResult,
        persistTransportOnSuccess: Bool
    ) {
        isUsbSetupInProgress = false
        adbStatus = result.adbStatus
        questUsbDevices = result.devices
        selectedQuestUsbSerial = result.selectedSerial
        selectedQuestUsbReversePorts = result.reversePorts
        questUsbStatus = result.statusMessage

        switch result.outcome {
        case .configured:
            if persistTransportOnSuccess {
                serverConfig.transport = .usbAdb
                saveStructuredConfig(statusMessage: "Selected USB ADB transport.")
            } else {
                statusMessage = "Configured Quest USB ADB transport."
            }
        case .needsDeviceSelection:
            statusMessage = "Select a Quest device for USB ADB transport."
        case let .failed(message, showAdbInstallGuidance):
            if serverConfig.transport != .usbAdb {
                mainTransportOverride = nil
            }
            questUsbStatus = message
            errorMessage = message
            if showAdbInstallGuidance {
                presentAdbInstallGuidance()
            }
        }
    }

    func presentAdbInstallGuidance() {
        isAdbInstallGuidancePresented = true
    }

    func dismissAdbInstallGuidance() {
        isAdbInstallGuidancePresented = false
    }

    func openAdbInstallHelp() {
        NSWorkspace.shared.open(HomeAdbInstallGuidance.homebrewURL)
        dismissAdbInstallGuidance()
    }

    func chooseCustomAdbExecutable() {
        if let selected = chooseExecutableFile(prompt: "Choose ADB", startingAt: customAdbPath) {
            customAdbPath = selected
            defaults.set(selected, forKey: customAdbPathKey)
            statusMessage = "Selected custom ADB executable."
            refreshTransportHealth(force: true)
        }
    }

    func clearCustomAdbPath() {
        customAdbPath = ""
        defaults.removeObject(forKey: customAdbPathKey)
        statusMessage = "ADB will be auto-detected."
        refreshTransportHealth(force: true)
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

    func refreshRuntimeStatus() {
        var status = RuntimeRegistrationStatus()
        status.activeRuntimeExists = activeRuntimeItemExists()
        if status.activeRuntimeExists {
            status.activeRuntimeTarget = destinationOfSymbolicLink(atPath: activeRuntimePath) ?? activeRuntimePath
        }
        runtimeStatus = status
    }

    func refreshRuntimeActivity() {
        let activity = HomeRuntimeActivity.read()
        updateRuntimeStatsHistory(with: activity)
        runtimeActivity = activity
    }

    private func updateRuntimeStatsHistory(with activity: HomeRuntimeActivity) {
        guard activity.state == .streaming else {
            resetRuntimeStatsHistory()
            return
        }

        let identity = RuntimeStatsStreamIdentity(
            processID: activity.processID,
            transport: activity.transport,
            clientName: activity.clientName
        )
        if runtimeStatsStreamIdentity != identity {
            runtimeStatsStreamIdentity = identity
            runtimeStatsHistory.removeAll(keepingCapacity: true)
        }

        guard let stats = activity.streamingStats else {
            return
        }

        if let lastIndex = runtimeStatsHistory.indices.last,
           runtimeStatsHistory[lastIndex].sampleUnixMilliseconds == stats.sampleUnixMilliseconds {
            runtimeStatsHistory[lastIndex] = stats
            return
        }

        runtimeStatsHistory.append(stats)
        if runtimeStatsHistory.count > maxRuntimeStatsSamples {
            runtimeStatsHistory.removeFirst(runtimeStatsHistory.count - maxRuntimeStatsSamples)
        }
    }

    private func resetRuntimeStatsHistory() {
        runtimeStatsStreamIdentity = nil
        runtimeStatsHistory.removeAll(keepingCapacity: true)
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
            _ = try? Shell.run("/bin/launchctl", ["unload", HomePaths.launchAgentPath])
            if fileManager.fileExists(atPath: HomePaths.launchAgentPath) {
                try fileManager.removeItem(atPath: HomePaths.launchAgentPath)
            }

            refreshRuntimeStatus()
            statusMessage = "Unregistered the OpenXR runtime."
        } catch {
            errorMessage = "Failed to unregister runtime: \(error.localizedDescription)"
        }
    }

    func reloadLauncherApps() {
        launcherStore = LauncherAppPersistence.load(from: HomePaths.launcherAppsPath)
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
            try LauncherAppPersistence.save(launcherStore, to: HomePaths.launcherAppsPath)
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
            try LauncherAppPersistence.save(launcherStore, to: HomePaths.launcherAppsPath)
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

            activeLaunchedAppID = appID
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

            let scriptsDirectory = URL(fileURLWithPath: HomePaths.terminalScriptsDirectory, isDirectory: true)
            try fileManager.createDirectory(at: scriptsDirectory, withIntermediateDirectories: true)
            let scriptURL = scriptsDirectory.appendingPathComponent("\(terminalSafeName(app.name)).command")
            let script = TerminalLaunchScriptBuilder.script(app: app, runtimeManifestPath: manifestPath)
            try script.write(to: scriptURL, atomically: true, encoding: .utf8)
            try fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: scriptURL.path)

            guard NSWorkspace.shared.open(scriptURL) else {
                throw NSError(
                    domain: "OXRSysHome",
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
                domain: "OXRSysHome",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "Runtime JSON not found at \(manifestPath)"]
            )
        }

        try fileManager.createDirectory(
            atPath: HomePaths.activeRuntimeDirectory,
            withIntermediateDirectories: true,
            attributes: nil
        )
        if activeRuntimeItemExists() {
            try fileManager.removeItem(atPath: activeRuntimePath)
        }
        try fileManager.createSymbolicLink(atPath: activeRuntimePath, withDestinationPath: manifestPath)

        try fileManager.createDirectory(
            atPath: HomePaths.launchAgentsDirectory,
            withIntermediateDirectories: true,
            attributes: nil
        )
        let launchAgent = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>Label</key>
            <string>net.demonixis.oxrsys.runtime-env</string>
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
        try launchAgent.write(toFile: HomePaths.launchAgentPath, atomically: true, encoding: .utf8)

        _ = try? Shell.run("/bin/launchctl", ["unload", HomePaths.launchAgentPath])
        _ = try Shell.run("/bin/launchctl", ["load", HomePaths.launchAgentPath])
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
        let filteredText = HomeApplicationLogFilter.filtered(text)
        guard !filteredText.isEmpty else {
            return
        }

        let combined = (appLogs[appID] ?? "") + filteredText
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
        if activeLaunchedAppID == appID {
            activeLaunchedAppID = launchedProcesses.first(where: { $0.value.isRunning })?.key
        }
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
                self.refreshRuntimeActivity()
                self.refreshTransportHealth()
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
        return name.isEmpty ? "XR-App" : name
    }
}
