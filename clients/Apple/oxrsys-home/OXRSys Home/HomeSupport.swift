// SPDX-License-Identifier: MPL-2.0

import AppKit
import Darwin
import Foundation
import UniformTypeIdentifiers

enum EncoderPreset: String, CaseIterable, Identifiable {
    case quality
    case balanced
    case speed

    var id: String { rawValue }
}

enum StreamingTransportSetting: String, CaseIterable, Identifiable {
    case auto
    case wifi
    case usbAdb = "usb_adb"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .auto:
            return "Auto"
        case .wifi:
            return "WiFi"
        case .usbAdb:
            return "USB ADB"
        }
    }
}

enum FoveationPresetSetting: String, CaseIterable, Identifiable {
    case off
    case light
    case medium
    case high

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .off:
            return "Off"
        case .light:
            return "Light"
        case .medium:
            return "Medium"
        case .high:
            return "High"
        }
    }
}

enum ClientFoveationPresetSetting: String, CaseIterable, Identifiable {
    case auto
    case off
    case light
    case medium
    case high

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .auto:
            return "Auto"
        case .off:
            return "Off"
        case .light:
            return "Light"
        case .medium:
            return "Medium"
        case .high:
            return "High"
        }
    }
}

enum ClientReprojectionSetting: String, CaseIterable, Identifiable {
    case off
    case pose
    case poseWarp = "pose_warp"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .off:
            return "Off"
        case .pose:
            return "Pose"
        case .poseWarp:
            return "Pose Warp"
        }
    }
}

enum AbrModeSetting: String, CaseIterable, Identifiable {
    case off
    case bitrate
    case full

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .off:
            return "Off"
        case .bitrate:
            return "Bitrate"
        case .full:
            return "Full"
        }
    }
}

struct HomePaths {
    static let appSupportDirectory = NSString(string: "~/Library/Application Support/OXRSys").expandingTildeInPath
    static let configFilePath = (appSupportDirectory as NSString).appendingPathComponent("oxrsys-runtime.toml")
    static let launcherAppsPath = (appSupportDirectory as NSString).appendingPathComponent("launcher_apps.json")
    static let terminalScriptsDirectory = (appSupportDirectory as NSString).appendingPathComponent("TerminalLaunchers")
    static let activeRuntimeDirectory = NSString(string: "~/.config/openxr/1").expandingTildeInPath
    static let activeRuntimePath = (activeRuntimeDirectory as NSString).appendingPathComponent("active_runtime.json")
    static let runtimeStatusPath = (appSupportDirectory as NSString).appendingPathComponent("runtime_status.json")
    static let runtimeLogsDirectory = appSupportDirectory
    static let launchAgentsDirectory = NSString(string: "~/Library/LaunchAgents").expandingTildeInPath
    static let launchAgentPath = (launchAgentsDirectory as NSString).appendingPathComponent("net.demonixis.oxrsys.runtime-env.plist")
}

struct RuntimeRegistrationStatus {
    var activeRuntimeExists = false
    var activeRuntimeTarget: String?
}

enum RuntimeActivityState: String {
    case idle
    case streaming
}

enum RuntimeActivityTransport: String {
    case wifi
    case usbAdb = "usb_adb"
}

enum RuntimeDeviceType: String {
    case quest
    case pico
    case simulator
    case visionPro = "vision_pro"
    case unknown

    var displayName: String {
        switch self {
        case .quest:
            return "Quest"
        case .pico:
            return "Pico"
        case .simulator:
            return "Simulator"
        case .visionPro:
            return "Vision Pro"
        case .unknown:
            return "Unknown"
        }
    }
}

struct HomeRuntimeStreamingStats: Equatable {
    struct Latency: Equatable {
        var serverPipelineMs: Double = 0
        var clientPipelineMs: Double = 0
        var clientReceiveToSubmitMs: Double = 0
        var clientDecodeMs: Double = 0
        var clientCompositorMs: Double = 0
        var predictionHorizonMs: Double = 0
        var displayedFrameAgeMs: Double = 0
    }

    struct Encode: Equatable {
        var queueAverageMs: Double = 0
        var queueP95Ms: Double = 0
        var gpuAverageMs: Double = 0
        var gpuP95Ms: Double = 0
        var submitAverageMs: Double = 0
        var submitP95Ms: Double = 0
        var callbackAverageMs: Double = 0
        var callbackP95Ms: Double = 0
        var totalAverageMs: Double = 0
        var totalP95Ms: Double = 0
    }

