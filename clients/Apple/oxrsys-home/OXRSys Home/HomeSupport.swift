// SPDX-License-Identifier: MPL-2.0

import AppKit
import Darwin
import Foundation
import IOKit
import IOKit.usb
import Security
import UniformTypeIdentifiers

enum EncoderPreset: String, CaseIterable, Identifiable {
    case quality
    case balanced
    case speed

    var id: String { rawValue }
}

enum VideoCodecSetting: String, CaseIterable, Identifiable {
    case h265
    case h264
    case auto

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .h265:
            return "H.265"
        case .h264:
            return "H.264"
        case .auto:
            return "Auto"
        }
    }
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

enum OcclusionModeSetting: String, CaseIterable, Identifiable {
    case off
    case sceneMesh = "scene_mesh"
    case environmentDepth = "environment_depth"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .off:
            return "Off"
        case .sceneMesh:
            return "Scene Mesh"
        case .environmentDepth:
            return "Environment Depth"
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
    var videoCodec: String = ""
    var encoderPreset: String = ""
    var foveatedEncodingPreset: String = ""
    var clientFoveationPreset: String = ""
    var clientUpscaling = false
    var clientReprojectionMode: String = ""
    var abrMode: String = ""
    var abrState: String = ""
    var abrProfile: String = ""
    var resolutionScale = 0.0
    var dynamicResolutionMinScale = 0.0
    var streamReconfigure = false
    var streamConfigSequence: Int = 0
    var passthroughEnabled = false
    var passthroughSupported = false
    var passthroughReady = false
    var occlusionMode: String = ""
    var spatialEnabled = false
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
            videoCodec: stringValue(object["video_codec"]) ?? "",
            encoderPreset: stringValue(object["encoder_preset"]) ?? "",
            foveatedEncodingPreset: stringValue(object["foveated_encoding_preset"]) ?? "",
            clientFoveationPreset: stringValue(object["client_foveation_preset"]) ?? "",
            clientUpscaling: boolValue(object["client_upscaling"]) ?? false,
            clientReprojectionMode: stringValue(object["client_reprojection_mode"]) ?? "",
            abrMode: stringValue(object["abr_mode"]) ?? "",
            abrState: stringValue(object["abr_state"]) ?? "",
            abrProfile: stringValue(object["abr_profile"]) ?? "",
            resolutionScale: doubleValue(object["resolution_scale"]),
            dynamicResolutionMinScale: doubleValue(object["dynamic_resolution_min_scale"]),
            streamReconfigure: boolValue(object["stream_reconfigure"]) ?? false,
            streamConfigSequence: intValue(object["stream_config_sequence"]) ?? 0,
            passthroughEnabled: boolValue(object["passthrough_enabled"]) ?? false,
            passthroughSupported: boolValue(object["passthrough_supported"]) ?? false,
            passthroughReady: boolValue(object["passthrough_ready"]) ?? false,
            occlusionMode: stringValue(object["occlusion_mode"]) ?? "",
            spatialEnabled: boolValue(object["spatial_enabled"]) ?? false,
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

struct HomeTransportReadiness: Equatable, Sendable {
    var isReady: Bool
    var message: String
    var canConfigureUsb: Bool = false
}

struct HomeAdbStatus: Equatable, Sendable {
    var executablePath: String?
    var message: String

    nonisolated var isAvailable: Bool {
        executablePath != nil
    }

    nonisolated static var unknown: HomeAdbStatus {
        HomeAdbStatus(
            executablePath: nil,
            message: "ADB status unavailable."
        )
    }

    nonisolated static var missing: HomeAdbStatus {
        HomeAdbStatus(
            executablePath: nil,
            message: "Connect a USB debugging-enabled headset, or configure an external ADB fallback."
        )
    }

    nonisolated static func available(at path: String, isCustom: Bool = false) -> HomeAdbStatus {
        HomeAdbStatus(
            executablePath: path,
            message: isCustom ? "Custom ADB found at \(path)." : "ADB found at \(path)."
        )
    }

    nonisolated static func invalidCustomPath(_ path: String) -> HomeAdbStatus {
        HomeAdbStatus(
            executablePath: nil,
            message: "Custom ADB path is invalid: \(path). Select an executable adb that passes `adb version`, or clear the custom path to auto-detect."
        )
    }

    nonisolated static var nativeServer: HomeAdbStatus {
        HomeAdbStatus(
            executablePath: "adb-server://127.0.0.1:5037",
            message: "ADB server found on 127.0.0.1:5037; no adb executable is required for USB reverse setup."
        )
    }

    nonisolated static var nativeUsb: HomeAdbStatus {
        HomeAdbStatus(
            executablePath: "adb-usb://native",
            message: "Native USB ADB is available; Android SDK tools are not required."
        )
    }
}

enum HomeAdbInstallGuidance {
    static let title = "USB debugging setup is required"
    static let message = """
    USB streaming can configure the ADB reverse tunnel directly through the headset USB connection. Android Studio and the Android SDK are not required.

    Enable developer mode and USB debugging on the headset, connect it over USB, then accept the computer authorization prompt inside the headset.

    External adb remains optional for diagnostics. With Homebrew, run:
    brew install adb-enhanced
    """
    static let homebrewURL = URL(string: "https://formulae.brew.sh/formula/adb-enhanced#default")!
}

struct QuestUsbDevice: Identifiable, Equatable, Sendable {
    let serial: String
    let state: String
    let details: String

    var id: String { serial }
    nonisolated var isUsable: Bool { state == "device" }
    nonisolated var displayName: String {
        if details.isEmpty {
            return "\(serial) (\(state))"
        }
        return "\(serial) (\(state)) \(details)"
    }
}

enum AdbServerProtocolError: LocalizedError {
    case unavailable
    case protocolFailure(String)

    var errorDescription: String? {
        switch self {
        case .unavailable:
            return "ADB server is not available on 127.0.0.1:5037."
        case let .protocolFailure(message):
            return message
        }
    }
}

enum AdbServerBridge {
    nonisolated private static var host: String { "127.0.0.1" }
    nonisolated private static var port: UInt16 { 5037 }
    nonisolated private static var socketTimeoutSeconds: Int { 1 }

