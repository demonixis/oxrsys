// SPDX-License-Identifier: MPL-2.0

import Combine
import Darwin
import Foundation
import IOKit
import IOKit.usb

// Everything Home needs for the "AVP USB" tab lives in this file.
//
// AVP USB is a Home-only concept: the entered Vision Pro address is used for a local ping to
// check the USB Ethernet link, the actual headset connection is established over the network.

/// Result of the host-side USB Ethernet reachability test.
/// `isReachable` only means a ping over the USB Ethernet interface succeeded. It does not
/// mean the visionOS app is running or that a runtime session exists.
struct AVPUSBLinkStatus: Equatable, Sendable {
    var isReachable: Bool
    var hostAddress: String
    var interfaceName: String?
    var message: String

    nonisolated static func disconnected(_ hostAddress: String, message: String) -> AVPUSBLinkStatus {
        AVPUSBLinkStatus(
            isReachable: false,
            hostAddress: hostAddress,
            interfaceName: nil,
            message: message
        )
    }
}

enum AVPUSBEthernetLink {
    /// Shown while no direct address is entered. Automatic discovery stays the default;
    /// a direct IP connection is only used when the user enters an address explicitly.
    nonisolated static let emptyAddressMessage =
        "No Vision Pro IP entered. Automatic discovery is used; enter an IPv4 address to connect directly."
    nonisolated static let idleMessage = "Test the Vision Pro USB Ethernet connection."

    nonisolated static func idleMessage(for hostAddress: String) -> String {
        hostAddress.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? emptyAddressMessage : idleMessage
    }

    nonisolated private static let routePath = "/sbin/route"
    nonisolated private static let networksetupPath = "/usr/sbin/networksetup"
    nonisolated private static let pingPath = "/sbin/ping"

    nonisolated static func testConnection(hostAddress: String) -> AVPUSBLinkStatus {
        let hostAddress = hostAddress.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !hostAddress.isEmpty else {
            return .disconnected(hostAddress, message: emptyAddressMessage)
        }
        guard isIPv4Address(hostAddress) else {
            return .disconnected(hostAddress, message: "Enter a valid Vision Pro IPv4 address.")
        }

        do {
            let route = try Shell.run(routePath, ["-n", "get", "-inet", hostAddress])
            guard let interfaceName = interfaceName(fromRoute: route) else {
                return .disconnected(hostAddress, message: "macOS did not report a route to the Vision Pro.")
            }

            let hardwarePorts = try Shell.run(networksetupPath, ["-listallhardwareports"])
            let hardwarePort = hardwarePortName(for: interfaceName, in: hardwarePorts)
            let isUsbAttached = isUsbAttachedInterface(interfaceName)
            if let rejection = usbInterfaceRejection(
                interfaceName: interfaceName,
                hardwarePort: hardwarePort,
                isUsbAttached: isUsbAttached
            ) {
                return .disconnected(hostAddress, message: rejection)
            }

            guard let sourceAddress = ipv4Address(for: interfaceName) else {
                return .disconnected(
                    hostAddress,
                    message: "USB Ethernet interface \(interfaceName) is not active or has no IPv4 address."
                )
            }

            do {
                try Shell.run(pingPath, ["-c", "1", "-W", "1000", "-S", sourceAddress, hostAddress])
            } catch {
                return .disconnected(
                    hostAddress,
                    message: "No ping reply from \(hostAddress) over \(interfaceName). \(error.localizedDescription)"
                )
            }

            let portLabel = hardwarePort ?? "USB Ethernet"
            return AVPUSBLinkStatus(
                isReachable: true,
                hostAddress: hostAddress,
                interfaceName: interfaceName,
                message: "USB link to Vision Pro reachable over \(portLabel) (\(interfaceName))."
            )
        } catch {
            return .disconnected(hostAddress, message: "USB Ethernet test failed: \(error.localizedDescription)")
        }
    }

    nonisolated static func interfaceName(fromRoute output: String) -> String? {
        output.split(whereSeparator: \.isNewline).compactMap { line in
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2, parts[0].trimmingCharacters(in: .whitespaces) == "interface" else {
                return nil
            }
            return parts[1].trimmingCharacters(in: .whitespaces)
        }.first
    }