    struct Counters: Equatable {
        var encodedFramesTotal: Int = 0
        var encoderDroppedFramesTotal: Int = 0
        var replacedFramesDelta: Int = 0
        var keyframeRequestsDelta: Int = 0
        var pendingDepthMax: Int = 0
        var reprojectedFramesDelta: Int = 0
        var staleFrameReusesDelta: Int = 0
        var renderPoseFallbacksDelta: Int = 0
    }

    var sampleUnixMilliseconds: Int64 = 0
    var refreshRateHz: Int = 0
    var currentBitrateMbps: Int = 0
    var maxBitrateMbps: Int = 0
    var renderWidth: Int = 0
    var renderHeight: Int = 0
    var encodedWidth: Int = 0
    var encodedHeight: Int = 0
    var encoderPreset: String = ""
    var foveatedEncodingPreset: String = ""
    var clientFoveationPreset: String = ""
    var clientUpscaling = false
    var clientReprojectionMode: String = ""
    var abrMode: String = ""
    var abrState: String = ""
    var abrProfile: String = ""
    var headsetAudio = false
    var latency = Latency()
    var encode = Encode()
    var counters = Counters()

    static func parse(_ value: Any?) -> HomeRuntimeStreamingStats? {
        guard let object = value as? [String: Any] else {
            return nil
        }

        let latencyObject = object["latency_ms"] as? [String: Any] ?? [:]
        let encodeObject = object["encode_ms"] as? [String: Any] ?? [:]
        let countersObject = object["counters"] as? [String: Any] ?? [:]

        return HomeRuntimeStreamingStats(
            sampleUnixMilliseconds: int64Value(object["sample_unix_ms"]) ?? 0,
            refreshRateHz: intValue(object["refresh_rate_hz"]) ?? 0,
            currentBitrateMbps: intValue(object["current_bitrate_mbps"]) ?? 0,
            maxBitrateMbps: intValue(object["max_bitrate_mbps"]) ?? 0,
            renderWidth: intValue(object["render_width"]) ?? 0,
            renderHeight: intValue(object["render_height"]) ?? 0,
            encodedWidth: intValue(object["encoded_width"]) ?? 0,
            encodedHeight: intValue(object["encoded_height"]) ?? 0,
            encoderPreset: stringValue(object["encoder_preset"]) ?? "",
            foveatedEncodingPreset: stringValue(object["foveated_encoding_preset"]) ?? "",
            clientFoveationPreset: stringValue(object["client_foveation_preset"]) ?? "",
            clientUpscaling: boolValue(object["client_upscaling"]) ?? false,
            clientReprojectionMode: stringValue(object["client_reprojection_mode"]) ?? "",
            abrMode: stringValue(object["abr_mode"]) ?? "",
            abrState: stringValue(object["abr_state"]) ?? "",
            abrProfile: stringValue(object["abr_profile"]) ?? "",
            headsetAudio: boolValue(object["headset_audio"]) ?? false,
            latency: Latency(
                serverPipelineMs: doubleValue(latencyObject["server_pipeline"]),
                clientPipelineMs: doubleValue(latencyObject["client_pipeline"]),
                clientReceiveToSubmitMs: doubleValue(latencyObject["client_receive_to_submit"]),
                clientDecodeMs: doubleValue(latencyObject["client_decode"]),
                clientCompositorMs: doubleValue(latencyObject["client_compositor"]),
                predictionHorizonMs: doubleValue(latencyObject["prediction_horizon"]),
                displayedFrameAgeMs: doubleValue(latencyObject["displayed_frame_age"])
            ),
            encode: Encode(
                queueAverageMs: doubleValue(encodeObject["queue_avg"]),
                queueP95Ms: doubleValue(encodeObject["queue_p95"]),
                gpuAverageMs: doubleValue(encodeObject["gpu_avg"]),
                gpuP95Ms: doubleValue(encodeObject["gpu_p95"]),
                submitAverageMs: doubleValue(encodeObject["submit_avg"]),
                submitP95Ms: doubleValue(encodeObject["submit_p95"]),
                callbackAverageMs: doubleValue(encodeObject["callback_avg"]),
                callbackP95Ms: doubleValue(encodeObject["callback_p95"]),
                totalAverageMs: doubleValue(encodeObject["total_avg"]),
                totalP95Ms: doubleValue(encodeObject["total_p95"])
            ),
            counters: Counters(
                encodedFramesTotal: intValue(countersObject["encoded_frames_total"]) ?? 0,
                encoderDroppedFramesTotal: intValue(countersObject["encoder_dropped_frames_total"]) ?? 0,
                replacedFramesDelta: intValue(countersObject["replaced_frames_delta"]) ?? 0,
                keyframeRequestsDelta: intValue(countersObject["keyframe_requests_delta"]) ?? 0,
                pendingDepthMax: intValue(countersObject["pending_depth_max"]) ?? 0,
                reprojectedFramesDelta: intValue(countersObject["reprojected_frames_delta"]) ?? 0,
                staleFrameReusesDelta: intValue(countersObject["stale_frame_reuses_delta"]) ?? 0,
                renderPoseFallbacksDelta: intValue(countersObject["render_pose_fallbacks_delta"]) ?? 0
            )
        )
    }