    nonisolated static func isAvailable() -> Bool {
        (try? request("host:version", expectsPayload: true)) != nil
    }

    nonisolated static func devices() throws -> [QuestUsbDevice] {
        try QuestUsbBridge.parseDevices(request("host:devices-l", expectsPayload: true))
    }

    nonisolated static func reverseMappings(for serial: String) throws -> Set<Int> {
        try QuestUsbBridge.parseReversePorts(
            request("host-serial:\(serial):reverse:list-forward", expectsPayload: true)
        )
    }

    @discardableResult
    nonisolated static func configureReverse(for serial: String) throws -> Set<Int> {
        for port in QuestUsbBridge.reversePorts {
            _ = try? request(
                "host-serial:\(serial):reverse:killforward:tcp:\(port)",
                expectsPayload: false
            )
        }
        for port in QuestUsbBridge.reversePorts {
            _ = try request(
                "host-serial:\(serial):reverse:forward:tcp:\(port);tcp:\(port)",
                expectsPayload: false
            )
        }

        let configuredPorts = try reverseMappings(for: serial)
        let missingPorts = QuestUsbBridge.reversePorts.filter { !configuredPorts.contains($0) }
        if !missingPorts.isEmpty {
            throw QuestUsbBridgeError.missingReversePorts(missingPorts)
        }
        return configuredPorts
    }