    nonisolated static func hardwarePortName(for interfaceName: String, in output: String) -> String? {
        var currentPort: String?
        for rawLine in output.split(whereSeparator: \.isNewline) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("Hardware Port:") {
                currentPort = String(line.dropFirst("Hardware Port:".count))
                    .trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("Device:"),
                      String(line.dropFirst("Device:".count)).trimmingCharacters(in: .whitespaces) == interfaceName {
                return currentPort
            }
        }
        return nil
    }

    /// Returns a rejection message unless the route's interface is positively identified as a
    /// USB Ethernet interface: either its hardware port is a USB/Vision Pro port, or the IOKit
    /// registry shows it is attached to a USB device. Loopback and interfaces that are neither
    /// (utun*, bridge*, Wi-Fi, ...) are rejected. A missing hardware-port entry alone is not
    /// enough to accept or reject: freshly attached USB NICs are often not listed there yet.
    nonisolated static func usbInterfaceRejection(
        interfaceName: String,
        hardwarePort: String?,
        isUsbAttached: Bool
    ) -> String? {
        if interfaceName.hasPrefix("lo") {
            return "Route to the Vision Pro uses the loopback interface \(interfaceName); no headset is connected over USB."
        }
        if let hardwarePort, isUsbEthernetPort(hardwarePort) {
            return nil
        }
        if isUsbAttached {
            return nil
        }
        if let hardwarePort {
            return "Route to the Vision Pro uses \(interfaceName) (\(hardwarePort)), not a detected USB Ethernet port."
        }
        return "Route to the Vision Pro uses \(interfaceName), which could not be identified as a USB Ethernet interface."
    }

    /// True when the BSD interface is an IOEthernetInterface with a USB device in its IOKit parent chain.
    nonisolated static func isUsbAttachedInterface(_ interfaceName: String) -> Bool {
        guard let matching = IOServiceMatching("IOEthernetInterface") else {
            return false
        }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return false
        }
        defer { IOObjectRelease(iterator) }

        var service = IOIteratorNext(iterator)
        while service != 0 {
            let bsdName = IORegistryEntryCreateCFProperty(
                service, "BSD Name" as CFString, kCFAllocatorDefault, 0
            )?.takeRetainedValue() as? String
            if bsdName == interfaceName {
                let result = hasUsbAncestor(service)
                IOObjectRelease(service)
                return result
            }
            IOObjectRelease(service)
            service = IOIteratorNext(iterator)
        }
        return false
    }

    private nonisolated static func hasUsbAncestor(_ entry: io_registry_entry_t) -> Bool {
        var current = entry
        IOObjectRetain(current)
        while true {
            if IOObjectConformsTo(current, "IOUSBHostDevice") != 0 ||
                IOObjectConformsTo(current, "IOUSBDevice") != 0 {
                IOObjectRelease(current)
                return true
            }
            var parent: io_registry_entry_t = 0
            let status = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent)
            IOObjectRelease(current)
            guard status == KERN_SUCCESS else {
                return false
            }
            current = parent
        }
    }

    nonisolated static func isUsbEthernetPort(_ hardwarePortName: String) -> Bool {
        let name = hardwarePortName.lowercased()
        return name.contains("usb") || name.contains("vision pro")
    }

    private nonisolated static func isIPv4Address(_ address: String) -> Bool {
        var parsedAddress = in_addr()
        return address.withCString { inet_pton(AF_INET, $0, &parsedAddress) } == 1
    }

    private nonisolated static func ipv4Address(for interfaceName: String) -> String? {
        var interfaces: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&interfaces) == 0 else {
            return nil
        }
        defer { freeifaddrs(interfaces) }

        var current = interfaces
        while let entry = current {
            defer { current = entry.pointee.ifa_next }
            guard String(cString: entry.pointee.ifa_name) == interfaceName,
                  entry.pointee.ifa_flags & UInt32(IFF_UP | IFF_RUNNING) == UInt32(IFF_UP | IFF_RUNNING),
                  let address = entry.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_INET) else {
                continue
            }

            let ipv4Address = UnsafeRawPointer(address).assumingMemoryBound(to: sockaddr_in.self).pointee.sin_addr
            var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            guard inet_ntop(AF_INET, [ipv4Address], &buffer, socklen_t(buffer.count)) != nil else {
                return nil
            }
            return String(cString: buffer)
        }
        return nil
    }
}