    private static func intValue(_ value: Any?) -> Int? {
        if let value = value as? Int {
            return value
        }
        if let number = value as? NSNumber {
            return number.intValue
        }
        return nil
    }

    private static func stringValue(_ value: Any?) -> String? {
        value as? String
    }

    private static func boolValue(_ value: Any?) -> Bool? {
        if let value = value as? Bool {
            return value
        }
        if let number = value as? NSNumber {
            return number.boolValue
        }
        return nil
    }

    private static func int64Value(_ value: Any?) -> Int64? {
        if let value = value as? Int64 {
            return value
        }
        if let value = value as? Int {
            return Int64(value)
        }
        if let number = value as? NSNumber {
            return number.int64Value
        }
        return nil
    }

    private static func doubleValue(_ value: Any?) -> Double {
        if let value = value as? Double {
            return value
        }
        if let value = value as? Float {
            return Double(value)
        }
        if let value = value as? Int {
            return Double(value)
        }
        if let number = value as? NSNumber {
            return number.doubleValue
        }
        return 0
    }
}

struct HomeRuntimeActivity: Equatable {
    var state: RuntimeActivityState = .idle
    var transport: RuntimeActivityTransport?
    var deviceType: RuntimeDeviceType?
    var clientName: String?
    var applicationName: String?
    var processID: Int?
    var updatedAtUnixMilliseconds: Int64?
    var streamingStats: HomeRuntimeStreamingStats?

    static let idle = HomeRuntimeActivity()

    var stateDisplayName: String {
        switch (state, transport) {
        case (.streaming, .wifi):
            return "Streaming (WiFi)"
        case (.streaming, .usbAdb):
            return "Streaming (USB)"
        case (.streaming, _):
            return "Streaming"
        case (.idle, _):
            return "Idle"
        }
    }

    var deviceDisplayName: String {
        guard state == .streaming else {
            return "None"
        }
        return deviceType?.displayName ?? "Unknown"
    }

    static func read(from path: String = HomePaths.runtimeStatusPath) -> HomeRuntimeActivity {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let status = parse(data, validateProcess: true) else {
            return .idle
        }
        return status
    }