    nonisolated private static func request(_ service: String, expectsPayload: Bool) throws -> String {
        let fileDescriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard fileDescriptor >= 0 else {
            throw AdbServerProtocolError.unavailable
        }
        defer { close(fileDescriptor) }

        var timeout = timeval(tv_sec: socketTimeoutSeconds, tv_usec: 0)
        setsockopt(fileDescriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fileDescriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        var noSigPipe: Int32 = 1
        setsockopt(fileDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, socklen_t(MemoryLayout<Int32>.size))

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        guard inet_pton(AF_INET, host, &address.sin_addr) == 1 else {
            throw AdbServerProtocolError.unavailable
        }

        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                connect(fileDescriptor, socketAddress, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard connected == 0 else {
            throw AdbServerProtocolError.unavailable
        }

        let command = String(format: "%04x%@", service.utf8.count, service)
        try writeAll(Data(command.utf8), to: fileDescriptor)

        let status = try readString(byteCount: 4, from: fileDescriptor)
        if status == "FAIL" {
            let message = (try? readPayload(from: fileDescriptor)) ?? "ADB server rejected \(service)."
            throw AdbServerProtocolError.protocolFailure(message)
        }
        guard status == "OKAY" else {
            throw AdbServerProtocolError.protocolFailure("ADB server returned unexpected status \(status).")
        }

        return expectsPayload ? try readPayload(from: fileDescriptor) : ""
    }

    nonisolated private static func writeAll(_ data: Data, to fileDescriptor: Int32) throws {
        try data.withUnsafeBytes { rawBuffer in
            guard let baseAddress = rawBuffer.bindMemory(to: UInt8.self).baseAddress else {
                return
            }
            var offset = 0
            while offset < data.count {
                let sent = Darwin.send(fileDescriptor, baseAddress.advanced(by: offset), data.count - offset, 0)
                guard sent > 0 else {
                    throw AdbServerProtocolError.unavailable
                }
                offset += sent
            }
        }
    }

    nonisolated private static func readPayload(from fileDescriptor: Int32) throws -> String {
        let lengthText = try readString(byteCount: 4, from: fileDescriptor)
        guard let length = Int(lengthText, radix: 16) else {
            throw AdbServerProtocolError.protocolFailure("ADB server returned an invalid payload length.")
        }
        return try readString(byteCount: length, from: fileDescriptor)
    }

    nonisolated private static func readString(byteCount: Int, from fileDescriptor: Int32) throws -> String {
        guard byteCount > 0 else {
            return ""
        }
        var data = Data(count: byteCount)
        var offset = 0
        while offset < byteCount {
            let readCount = data.withUnsafeMutableBytes { rawBuffer in
                Darwin.recv(
                    fileDescriptor,
                    rawBuffer.bindMemory(to: UInt8.self).baseAddress!.advanced(by: offset),
                    byteCount - offset,
                    0
                )
            }
            guard readCount > 0 else {
                throw AdbServerProtocolError.unavailable
            }
            offset += readCount
        }
        guard let text = String(data: data, encoding: .utf8) else {
            throw AdbServerProtocolError.protocolFailure("ADB server returned non-UTF-8 payload.")
        }
        return text
    }
}

enum AdbUsbProtocolError: LocalizedError {
    case noDevice
    case deviceNotFound(String)
    case authorizationRequired
    case timeout
    case usbFailure(String)
    case protocolFailure(String)
    case keyFailure(String)

    var errorDescription: String? {
        switch self {
        case .noDevice:
            return "No native USB ADB interface was found. Connect a developer-mode headset over USB and enable USB debugging."
        case let .deviceNotFound(serial):
            return "Native USB ADB did not find device \(serial)."
        case .authorizationRequired:
            return "USB debugging authorization is pending. Put on the headset and allow this computer."
        case .timeout:
            return "Native USB ADB timed out waiting for the headset."
        case let .usbFailure(message):
            return message
        case let .protocolFailure(message):
            return message
        case let .keyFailure(message):
            return message
        }
    }
}

enum AdbUsbBridge {
    nonisolated static func isAvailable() -> Bool {
        guard let services = try? matchingServices() else {
            return false
        }
        defer {
            for service in services {
                IOObjectRelease(service)
            }
        }
        return !services.isEmpty
    }

    nonisolated static func devices() throws -> [QuestUsbDevice] {
        let services = try matchingServices()
        guard !services.isEmpty else {
            throw AdbUsbProtocolError.noDevice
        }
        defer {
            for service in services {
                IOObjectRelease(service)
            }
        }

        var devices: [QuestUsbDevice] = []
        var lastError: Error?
        for service in services {
            do {
                let connection = try AdbUsbConnection(service: service)
                do {
                    let banner = try connection.connect()
                    devices.append(
                        QuestUsbDevice(
                            serial: banner.serial ?? connection.locationIdentifier,
                            state: "device",
                            details: banner.details(locationIdentifier: connection.locationIdentifier)
                        )
                    )
                } catch AdbUsbProtocolError.authorizationRequired {
                    devices.append(
                        QuestUsbDevice(
                            serial: connection.locationIdentifier,
                            state: "unauthorized",
                            details: "USB debugging authorization pending"
                        )
                    )
                }
            } catch {
                lastError = error
            }
        }

        if !devices.isEmpty {
            return devices
        }
        throw lastError ?? AdbUsbProtocolError.noDevice
    }

    nonisolated static func reverseMappings(for serial: String) throws -> Set<Int> {
        let (connection, _) = try connectedDevice(matching: serial)
        return QuestUsbBridge.parseReversePorts(try connection.runService("reverse:list-forward"))
    }

    @discardableResult
    nonisolated static func configureReverse(for serial: String) throws -> Set<Int> {
        let (connection, _) = try connectedDevice(matching: serial)
        for port in QuestUsbBridge.reversePorts {
            _ = try? connection.runService("reverse:killforward:tcp:\(port)")
        }
        for port in QuestUsbBridge.reversePorts {
            _ = try connection.runService("reverse:forward:tcp:\(port);tcp:\(port)")
        }

        let configuredPorts = QuestUsbBridge.parseReversePorts(try connection.runService("reverse:list-forward"))
        let missingPorts = QuestUsbBridge.reversePorts.filter { !configuredPorts.contains($0) }
        if !missingPorts.isEmpty {
            throw QuestUsbBridgeError.missingReversePorts(missingPorts)
        }
        return configuredPorts
    }

    nonisolated private static func connectedDevice(
        matching requestedSerial: String
    ) throws -> (AdbUsbConnection, AdbUsbBanner) {
        let services = try matchingServices()
        guard !services.isEmpty else {
            throw AdbUsbProtocolError.noDevice
        }
        defer {
            for service in services {
                IOObjectRelease(service)
            }
        }

        var sawAuthorizationRequired = false
        for service in services {
            do {
                let connection = try AdbUsbConnection(service: service)
                let banner = try connection.connect()
                let serial = banner.serial ?? connection.locationIdentifier
                if serial == requestedSerial || connection.locationIdentifier == requestedSerial {
                    return (connection, banner)
                }
            } catch AdbUsbProtocolError.authorizationRequired {
                sawAuthorizationRequired = true
            } catch {
                continue
            }
        }

        if sawAuthorizationRequired {
            throw AdbUsbProtocolError.authorizationRequired
        }
        throw AdbUsbProtocolError.deviceNotFound(requestedSerial)
    }

    nonisolated private static func matchingServices() throws -> [io_service_t] {
        guard let matching = IOServiceMatching(kIOUSBInterfaceClassName) else {
            throw AdbUsbProtocolError.usbFailure("Unable to create a USB interface matching dictionary.")
        }
        let dictionary = matching as NSMutableDictionary
        dictionary[kUSBInterfaceClass] = NSNumber(value: 0xff)
        dictionary[kUSBInterfaceSubClass] = NSNumber(value: 0x42)
        dictionary[kUSBInterfaceProtocol] = NSNumber(value: 0x01)

        var iterator: io_iterator_t = 0
        let result = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
        guard result == kIOReturnSuccess else {
            throw AdbUsbProtocolError.usbFailure("USB interface lookup failed with IOKit status \(AdbUsbConnection.formatIOReturn(result)).")
        }
        defer {
            IOObjectRelease(iterator)
        }

        var services: [io_service_t] = []
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 {
                break
            }
            services.append(service)
        }
        return services
    }
}

nonisolated struct AdbUsbBanner: Sendable {
    let rawValue: String

    nonisolated var serial: String? {
        property(named: "ro.serialno")
    }

    nonisolated func details(locationIdentifier: String) -> String {
        var parts = ["usb:native", "location:\(locationIdentifier)"]
        if let product = property(named: "ro.product.name") {
            parts.append("product:\(product)")
        }
        if let model = property(named: "ro.product.model") {
            parts.append("model:\(model.replacingOccurrences(of: " ", with: "_"))")
        }
        if let device = property(named: "ro.product.device") {
            parts.append("device:\(device)")
        }
        return parts.joined(separator: " ")
    }

    nonisolated private func property(named name: String) -> String? {
        for rawPart in rawValue.components(separatedBy: ";") {
            let part: String
            if let propertyRange = rawPart.range(of: "ro.") {
                part = String(rawPart[propertyRange.lowerBound...])
            } else {
                part = rawPart
            }
            let keyValue = part.split(separator: "=", maxSplits: 1)
            if keyValue.count == 2, keyValue[0] == name {
                return String(keyValue[1])
            }
        }
        return nil
    }
}

nonisolated final class AdbUsbConnection {
    nonisolated private static let usbInterfaceUserClientTypeID = AdbUsbConnection.uuid(
        0x2d, 0x97, 0x86, 0xc6, 0x9e, 0xf3, 0x11, 0xd4,
        0xad, 0x51, 0x00, 0x0a, 0x27, 0x05, 0x28, 0x61
    )
    nonisolated private static let cfPluginInterfaceID = AdbUsbConnection.uuid(
        0xc2, 0x44, 0xe8, 0x58, 0x10, 0x9c, 0x11, 0xd4,
        0x91, 0xd4, 0x00, 0x50, 0xe4, 0xc6, 0x42, 0x6f
    )
    nonisolated private static let usbInterfaceInterfaceID300 = AdbUsbConnection.uuid(
        0xbc, 0xea, 0xad, 0xdc, 0x88, 0x4d, 0x4f, 0x27,
        0x83, 0x40, 0x36, 0xd6, 0x9f, 0xab, 0x90, 0xf6
    )

    nonisolated private static var connectTimeoutMilliseconds: UInt32 { 1_000 }
    nonisolated private static var authorizationTimeoutSeconds: TimeInterval { 20 }
    nonisolated private static var serviceTimeoutMilliseconds: UInt32 { 5_000 }
    nonisolated private static var maxPayloadBytes: UInt32 { 1_048_576 }

    let locationIdentifier: String

    private let handle: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBInterfaceInterface300>?>
    private let table: UnsafeMutablePointer<IOUSBInterfaceInterface300>
    private let inputPipe: UInt8
    private let outputPipe: UInt8
    private var nextLocalId: UInt32 = 1
    private var isClosed = false
    private var maxDataBytes: UInt32 = 4_096

    init(service: io_service_t) throws {
        var plugin: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>?
        var score: Int32 = 0
        let pluginResult = IOCreatePlugInInterfaceForService(
            service,
            Self.usbInterfaceUserClientTypeID,
            Self.cfPluginInterfaceID,
            &plugin,
            &score
        )
        guard pluginResult == kIOReturnSuccess, let pluginInterface = plugin?.pointee else {
            throw AdbUsbProtocolError.usbFailure("Unable to open the native USB ADB plug-in: \(Self.formatIOReturn(pluginResult)).")
        }
        defer {
            _ = pluginInterface.pointee.Release(pluginInterface)
        }

        var rawInterface: LPVOID?
        let queryResult = pluginInterface.pointee.QueryInterface(
            pluginInterface,
            CFUUIDGetUUIDBytes(Self.usbInterfaceInterfaceID300),
            &rawInterface
        )
        guard queryResult == S_OK,
              let rawInterface else {
            throw AdbUsbProtocolError.usbFailure("Unable to query the native USB ADB interface.")
        }

        let handle = rawInterface.assumingMemoryBound(to: UnsafeMutablePointer<IOUSBInterfaceInterface300>?.self)
        guard let table = handle.pointee else {
            throw AdbUsbProtocolError.usbFailure("Native USB ADB returned an empty interface.")
        }
        self.handle = handle
        self.table = table

        var locationID: UInt32 = 0
        _ = table.pointee.GetLocationID(handle, &locationID)
        self.locationIdentifier = String(format: "usb-%08x", locationID)

        let openResult = table.pointee.USBInterfaceOpen(handle)
        guard openResult == kIOReturnSuccess else {
            _ = table.pointee.Release(handle)
            throw AdbUsbProtocolError.usbFailure("Unable to claim the native USB ADB interface: \(Self.formatIOReturn(openResult)).")
        }

        do {
            let pipes = try Self.findBulkPipes(handle: handle, table: table)
            self.inputPipe = pipes.input
            self.outputPipe = pipes.output
        } catch {
            _ = table.pointee.USBInterfaceClose(handle)
            _ = table.pointee.Release(handle)
            throw error
        }
    }

    deinit {
        close()
    }

    func close() {
        guard !isClosed else {
            return
        }
        isClosed = true
        _ = table.pointee.USBInterfaceClose(handle)
        _ = table.pointee.Release(handle)
    }

    func connect() throws -> AdbUsbBanner {
        try send(
            command: AdbUsbCommand.cnxn,
            arg0: AdbUsbCommand.protocolVersion,
            arg1: maxDataBytes,
            payload: Data("host::\0".utf8)
        )

        var sentSignature = false
        var sentPublicKey = false
        let deadline = Date().addingTimeInterval(Self.authorizationTimeoutSeconds)
        while Date() < deadline {
            do {
                let message = try readMessage(timeoutMilliseconds: Self.connectTimeoutMilliseconds)
                switch message.command {
                case AdbUsbCommand.cnxn:
                    if message.arg1 > 0 {
                        maxDataBytes = message.arg1
                    }
                    let bannerText = String(data: message.payload, encoding: .utf8)?
                        .trimmingCharacters(in: CharacterSet(charactersIn: "\0")) ?? ""
                    return AdbUsbBanner(rawValue: bannerText)
                case AdbUsbCommand.auth where message.arg0 == AdbUsbCommand.authToken:
                    if !sentSignature {
                        try send(
                            command: AdbUsbCommand.auth,
                            arg0: AdbUsbCommand.authSignature,
                            arg1: 0,
                            payload: try AdbHostKeyStore.signature(for: message.payload)
                        )
                        sentSignature = true
                    } else if !sentPublicKey {
                        try send(
                            command: AdbUsbCommand.auth,
                            arg0: AdbUsbCommand.authPublicKey,
                            arg1: 0,
                            payload: try AdbHostKeyStore.publicKeyPayload()
                        )
                        sentPublicKey = true
                    }
                default:
                    break
                }
            } catch AdbUsbProtocolError.timeout {
                continue
            }
        }
        throw AdbUsbProtocolError.authorizationRequired
    }

    func runService(_ service: String) throws -> String {
        let localId = nextLocalId
        nextLocalId &+= 1
        try send(
            command: AdbUsbCommand.open,
            arg0: localId,
            arg1: 0,
            payload: Data("\(service)\0".utf8)
        )

        var remoteId: UInt32?
        var output = Data()
        let deadline = Date().addingTimeInterval(TimeInterval(Self.serviceTimeoutMilliseconds) / 1_000.0)
        while Date() < deadline {
            let message = try readMessage(timeoutMilliseconds: Self.serviceTimeoutMilliseconds)
            switch message.command {
            case AdbUsbCommand.okay where message.arg1 == localId:
                remoteId = message.arg0
            case AdbUsbCommand.wrte where message.arg1 == localId:
                remoteId = message.arg0
                output.append(message.payload)
                try send(command: AdbUsbCommand.okay, arg0: localId, arg1: message.arg0, payload: Data())
            case AdbUsbCommand.clse where message.arg1 == localId || message.arg0 == remoteId:
                if let remoteId {
                    try? send(command: AdbUsbCommand.clse, arg0: localId, arg1: remoteId, payload: Data())
                }
                return try Self.serviceResultText(output, service: service)
            default:
                break
            }
        }
        throw AdbUsbProtocolError.timeout
    }

    private func send(command: UInt32, arg0: UInt32, arg1: UInt32, payload: Data) throws {
        if payload.count > Int(maxDataBytes) {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB payload is larger than the negotiated limit.")
        }
        try write(AdbUsbMessage(command: command, arg0: arg0, arg1: arg1, payload: payload).encoded())
    }

    private func readMessage(timeoutMilliseconds: UInt32) throws -> AdbUsbMessage {
        let header = try readExact(byteCount: AdbUsbMessage.headerSize, timeoutMilliseconds: timeoutMilliseconds)
        let length = header.adbLittleEndianUInt32(at: 12)
        guard length <= Self.maxPayloadBytes else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB returned an oversized payload.")
        }
        let payload = length == 0
            ? Data()
            : try readExact(byteCount: Int(length), timeoutMilliseconds: timeoutMilliseconds)
        return try AdbUsbMessage(header: header, payload: payload)
    }