/// Phase shown in the AVP USB status pill.
enum AVPUSBConnectionPhase: Equatable {
    case testing
    case linkUnreachable
    case linkReachable
    /// The runtime reports an active session with a Vision Pro.
    case sessionActive

    var pillTitle: String {
        switch self {
        case .testing:
            return "Testing"
        case .linkUnreachable:
            return "USB link unreachable"
        case .linkReachable:
            return "USB link reachable"
        case .sessionActive:
            return "Connected"
        }
    }

    /// Only the runtime session counts as ready; a successful ping alone does not.
    var isSessionActive: Bool {
        self == .sessionActive
    }

    /// Decides the phase from the ping result and the runtime's session status.
    static func resolve(
        isTesting: Bool,
        linkStatus: AVPUSBLinkStatus,
        runtimeActivity: HomeRuntimeActivity
    ) -> AVPUSBConnectionPhase {
        if runtimeActivity.state == .streaming,
           runtimeActivity.deviceType == nil || runtimeActivity.deviceType == .visionPro {
            return .sessionActive
        }
        if isTesting {
            return .testing
        }
        return linkStatus.isReachable ? .linkReachable : .linkUnreachable
    }
}

/// Home-side state for the AVP USB tab: entered address, ping result and persisted selection.
@MainActor
final class AVPUSBEthernetLinkModel: ObservableObject {
    @Published private(set) var hostAddress: String
    @Published private(set) var status: AVPUSBLinkStatus
    @Published private(set) var isTesting = false
    @Published private(set) var isSelected: Bool

    private let defaults: UserDefaults
    private let hostAddressKey = "avpUsbHostAddress"
    private let isSelectedKey = "avpUsbTabSelected"
    private var latestTestID = 0
    private var scheduledTestTask: Task<Void, Never>?

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let address = defaults.string(forKey: hostAddressKey) ?? ""
        hostAddress = address
        isSelected = defaults.bool(forKey: isSelectedKey)
        status = .disconnected(address, message: AVPUSBEthernetLink.idleMessage(for: address))
    }

    func setHostAddress(_ address: String) {
        guard hostAddress != address else {
            return
        }
        hostAddress = address
        defaults.set(address, forKey: hostAddressKey)
        cancelPendingTest()
        status = .disconnected(address, message: AVPUSBEthernetLink.idleMessage(for: address))
    }

    /// Remembers whether the AVP USB tab is selected. Stored only in UserDefaults, never in the runtime config.
    func setSelected(_ selected: Bool) {
        guard isSelected != selected else {
            return
        }
        isSelected = selected
        defaults.set(selected, forKey: isSelectedKey)
    }

    func cancelPendingTest() {
        scheduledTestTask?.cancel()
        scheduledTestTask = nil
        latestTestID += 1
        isTesting = false
    }

    /// Tests shortly after the address was edited, so typing an IP does not ping every keystroke.
    func scheduleTestAfterAddressEntry(delay: Duration = .milliseconds(800)) {
        scheduledTestTask?.cancel()
        scheduledTestTask = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, self.isSelected else {
                return
            }
            self.test()
        }
    }

    /// - Parameter silently: re-checks without switching to "Testing", used to notice a dropped link
    ///   while it is currently reachable.
    func test(silently: Bool = false) {
        guard !isTesting else {
            return
        }
        let address = hostAddress
        latestTestID += 1
        let requestID = latestTestID
        if !silently {
            isTesting = true
            status = .disconnected(address, message: "Testing USB Ethernet link...")
        }

        Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                AVPUSBEthernetLink.testConnection(hostAddress: address)
            }.value
            guard let self,
                  requestID == self.latestTestID,
                  self.hostAddress == address else {
                return
            }
            self.isTesting = false
            if self.status != result {
                self.status = result
            }
        }
    }
}

