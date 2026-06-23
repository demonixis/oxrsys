// SPDX-License-Identifier: MPL-2.0

import Foundation

struct OXRSysServerConfig: Equatable {
    static let minBitrateMbps = 1
    static let maxBitrateMbps = 200
    static let supportedRefreshRates = [60, 72, 80, 90, 120]

    var runtimeEnabled = true
    var bitrateMbps = 50
    var refreshRateHz = 72
    var resolutionScale = 0.75
    var dynamicResolutionMinScale = 0.50
    var keyframeIntervalSec = 2
    var encoderPreset: EncoderPreset = .balanced
    var transport: StreamingTransportSetting = .auto
    var foveatedEncodingPreset: FoveationPresetSetting = .off
    var clientFoveationPreset: ClientFoveationPresetSetting = .auto
    var clientUpscaling = false
    var clientReprojection: ClientReprojectionSetting = .pose
    var abrMode: AbrModeSetting = .bitrate
    var passthroughEnabled = false
    var occlusionMode: OcclusionModeSetting = .off
    var headsetAudio = false
    var spatialEnabled = false
    var spatialAnchors = false
    var spatialScene = false
    var spatialPersistence = false
    var fileLogging = true
    var questLogcat = false

    static let defaultText = """
    # OXRSys Runtime Configuration
    # Preferred location: ~/Library/Application Support/OXRSys/oxrsys-runtime.toml

    [general]
    # Enable or disable this runtime without unregistering the manifest.
    # When false, xrCreateInstance returns XR_ERROR_RUNTIME_UNAVAILABLE.
    runtime_enabled = true

    [streaming]
    # H.265 encoding bitrate in Mbps. Lower = less latency but more artifacts.
    # Try 20-30 for lower latency, 50+ for quality.
    bitrate_mbps = 50

    # Preferred headset display refresh rate: 60, 72, 80, 90, or 120.
    refresh_rate_hz = 72

    # Resolution multiplier (0.25 to 1.0). Lower = faster encode, less bandwidth, more blur.
    # 0.5 = half resolution (recommended for WiFi), 1.0 = native resolution.
    resolution_scale = 0.75

    # Minimum encoded-resolution multiplier used only when abr_mode = "full"
    # and a reliable USB TCP headset client supports live stream reconfiguration.
    dynamic_resolution_min_scale = 0.50

    # Keyframe interval in seconds (1-10). Higher = less bandwidth spikes, slower recovery.
    # Default 2 is a good balance. Use 1 for lossy WiFi, 5+ for USB.
    keyframe_interval_sec = 2

    # Encoder speed preset: "quality", "balanced", "speed"
    # speed  = lowest latency, fastest encode, lower quality
    # balanced = default, good mix
    # quality  = best visual quality, slightly higher latency
    encoder_preset = "balanced"

    # Streaming transport: "auto", "wifi", or "usb_adb".
    transport = "auto"

    # Server-side foveated video encoding preset: "off", "light", "medium", or "high".
    foveated_encoding_preset = "off"

    # Headset client foveation override: "auto", "off", "light", "medium", or "high".
    # "auto" leaves the headset client unmanaged by Home.
    client_foveation_preset = "auto"

    # Enable Quest shader upscaling after video decode.
    client_upscaling = false

    # Quest client reprojection for short decode/network gaps: "off", "pose", or "pose_warp".
    client_reprojection = "pose"

    # Adaptive bitrate mode: "off", "bitrate", or "full".
    # "full" may select live streaming resolution profiles on reliable USB TCP.
    abr_mode = "bitrate"

    # Enable headset passthrough as a runtime feature. Apps still choose opaque
    # or alpha blend through OpenXR environment blend modes.
    passthrough_enabled = false

    # Occlusion mode: "off", "scene_mesh", or "environment_depth".
    occlusion_mode = "off"

    # Reserved for headset speaker audio. The runtime does not advertise audio until
    # a platform capture/playback path is attached.
    headset_audio = false

    [spatial]
    # Spatial features stay disabled unless the headset client and runtime path support them.
    enabled = false
    anchors = false
    scene = false
    persistence = false

    [logging]
    # Write server logs to ~/Library/Application Support/OXRSys/oxrsys-runtime.log.
    file_logging = true

    # Capture Quest/headset logcat to ~/Library/Application Support/OXRSys/oxrsys-headset.log.
    # Requires adb in PATH and a connected device.
    quest_logcat = false
    """