    private func readExact(byteCount: Int, timeoutMilliseconds: UInt32) throws -> Data {
        var result = Data()
        while result.count < byteCount {
            var buffer = [UInt8](repeating: 0, count: byteCount - result.count)
            var requestedSize = UInt32(buffer.count)
            let ioResult = buffer.withUnsafeMutableBytes { rawBuffer -> IOReturn in
                guard let baseAddress = rawBuffer.baseAddress else {
                    return kIOReturnBadArgument
                }
                return table.pointee.ReadPipeTO(
                    handle,
                    inputPipe,
                    baseAddress,
                    &requestedSize,
                    timeoutMilliseconds,
                    timeoutMilliseconds
                )
            }
            if ioResult == kIOReturnTimeout {
                throw AdbUsbProtocolError.timeout
            }
            guard ioResult == kIOReturnSuccess else {
                throw AdbUsbProtocolError.usbFailure("Native USB ADB read failed: \(Self.formatIOReturn(ioResult)).")
            }
            guard requestedSize > 0 else {
                throw AdbUsbProtocolError.timeout
            }
            result.append(contentsOf: buffer.prefix(Int(requestedSize)))
        }
        return result
    }

    private func write(_ data: Data) throws {
        try data.withUnsafeBytes { rawBuffer in
            guard let baseAddress = rawBuffer.baseAddress else {
                return
            }
            let ioResult = table.pointee.WritePipeTO(
                handle,
                outputPipe,
                UnsafeMutableRawPointer(mutating: baseAddress),
                UInt32(data.count),
                Self.serviceTimeoutMilliseconds,
                Self.serviceTimeoutMilliseconds
            )
            guard ioResult == kIOReturnSuccess else {
                throw AdbUsbProtocolError.usbFailure("Native USB ADB write failed: \(Self.formatIOReturn(ioResult)).")
            }
        }
    }