    static func parse(_ data: Data, validateProcess: Bool = false) -> HomeRuntimeActivity? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }

        let processID = intValue(object["process_id"])
        if validateProcess, let processID, !isProcessRunning(processID) {
            return .idle
        }

        let state = stringValue(object["state"])
            .flatMap(RuntimeActivityState.init(rawValue:)) ?? .idle
        let transport = stringValue(object["transport"])
            .flatMap(RuntimeActivityTransport.init(rawValue:))
        let deviceType = stringValue(object["device_type"])
            .flatMap(RuntimeDeviceType.init(rawValue:))

        return HomeRuntimeActivity(
            state: state,
            transport: transport,
            deviceType: deviceType,
            clientName: nonEmptyStringValue(object["client_name"]),
            applicationName: nonEmptyStringValue(object["application_name"]),
            processID: processID,
            updatedAtUnixMilliseconds: int64Value(object["updated_at_unix_ms"]),
            streamingStats: state == .streaming ?
                HomeRuntimeStreamingStats.parse(object["streaming_stats"]) : nil
        )
    }

    private static func stringValue(_ value: Any?) -> String? {
        value as? String
    }

    private static func nonEmptyStringValue(_ value: Any?) -> String? {
        guard let text = stringValue(value), !text.isEmpty else {
            return nil
        }
        return text
    }

    private static func intValue(_ value: Any?) -> Int? {
        if let value = value as? Int {
            return value
        }
        if let number = value as? NSNumber {
            return number.intValue
        }
        return nil
    }

    private static func int64Value(_ value: Any?) -> Int64? {
        if let value = value as? Int64 {
            return value
        }
        if let value = value as? Int {
            return Int64(value)
        }
        if let number = value as? NSNumber {
            return number.int64Value
        }
        return nil
    }

    private static func isProcessRunning(_ processID: Int) -> Bool {
        guard processID > 0 else {
            return false
        }
        if kill(pid_t(processID), 0) == 0 {
            return true
        }
        return errno == EPERM
    }
}

enum HomePrimaryTransport: String, CaseIterable, Identifiable {
    case wifi
    case usbAdb

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .wifi:
            return "WiFi"
        case .usbAdb:
            return "USB"
        }
    }

    var configTransport: StreamingTransportSetting {
        switch self {
        case .wifi:
            return .wifi
        case .usbAdb:
            return .usbAdb
        }
    }
}

struct MacWifiStatus: Equatable {
    var interfaceName: String?
    var isPoweredOn: Bool?
    var message: String

    static let unknown = MacWifiStatus(
        interfaceName: nil,
        isPoweredOn: nil,
        message: "WiFi status unavailable."
    )
}

enum MacWifiBridge {
    static func status() -> MacWifiStatus {
        do {
            let hardwareOutput = try Shell.run("/usr/sbin/networksetup", ["-listallhardwareports"])
            guard let device = wifiDevice(from: hardwareOutput) else {
                return MacWifiStatus(
                    interfaceName: nil,
                    isPoweredOn: nil,
                    message: "No WiFi interface found."
                )
            }

            let powerOutput = try Shell.run("/usr/sbin/networksetup", ["-getairportpower", device])
            guard let poweredOn = parsePowerOutput(powerOutput) else {
                return MacWifiStatus(
                    interfaceName: device,
                    isPoweredOn: nil,
                    message: "WiFi status unavailable for \(device)."
                )
            }

            return MacWifiStatus(
                interfaceName: device,
                isPoweredOn: poweredOn,
                message: poweredOn ? "WiFi is on on \(device)." : "WiFi is off on \(device)."
            )
        } catch {
            return MacWifiStatus(
                interfaceName: nil,
                isPoweredOn: nil,
                message: "WiFi status unavailable: \(error.localizedDescription)"
            )
        }
    }