    static func parse(from text: String) -> OXRSysServerConfig {
        var config = OXRSysServerConfig()

        if let value = boolValue("runtime_enabled", in: text) {
            config.runtimeEnabled = value
        }
        if let value = intValue("bitrate_mbps", in: text),
           (Self.minBitrateMbps...Self.maxBitrateMbps).contains(value) {
            config.bitrateMbps = value
        }
        if let value = intValue("refresh_rate_hz", in: text), supportedRefreshRates.contains(value) {
            config.refreshRateHz = value
        }
        if let value = doubleValue("resolution_scale", in: text), value >= 0.25, value <= 1.0 {
            config.resolutionScale = value
        }
        if let value = doubleValue("dynamic_resolution_min_scale", in: text), value >= 0.25, value <= 1.0 {
            config.dynamicResolutionMinScale = value
        }
        if let value = intValue("keyframe_interval_sec", in: text), (1...10).contains(value) {
            config.keyframeIntervalSec = value
        }
        if let value = stringValue("encoder_preset", in: text), let preset = EncoderPreset(rawValue: value) {
            config.encoderPreset = preset
        }
        if let value = stringValue("transport", in: text), let transport = StreamingTransportSetting(rawValue: value) {
            config.transport = transport
        }
        if let value = stringValue("foveated_encoding_preset", in: text), let preset = FoveationPresetSetting(rawValue: value) {
            config.foveatedEncodingPreset = preset
        }
        if let value = stringValue("client_foveation_preset", in: text), let preset = ClientFoveationPresetSetting(rawValue: value) {
            config.clientFoveationPreset = preset
        }
        if let value = boolValue("client_upscaling", in: text) {
            config.clientUpscaling = value
        }
        if let value = stringValue("client_reprojection", in: text), let mode = ClientReprojectionSetting(rawValue: value) {
            config.clientReprojection = mode
        }
        if let value = stringValue("abr_mode", in: text), let mode = AbrModeSetting(rawValue: value) {
            config.abrMode = mode
        }
        config.passthroughEnabled = boolValue("passthrough_enabled", in: text) ??
            (stringValue("mixed_reality_mode", in: text).map { $0 != "off" } ?? config.passthroughEnabled)
        if let value = stringValue("occlusion_mode", in: text), let mode = OcclusionModeSetting(rawValue: value) {
            config.occlusionMode = mode
        }
        if let value = boolValue("headset_audio", in: text) {
            config.headsetAudio = value
        }
        if let value = boolValue("enabled", in: text) {
            config.spatialEnabled = value
        }
        if let value = boolValue("anchors", in: text) {
            config.spatialAnchors = value
        }
        if let value = boolValue("scene", in: text) {
            config.spatialScene = value
        }
        if let value = boolValue("persistence", in: text) {
            config.spatialPersistence = value
        }
        if let value = boolValue("file_logging", in: text) {
            config.fileLogging = value
        }
        if let value = boolValue("quest_logcat", in: text) {
            config.questLogcat = value
        }

        return config
    }

    func merged(into currentText: String) -> String {
        var text = currentText.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty {
            text = Self.defaultText
        }
        text = removingKeys(Set(["fov_degrees"]), fromSection: "streaming", in: text)

        let sectionValues: [(name: String, keys: [(key: String, value: String)])] = [
            ("general", [
                ("runtime_enabled", boolString(runtimeEnabled)),
            ]),
            ("streaming", [
                ("bitrate_mbps", "\(bitrateMbps)"),
                ("refresh_rate_hz", "\(refreshRateHz)"),
                ("resolution_scale", decimalString(resolutionScale)),
                ("dynamic_resolution_min_scale", decimalString(dynamicResolutionMinScale)),
                ("keyframe_interval_sec", "\(keyframeIntervalSec)"),
                ("encoder_preset", "\"\(encoderPreset.rawValue)\""),
                ("transport", "\"\(transport.rawValue)\""),
                ("foveated_encoding_preset", "\"\(foveatedEncodingPreset.rawValue)\""),
                ("client_foveation_preset", "\"\(clientFoveationPreset.rawValue)\""),
                ("client_upscaling", boolString(clientUpscaling)),
                ("client_reprojection", "\"\(clientReprojection.rawValue)\""),
                ("abr_mode", "\"\(abrMode.rawValue)\""),
                ("passthrough_enabled", boolString(passthroughEnabled)),
                ("occlusion_mode", "\"\(occlusionMode.rawValue)\""),
                ("headset_audio", boolString(headsetAudio)),
            ]),
            ("spatial", [
                ("enabled", boolString(spatialEnabled)),
                ("anchors", boolString(spatialAnchors)),
                ("scene", boolString(spatialScene)),
                ("persistence", boolString(spatialPersistence)),
            ]),
            ("logging", [
                ("file_logging", boolString(fileLogging)),
                ("quest_logcat", boolString(questLogcat)),
            ]),
        ]

        for sectionValue in sectionValues {
            text = upsertingSection(sectionValue.name, keys: sectionValue.keys, in: text)
        }

        return text.hasSuffix("\n") ? text : text + "\n"
    }
}