    nonisolated private static func findBulkPipes(
        handle: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBInterfaceInterface300>?>,
        table: UnsafeMutablePointer<IOUSBInterfaceInterface300>
    ) throws -> (input: UInt8, output: UInt8) {
        var endpointCount: UInt8 = 0
        let endpointResult = table.pointee.GetNumEndpoints(handle, &endpointCount)
        guard endpointResult == kIOReturnSuccess else {
            throw AdbUsbProtocolError.usbFailure("Unable to inspect native USB ADB endpoints: \(formatIOReturn(endpointResult)).")
        }
        guard endpointCount > 0 else {
            throw AdbUsbProtocolError.usbFailure("Native USB ADB did not expose any endpoints.")
        }

        var inputPipe: UInt8?
        var outputPipe: UInt8?
        for pipe in UInt8(1)...endpointCount {
            var direction: UInt8 = 0
            var number: UInt8 = 0
            var transferType: UInt8 = 0
            var maxPacketSize: UInt16 = 0
            var interval: UInt8 = 0
            let pipeResult = table.pointee.GetPipeProperties(
                handle,
                pipe,
                &direction,
                &number,
                &transferType,
                &maxPacketSize,
                &interval
            )
            guard pipeResult == kIOReturnSuccess else {
                continue
            }
            if transferType == UInt8(kUSBBulk), direction == UInt8(kUSBIn) {
                inputPipe = pipe
            } else if transferType == UInt8(kUSBBulk), direction == UInt8(kUSBOut) {
                outputPipe = pipe
            }
        }

        guard let inputPipe, let outputPipe else {
            throw AdbUsbProtocolError.usbFailure("Native USB ADB did not expose bulk input and output endpoints.")
        }
        return (inputPipe, outputPipe)
    }

    nonisolated static func formatIOReturn(_ value: IOReturn) -> String {
        String(format: "0x%08x", UInt32(bitPattern: value))
    }

    nonisolated private static func uuid(
        _ b0: UInt8, _ b1: UInt8, _ b2: UInt8, _ b3: UInt8,
        _ b4: UInt8, _ b5: UInt8, _ b6: UInt8, _ b7: UInt8,
        _ b8: UInt8, _ b9: UInt8, _ b10: UInt8, _ b11: UInt8,
        _ b12: UInt8, _ b13: UInt8, _ b14: UInt8, _ b15: UInt8
    ) -> CFUUID {
        CFUUIDCreateWithBytes(
            nil,
            b0, b1, b2, b3,
            b4, b5, b6, b7,
            b8, b9, b10, b11,
            b12, b13, b14, b15
        )
    }

    nonisolated private static func serviceResultText(_ data: Data, service: String) throws -> String {
        guard let text = String(data: data, encoding: .utf8) else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB service \(service) returned non-UTF-8 output.")
        }
        if text.hasPrefix("FAIL") {
            let message = String(text.dropFirst(4)).trimmingCharacters(in: .whitespacesAndNewlines)
            throw AdbUsbProtocolError.protocolFailure(message.isEmpty ? "Native USB ADB service \(service) failed." : message)
        }
        if text.hasPrefix("OKAY") {
            return String(text.dropFirst(4))
        }
        return text
    }
}

nonisolated enum AdbUsbCommand {
    nonisolated static let sync = AdbUsbCommand.command("SYNC")
    nonisolated static let cnxn = AdbUsbCommand.command("CNXN")
    nonisolated static let auth = AdbUsbCommand.command("AUTH")
    nonisolated static let open = AdbUsbCommand.command("OPEN")
    nonisolated static let okay = AdbUsbCommand.command("OKAY")
    nonisolated static let clse = AdbUsbCommand.command("CLSE")
    nonisolated static let wrte = AdbUsbCommand.command("WRTE")
    nonisolated static let protocolVersion: UInt32 = 0x0100_0000
    nonisolated static let authToken: UInt32 = 1
    nonisolated static let authSignature: UInt32 = 2
    nonisolated static let authPublicKey: UInt32 = 3

    nonisolated private static func command(_ text: String) -> UInt32 {
        var value: UInt32 = 0
        for (index, byte) in text.utf8.enumerated() {
            value |= UInt32(byte) << UInt32(index * 8)
        }
        return value
    }
}