    static func wifiDevice(from output: String) -> String? {
        var currentPort: String?
        for rawLine in output.split(whereSeparator: { $0.isNewline }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("Hardware Port:") {
                currentPort = String(line.dropFirst("Hardware Port:".count))
                    .trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("Device:"),
                      let currentPort,
                      isWifiPortName(currentPort) {
                return String(line.dropFirst("Device:".count))
                    .trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }

    static func parsePowerOutput(_ output: String) -> Bool? {
        let parts = output.split(separator: ":", maxSplits: 1)
        guard let value = parts.last?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() else {
            return nil
        }
        if value == "on" {
            return true
        }
        if value == "off" {
            return false
        }
        return nil
    }

    private static func isWifiPortName(_ name: String) -> Bool {
        let normalized = name.lowercased()
        return normalized.contains("wi-fi") ||
               normalized.contains("wifi") ||
               normalized.contains("airport")
    }
}

struct HomeTransportReadiness: Equatable {
    var isReady: Bool
    var message: String
    var canConfigureUsb: Bool = false
}

struct HomeAdbStatus: Equatable {
    var executablePath: String?
    var message: String

    var isAvailable: Bool {
        executablePath != nil
    }

    static let unknown = HomeAdbStatus(
        executablePath: nil,
        message: "ADB status unavailable."
    )

    static let missing = HomeAdbStatus(
        executablePath: nil,
        message: "ADB is required for USB mode."
    )

    static func available(at path: String, isCustom: Bool = false) -> HomeAdbStatus {
        HomeAdbStatus(
            executablePath: path,
            message: isCustom ? "Custom ADB found at \(path)." : "ADB found at \(path)."
        )
    }

    static func invalidCustomPath(_ path: String) -> HomeAdbStatus {
        HomeAdbStatus(
            executablePath: nil,
            message: "Custom ADB path is invalid: \(path). Select an executable adb that passes `adb version`, or clear the custom path to auto-detect."
        )
    }
}

enum HomeAdbInstallGuidance {
    static let title = "ADB is required for USB mode"
    static let message = """
    USB streaming uses Android Debug Bridge (adb) to configure the reverse TCP tunnel. Install adb-enhanced, then make sure adb is available in PATH.

    With Homebrew, run:
    brew install adb-enhanced
    """
    static let homebrewURL = URL(string: "https://formulae.brew.sh/formula/adb-enhanced#default")!
}

struct QuestUsbDevice: Identifiable, Equatable {
    let serial: String
    let state: String
    let details: String

    var id: String { serial }
    var isUsable: Bool { state == "device" }
    var displayName: String {
        if details.isEmpty {
            return "\(serial) (\(state))"
        }
        return "\(serial) (\(state)) \(details)"
    }
}

enum QuestUsbBridge {
    static let reversePorts = [9944, 9945, 9946]

    static func devices(customAdbPath: String? = nil) throws -> [QuestUsbDevice] {
        let adb = try adbExecutablePath(customPath: customAdbPath)
        return try parseDevices(Shell.run(adb, ["devices", "-l"]))
    }

    @discardableResult
    static func configureReverse(for serial: String, customAdbPath: String? = nil) throws -> Set<Int> {
        let adb = try adbExecutablePath(customPath: customAdbPath)
        for port in reversePorts {
            _ = try? Shell.run(adb, ["-s", serial, "reverse", "--remove", "tcp:\(port)"])
        }
        for port in reversePorts {
            _ = try Shell.run(adb, ["-s", serial, "reverse", "tcp:\(port)", "tcp:\(port)"])
        }
        let configuredPorts = try reverseMappings(for: serial, customAdbPath: customAdbPath)
        let missingPorts = reversePorts.filter { !configuredPorts.contains($0) }
        if !missingPorts.isEmpty {
            throw QuestUsbBridgeError.missingReversePorts(missingPorts)
        }
        return configuredPorts
    }

    static func reverseMappings(for serial: String, customAdbPath: String? = nil) throws -> Set<Int> {
        let adb = try adbExecutablePath(customPath: customAdbPath)
        return parseReversePorts(try Shell.run(adb, ["-s", serial, "reverse", "--list"]))
    }

    static func adbExecutablePath(customPath: String? = nil) throws -> String {
        if let custom = cleanedCustomAdbPath(customPath),
           !validateAdbExecutable(at: custom) {
            throw QuestUsbBridgeError.invalidCustomAdb(custom)
        }
        if let path = resolveAdbExecutablePath(customPath: customPath) {
            return path
        }
        throw QuestUsbBridgeError.adbNotFound
    }

    static func status(customPath: String? = nil) -> HomeAdbStatus {
        if let custom = cleanedCustomAdbPath(customPath) {
            if validateAdbExecutable(at: custom) {
                return .available(at: custom, isCustom: true)
            }
            return .invalidCustomPath(custom)
        }

        if let path = resolveAdbExecutablePath() {
            return .available(at: path)
        }
        return .missing
    }

    static func resolveAdbExecutablePath(
        customPath: String? = nil,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        homeDirectory: String = NSHomeDirectory(),
        includeShellLookup: Bool = true
    ) -> String? {
        let fileManager = FileManager.default
        if let custom = cleanedCustomAdbPath(customPath) {
            return validateAdbExecutable(at: custom) ? custom : nil
        }

        for candidate in adbCandidatePaths(environment: environment, homeDirectory: homeDirectory) {
            if fileManager.isExecutableFile(atPath: candidate) {
                return candidate
            }
        }

        if includeShellLookup,
           let shellPath = try? Shell.run("/bin/zsh", ["-lc", "command -v adb"]),
           !shellPath.isEmpty,
           fileManager.isExecutableFile(atPath: shellPath) {
            return shellPath
        }

        return nil
    }

    static func adbCandidatePaths(
        customPath: String? = nil,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        homeDirectory: String = NSHomeDirectory()
    ) -> [String] {
        var paths: [String] = []
        func append(_ path: String?) {
            guard let path, !path.isEmpty, !paths.contains(path) else { return }
            paths.append(path)
        }

        append(cleanedCustomAdbPath(customPath))
        append(environment["ANDROID_HOME"].map { "\($0)/platform-tools/adb" })
        append(environment["ANDROID_SDK_ROOT"].map { "\($0)/platform-tools/adb" })
        append("\(homeDirectory)/Library/Android/sdk/platform-tools/adb")
        append("/opt/homebrew/bin/adb")
        append("/usr/local/bin/adb")

        if let pathEnvironment = environment["PATH"] {
            for directory in pathEnvironment.split(separator: ":") {
                append("\(directory)/adb")
            }
        }

        return paths
    }

    static func validateAdbExecutable(at path: String) -> Bool {
        FileManager.default.isExecutableFile(atPath: path) &&
            ((try? Shell.run(path, ["version"])) != nil)
    }

    private static func cleanedCustomAdbPath(_ path: String?) -> String? {
        guard let path else { return nil }
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        return (trimmed as NSString).expandingTildeInPath
    }

    static func parseDevices(_ output: String) -> [QuestUsbDevice] {
        output
            .split(whereSeparator: { $0.isNewline })
            .dropFirst()
            .compactMap { line -> QuestUsbDevice? in
                let parts = line.split(maxSplits: 2, omittingEmptySubsequences: true) {
                    $0 == " " || $0 == "\t"
                }
                guard parts.count >= 2 else {
                    return nil
                }
                let serial = String(parts[0])
                let state = String(parts[1])
                let details = parts.count >= 3 ? String(parts[2]) : ""
                return QuestUsbDevice(serial: serial, state: state, details: details)
            }
    }

    static func parseReversePorts(_ output: String) -> Set<Int> {
        var ports = Set<Int>()
        for line in output.split(whereSeparator: { $0.isNewline }) {
            let text = String(line)
            for port in reversePorts where text.contains("tcp:\(port) tcp:\(port)") {
                ports.insert(port)
            }
        }
        return ports
    }
}

enum QuestUsbBridgeError: LocalizedError {
    case adbNotFound
    case invalidCustomAdb(String)
    case missingReversePorts([Int])

    var errorDescription: String? {
        switch self {
        case .adbNotFound:
            return "adb was not found from the OXRSys Home. Install Android Platform Tools or make sure adb exists at /opt/homebrew/bin/adb, /usr/local/bin/adb, or ~/Library/Android/sdk/platform-tools/adb."
        case let .invalidCustomAdb(path):
            return "Custom adb path is invalid: \(path). Select an executable adb that passes `adb version`, or clear the custom path to auto-detect."
        case let .missingReversePorts(ports):
            let portList = ports.map(String.init).joined(separator: ", ")
            return "adb reverse did not report the expected mapping for port(s): \(portList)."
        }
    }
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
        var repoRoot = URL(fileURLWithPath: sourceFilePath).deletingLastPathComponent()
        let fileManager = FileManager.default
        while repoRoot.path != repoRoot.deletingLastPathComponent().path {
            let cmakePath = repoRoot.appendingPathComponent("CMakeLists.txt").path
            let runtimePath = repoRoot.appendingPathComponent("runtime").path
            if fileManager.fileExists(atPath: cmakePath) &&
                fileManager.fileExists(atPath: runtimePath) {
                break
            }
            repoRoot.deleteLastPathComponent()
        }
        return repoRoot.appendingPathComponent("build/runtime/oxrsys-runtime.json").path
    }
}

func revealInFinder(_ path: String) {
    NSWorkspace.shared.selectFile(path, inFileViewerRootedAtPath: "")
}

func openFolderInFinder(_ path: String) {
    NSWorkspace.shared.open(URL(fileURLWithPath: path, isDirectory: true))
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

func chooseExecutableFile(prompt: String, startingAt path: String?) -> String? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.allowsMultipleSelection = false
    panel.prompt = prompt
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