private func boolValue(_ key: String, in text: String) -> Bool? {
    guard let value = rawValue(key, in: text)?.lowercased() else {
        return nil
    }
    if ["true", "1", "yes"].contains(value) {
        return true
    }
    if ["false", "0", "no"].contains(value) {
        return false
    }
    return nil
}

private func intValue(_ key: String, in text: String) -> Int? {
    guard let value = rawValue(key, in: text) else {
        return nil
    }
    return Int(value)
}

private func doubleValue(_ key: String, in text: String) -> Double? {
    guard let value = rawValue(key, in: text) else {
        return nil
    }
    return Double(value)
}

private func stringValue(_ key: String, in text: String) -> String? {
    guard let value = rawValue(key, in: text) else {
        return nil
    }
    if value.hasPrefix("\""), value.hasSuffix("\""), value.count >= 2 {
        return String(value.dropFirst().dropLast())
    }
    return value
}

private func rawValue(_ key: String, in text: String) -> String? {
    let pattern = "(?m)^\\s*\(NSRegularExpression.escapedPattern(for: key))\\s*=\\s*(.+?)\\s*$"
    guard let regex = try? NSRegularExpression(pattern: pattern) else {
        return nil
    }
    let range = NSRange(text.startIndex..<text.endIndex, in: text)
    guard let match = regex.firstMatch(in: text, range: range),
          let valueRange = Range(match.range(at: 1), in: text) else {
        return nil
    }
    return String(text[valueRange]).trimmingCharacters(in: .whitespacesAndNewlines)
}

private func decimalString(_ value: Double) -> String {
    String(format: "%.2f", value)
}

private func boolString(_ value: Bool) -> String {
    value ? "true" : "false"
}

private func upsertingSection(_ sectionName: String, keys: [(key: String, value: String)], in text: String) -> String {
    var lines = text.components(separatedBy: .newlines)
    let sectionHeader = "[\(sectionName)]"

    func nextSectionStart(after index: Int) -> Int {
        for offset in (index + 1)..<lines.count where isSectionHeader(lines[offset]) {
            return offset
        }
        return lines.count
    }

    if let sectionIndex = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces) == sectionHeader }) {
        let sectionEnd = nextSectionStart(after: sectionIndex)
        var insertIndex = sectionEnd

        for item in keys {
            let newLine = "\(item.key) = \(item.value)"
            if let existingIndex = (sectionIndex + 1..<sectionEnd).first(where: { lineIndex in
                matchesKey(item.key, line: lines[lineIndex])
            }) {
                lines[existingIndex] = newLine
            } else {
                lines.insert(newLine, at: insertIndex)
                insertIndex += 1
            }
        }
    } else {
        if !lines.isEmpty, !lines.last!.isEmpty {
            lines.append("")
        }
        lines.append(sectionHeader)
        for item in keys {
            lines.append("\(item.key) = \(item.value)")
        }
    }

    return lines.joined(separator: "\n")
}

private func removingKeys(_ keys: Set<String>, fromSection sectionName: String, in text: String) -> String {
    let lines = text.components(separatedBy: .newlines)
    let sectionHeader = "[\(sectionName)]"
    guard let sectionIndex = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces) == sectionHeader }) else {
        return text
    }

    let sectionEnd = ((sectionIndex + 1)..<lines.count).first(where: { isSectionHeader(lines[$0]) }) ?? lines.count
    var indexesToRemove = Set<Int>()
    for index in (sectionIndex + 1)..<sectionEnd where keys.contains(where: { matchesKey($0, line: lines[index]) }) {
        indexesToRemove.insert(index)
        let previousIndex = index - 1
        if previousIndex > sectionIndex, lines[previousIndex].contains("Rendering FOV") {
            indexesToRemove.insert(previousIndex)
        }
    }

    var filtered: [String] = []
    filtered.reserveCapacity(lines.count)
    for (index, line) in lines.enumerated() {
        if indexesToRemove.contains(index) {
            continue
        }
        filtered.append(line)
    }
    return filtered.joined(separator: "\n")
}

private func isSectionHeader(_ line: String) -> Bool {
    let trimmed = line.trimmingCharacters(in: .whitespaces)
    return trimmed.hasPrefix("[") && trimmed.hasSuffix("]")
}

private func matchesKey(_ key: String, line: String) -> Bool {
    let trimmed = line.trimmingCharacters(in: .whitespaces)
    guard let equalsIndex = trimmed.firstIndex(of: "=") else {
        return false
    }
    let lineKey = trimmed[..<equalsIndex].trimmingCharacters(in: .whitespaces)
    return lineKey == key
}