nonisolated struct AdbUsbMessage {
    nonisolated static var headerSize: Int { 24 }

    let command: UInt32
    let arg0: UInt32
    let arg1: UInt32
    let payload: Data

    init(command: UInt32, arg0: UInt32, arg1: UInt32, payload: Data) {
        self.command = command
        self.arg0 = arg0
        self.arg1 = arg1
        self.payload = payload
    }

    init(header: Data, payload: Data) throws {
        guard header.count == Self.headerSize else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB returned a short message header.")
        }
        command = header.adbLittleEndianUInt32(at: 0)
        arg0 = header.adbLittleEndianUInt32(at: 4)
        arg1 = header.adbLittleEndianUInt32(at: 8)
        let length = header.adbLittleEndianUInt32(at: 12)
        let checksum = header.adbLittleEndianUInt32(at: 16)
        let magic = header.adbLittleEndianUInt32(at: 20)

        guard length == payload.count else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB returned an inconsistent payload length.")
        }
        guard checksum == payload.adbChecksum else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB returned a payload checksum mismatch.")
        }
        guard magic == (command ^ 0xffff_ffff) else {
            throw AdbUsbProtocolError.protocolFailure("Native USB ADB returned an invalid message magic.")
        }
        self.payload = payload
    }

    func encoded() -> Data {
        var data = Data()
        data.appendAdbLittleEndian(command)
        data.appendAdbLittleEndian(arg0)
        data.appendAdbLittleEndian(arg1)
        data.appendAdbLittleEndian(UInt32(payload.count))
        data.appendAdbLittleEndian(payload.adbChecksum)
        data.appendAdbLittleEndian(command ^ 0xffff_ffff)
        data.append(payload)
        return data
    }
}

nonisolated enum AdbHostKeyStore {
    nonisolated private static var keyTag: Data {
        Data("net.demonixis.oxrsys.adb-host-key".utf8)
    }

    nonisolated static func signature(for token: Data) throws -> Data {
        let privateKey = try loadOrCreatePrivateKey()
        var error: Unmanaged<CFError>?
        if SecKeyIsAlgorithmSupported(privateKey, .sign, .rsaSignatureDigestPKCS1v15SHA1),
           let signature = SecKeyCreateSignature(
            privateKey,
            .rsaSignatureDigestPKCS1v15SHA1,
            token as CFData,
            &error
           ) as Data? {
            return signature
        }
        let message = error?.takeRetainedValue().localizedDescription ?? "RSA SHA-1 signing is not available."
        throw AdbUsbProtocolError.keyFailure("Unable to sign the USB debugging challenge: \(message)")
    }

    nonisolated static func publicKeyPayload() throws -> Data {
        let privateKey = try loadOrCreatePrivateKey()
        guard let publicKey = SecKeyCopyPublicKey(privateKey) else {
            throw AdbUsbProtocolError.keyFailure("Unable to read the USB debugging public key.")
        }
        var error: Unmanaged<CFError>?
        guard let publicKeyData = SecKeyCopyExternalRepresentation(publicKey, &error) as Data? else {
            let message = error?.takeRetainedValue().localizedDescription ?? "public key export failed"
            throw AdbUsbProtocolError.keyFailure("Unable to export the USB debugging public key: \(message)")
        }
        let hostName = ProcessInfo.processInfo.hostName.replacingOccurrences(of: " ", with: "-")
        return try AdbPublicKeyEncoder.adbPublicKeyPayload(
            derPublicKey: publicKeyData,
            comment: "OXRSys@\(hostName)"
        )
    }

    nonisolated private static func loadOrCreatePrivateKey() throws -> SecKey {
        if let existing = try findPrivateKey() {
            return existing
        }

        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeySizeInBits as String: 2_048,
            kSecPrivateKeyAttrs as String: [
                kSecAttrIsPermanent as String: true,
                kSecAttrApplicationTag as String: keyTag,
            ],
        ]
        var error: Unmanaged<CFError>?
        if let key = SecKeyCreateRandomKey(attributes as CFDictionary, &error) {
            return key
        }

        if let existing = try findPrivateKey() {
            return existing
        }
        let message = error?.takeRetainedValue().localizedDescription ?? "key generation failed"
        throw AdbUsbProtocolError.keyFailure("Unable to create the USB debugging host key: \(message)")
    }

    nonisolated private static func findPrivateKey() throws -> SecKey? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: keyTag,
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecReturnRef as String: true,
        ]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound {
            return nil
        }
        guard status == errSecSuccess else {
            throw AdbUsbProtocolError.keyFailure("Unable to load the USB debugging host key: \(SecCopyErrorMessageString(status, nil) as String? ?? "\(status)")")
        }
        return (result as! SecKey)
    }
}

