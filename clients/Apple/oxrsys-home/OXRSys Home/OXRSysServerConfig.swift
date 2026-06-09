// SPDX-License-Identifier: MPL-2.0

import Foundation

struct OXRSysServerConfig: Equatable {
    static let minBitrateMbps = 1
    static let maxBitrateMbps = 200

    var runtimeEnabled = true
    var bitrateMbps = 50
    var fovDegrees = 100
    var resolutionScale = 0.75
    var keyframeIntervalSec = 2
    var encoderPreset: EncoderPreset = .balanced
    var transport: StreamingTransportSetting = .auto
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

    # Rendering FOV in degrees (symmetric). Must match Quest client's kMacHalfFov.
    fov_degrees = 100

    # Resolution multiplier (0.25 to 1.0). Lower = faster encode, less bandwidth, more blur.
    # 0.5 = half resolution (recommended for WiFi), 1.0 = native resolution.
    resolution_scale = 0.75

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
        if let value = intValue("fov_degrees", in: text), (60...150).contains(value) {
            config.fovDegrees = value
        }
        if let value = doubleValue("resolution_scale", in: text), value >= 0.25, value <= 1.0 {
            config.resolutionScale = value
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

        let sectionValues: [(name: String, keys: [(key: String, value: String)])] = [
            ("general", [
                ("runtime_enabled", boolString(runtimeEnabled)),
            ]),
            ("streaming", [
                ("bitrate_mbps", "\(bitrateMbps)"),
                ("fov_degrees", "\(fovDegrees)"),
                ("resolution_scale", decimalString(resolutionScale)),
                ("keyframe_interval_sec", "\(keyframeIntervalSec)"),
                ("encoder_preset", "\"\(encoderPreset.rawValue)\""),
                ("transport", "\"\(transport.rawValue)\""),
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