nonisolated enum AdbPublicKeyEncoder {
    nonisolated static func adbPublicKeyPayload(derPublicKey: Data, comment: String) throws -> Data {
        let parsed = try parseRSAPublicKey(derPublicKey)
        var modulusBytes = parsed.modulus
        while modulusBytes.first == 0, modulusBytes.count > 1 {
            modulusBytes.removeFirst()
        }
        let wordCount = max(1, (modulusBytes.count + 3) / 4)
        let modulus = AdbBigUInt(bigEndianBytes: modulusBytes)
        guard let firstWord = modulus.words.first, firstWord & 1 == 1 else {
            throw AdbUsbProtocolError.keyFailure("USB debugging public key modulus is invalid.")
        }

        var rawKey = Data()
        rawKey.appendAdbLittleEndian(UInt32(wordCount))
        rawKey.appendAdbLittleEndian(UInt32(0) &- modularInverse32(firstWord))
        rawKey.append(modulus.littleEndianWordData(wordCount: wordCount))
        rawKey.append(AdbBigUInt.montgomeryRR(modulus: modulus, wordCount: wordCount).littleEndianWordData(wordCount: wordCount))
        rawKey.appendAdbLittleEndian(parsed.exponent)

        var payload = Data("\(rawKey.base64EncodedString()) \(comment)".utf8)
        payload.append(0)
        return payload
    }

    nonisolated private static func modularInverse32(_ value: UInt32) -> UInt32 {
        var inverse: UInt32 = 1
        for _ in 0..<5 {
            inverse = inverse &* (2 &- value &* inverse)
        }
        return inverse
    }

    nonisolated private static func parseRSAPublicKey(_ data: Data) throws -> (modulus: [UInt8], exponent: UInt32) {
        var reader = DERReader(bytes: [UInt8](data))
        let sequence = try reader.readElement(expectedTag: 0x30)
        var sequenceReader = DERReader(bytes: sequence)
        if sequenceReader.peekTag() == 0x02 {
            return try parsePKCS1PublicKey(sequence)
        }

        _ = try sequenceReader.readElement(expectedTag: 0x30)
        var bitString = try sequenceReader.readElement(expectedTag: 0x03)
        guard !bitString.isEmpty else {
            throw AdbUsbProtocolError.keyFailure("USB debugging public key bit string is empty.")
        }
        bitString.removeFirst()
        return try parsePKCS1PublicKey(bitString)
    }

    nonisolated private static func parsePKCS1PublicKey(_ data: [UInt8]) throws -> (modulus: [UInt8], exponent: UInt32) {
        var reader = DERReader(bytes: data)
        let sequence = try reader.readElement(expectedTag: 0x30)
        var sequenceReader = DERReader(bytes: sequence)
        let modulus = try sequenceReader.readElement(expectedTag: 0x02)
        let exponentBytes = try sequenceReader.readElement(expectedTag: 0x02)
        guard exponentBytes.count <= 4 else {
            throw AdbUsbProtocolError.keyFailure("USB debugging public key exponent is too large.")
        }
        var exponent: UInt32 = 0
        for byte in exponentBytes {
            exponent = (exponent << 8) | UInt32(byte)
        }
        return (modulus, exponent)
    }
}

nonisolated struct DERReader {
    private let bytes: [UInt8]
    private var offset = 0

    init(bytes: [UInt8]) {
        self.bytes = bytes
    }

    mutating func peekTag() -> UInt8? {
        offset < bytes.count ? bytes[offset] : nil
    }

    mutating func readElement(expectedTag: UInt8) throws -> [UInt8] {
        guard offset < bytes.count else {
            throw AdbUsbProtocolError.keyFailure("Unexpected end of USB debugging public key.")
        }
        let tag = bytes[offset]
        offset += 1
        guard tag == expectedTag else {
            throw AdbUsbProtocolError.keyFailure("Unexpected USB debugging public key tag.")
        }
        let length = try readLength()
        guard offset + length <= bytes.count else {
            throw AdbUsbProtocolError.keyFailure("Invalid USB debugging public key length.")
        }
        let value = Array(bytes[offset..<(offset + length)])
        offset += length
        return value
    }

    private mutating func readLength() throws -> Int {
        guard offset < bytes.count else {
            throw AdbUsbProtocolError.keyFailure("Missing USB debugging public key length.")
        }
        let first = bytes[offset]
        offset += 1
        if first & 0x80 == 0 {
            return Int(first)
        }
        let count = Int(first & 0x7f)
        guard count > 0, count <= 4, offset + count <= bytes.count else {
            throw AdbUsbProtocolError.keyFailure("Unsupported USB debugging public key length.")
        }
        var length = 0
        for _ in 0..<count {
            length = (length << 8) | Int(bytes[offset])
            offset += 1
        }
        return length
    }
}

nonisolated struct AdbBigUInt {
    private(set) var words: [UInt32]

    init(bigEndianBytes: [UInt8]) {
        var words: [UInt32] = []
        var current: UInt32 = 0
        var shift = 0
        for byte in bigEndianBytes.reversed() {
            current |= UInt32(byte) << UInt32(shift)
            shift += 8
            if shift == 32 {
                words.append(current)
                current = 0
                shift = 0
            }
        }
        if shift > 0 {
            words.append(current)
        }
        self.words = words
        normalize()
    }

    init(words: [UInt32]) {
        self.words = words
        normalize()
    }

    nonisolated static func montgomeryRR(modulus: AdbBigUInt, wordCount: Int) -> AdbBigUInt {
        var value = AdbBigUInt(words: [1])
        for _ in 0..<(wordCount * 64) {
            value.shiftLeftOneModulo(modulus)
        }
        return value
    }

    func littleEndianWordData(wordCount: Int) -> Data {
        var data = Data()
        for index in 0..<wordCount {
            data.appendAdbLittleEndian(index < words.count ? words[index] : 0)
        }
        return data
    }

    private mutating func shiftLeftOneModulo(_ modulus: AdbBigUInt) {
        var carry: UInt64 = 0
        let targetCount = max(words.count, modulus.words.count)
        if words.count < targetCount {
            words.append(contentsOf: repeatElement(0, count: targetCount - words.count))
        }
        for index in 0..<words.count {
            let value = UInt64(words[index]) * 2 + carry
            words[index] = UInt32(value & 0xffff_ffff)
            carry = value >> 32
        }
        if carry != 0 {
            words.append(UInt32(carry))
        }
        normalize()
        if compare(to: modulus) >= 0 {
            subtract(modulus)
        }
    }

    private mutating func subtract(_ other: AdbBigUInt) {
        var borrow: UInt64 = 0
        for index in 0..<words.count {
            let lhs = UInt64(words[index])
            let rhs = UInt64(index < other.words.count ? other.words[index] : 0) + borrow
            if lhs >= rhs {
                words[index] = UInt32(lhs - rhs)
                borrow = 0
            } else {
                words[index] = UInt32((1 << 32) + lhs - rhs)
                borrow = 1
            }
        }
        normalize()
    }

    private func compare(to other: AdbBigUInt) -> Int {
        if words.count != other.words.count {
            return words.count < other.words.count ? -1 : 1
        }
        for index in stride(from: words.count - 1, through: 0, by: -1) {
            if words[index] != other.words[index] {
                return words[index] < other.words[index] ? -1 : 1
            }
        }
        return 0
    }

    private mutating func normalize() {
        while words.count > 1, words.last == 0 {
            words.removeLast()
        }
        if words.isEmpty {
            words = [0]
        }
    }
}

nonisolated private extension Data {
    var adbChecksum: UInt32 {
        reduce(UInt32(0)) { partial, byte in
            partial &+ UInt32(byte)
        }
    }

    func adbLittleEndianUInt32(at offset: Int) -> UInt32 {
        UInt32(self[offset]) |
            (UInt32(self[offset + 1]) << 8) |
            (UInt32(self[offset + 2]) << 16) |
            (UInt32(self[offset + 3]) << 24)
    }

    mutating func appendAdbLittleEndian(_ value: UInt32) {
        var littleEndianValue = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndianValue) { bytes in
            append(contentsOf: bytes)
        }
    }
}

enum QuestUsbBridge {
    nonisolated static var reversePorts: [Int] { [9944, 9945, 9946, 9948] }

    nonisolated static func devices(customAdbPath: String? = nil) throws -> [QuestUsbDevice] {
        if cleanedCustomAdbPath(customAdbPath) == nil {
            if let devices = try? AdbUsbBridge.devices() {
                return devices
            }
            if let devices = try? AdbServerBridge.devices() {
                return devices
            }
        }
        let adb = try adbExecutablePath(customPath: customAdbPath)
        return try parseDevices(Shell.run(adb, ["devices", "-l"]))
    }

    @discardableResult
    nonisolated static func configureReverse(for serial: String, customAdbPath: String? = nil) throws -> Set<Int> {
        if cleanedCustomAdbPath(customAdbPath) == nil {
            if let configuredPorts = try? AdbUsbBridge.configureReverse(for: serial) {
                return configuredPorts
            }
            if let configuredPorts = try? AdbServerBridge.configureReverse(for: serial) {
                return configuredPorts
            }
        }
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

    nonisolated static func reverseMappings(for serial: String, customAdbPath: String? = nil) throws -> Set<Int> {
        if cleanedCustomAdbPath(customAdbPath) == nil {
            if let ports = try? AdbUsbBridge.reverseMappings(for: serial) {
                return ports
            }
            if let ports = try? AdbServerBridge.reverseMappings(for: serial) {
                return ports
            }
        }
        let adb = try adbExecutablePath(customPath: customAdbPath)
        return parseReversePorts(try Shell.run(adb, ["-s", serial, "reverse", "--list"]))
    }

    nonisolated static func adbExecutablePath(customPath: String? = nil) throws -> String {
        if let custom = cleanedCustomAdbPath(customPath),
           !validateAdbExecutable(at: custom) {
            throw QuestUsbBridgeError.invalidCustomAdb(custom)
        }
        if let path = resolveAdbExecutablePath(customPath: customPath) {
            return path
        }
        throw QuestUsbBridgeError.adbNotFound
    }

    nonisolated static func status(customPath: String? = nil) -> HomeAdbStatus {
        if let custom = cleanedCustomAdbPath(customPath) {
            if validateAdbExecutable(at: custom) {
                return .available(at: custom, isCustom: true)
            }
            return .invalidCustomPath(custom)
        }

        if AdbUsbBridge.isAvailable() {
            return .nativeUsb
        }
        if AdbServerBridge.isAvailable() {
            return .nativeServer
        }
        if let path = resolveAdbExecutablePath() {
            return .available(at: path)
        }
        return .missing
    }

    nonisolated static func resolveAdbExecutablePath(
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

    nonisolated static func adbCandidatePaths(
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

    nonisolated static func validateAdbExecutable(at path: String) -> Bool {
        FileManager.default.isExecutableFile(atPath: path) &&
            ((try? Shell.run(path, ["version"])) != nil)
    }

    nonisolated private static func cleanedCustomAdbPath(_ path: String?) -> String? {
        guard let path else { return nil }
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        return (trimmed as NSString).expandingTildeInPath
    }

    nonisolated static func parseDevices(_ output: String) -> [QuestUsbDevice] {
        let lines = output.split(whereSeparator: { $0.isNewline })
        let deviceLines = lines.first?.contains("List of devices") == true ? lines.dropFirst() : lines.dropFirst(0)
        return deviceLines
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

    nonisolated static func parseReversePorts(_ output: String) -> Set<Int> {
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
            return "No native USB ADB headset was available, no ADB server was available on 127.0.0.1:5037, and adb was not found from OXRSys Home. Connect a developer-mode headset over USB and accept USB debugging, or configure an optional adb fallback."
        case let .invalidCustomAdb(path):
            return "Custom adb path is invalid: \(path). Select an executable adb that passes `adb version`, or clear the custom path to auto-detect."
        case let .missingReversePorts(ports):
            let portList = ports.map(String.init).joined(separator: ", ")
            return "USB reverse setup did not report the expected mapping for port(s): \(portList)."
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
    nonisolated static func run(_ launchPath: String, _ arguments: [String]) throws -> String {
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
    static func defaultRuntimeManifestPath(
        sourceFilePath: String = #filePath,
        bundleURL: URL = Bundle.main.bundleURL
    ) -> String {
        for candidate in runtimeManifestCandidates(sourceFilePath: sourceFilePath, bundleURL: bundleURL) {
            if FileManager.default.fileExists(atPath: candidate) {
                return candidate
            }
        }
        return runtimeManifestCandidates(sourceFilePath: sourceFilePath, bundleURL: bundleURL).first ?? ""
    }

    static func runtimeManifestCandidates(sourceFilePath: String = #filePath, bundleURL: URL = Bundle.main.bundleURL) -> [String] {
        var candidates: [String] = []
        func append(_ path: String) {
            let normalized = normalizedPath(path)
            guard !normalized.isEmpty, !candidates.contains(normalized) else { return }
            candidates.append(normalized)
        }

        let packagedRuntime = bundleURL
            .deletingLastPathComponent()
            .appendingPathComponent("runtime")
            .appendingPathComponent("oxrsys-runtime.json")
            .path
        append(packagedRuntime)

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
        append(repoRoot.appendingPathComponent("build/runtime/oxrsys-runtime.json").path)
        return candidates
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
