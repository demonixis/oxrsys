// SPDX-License-Identifier: MPL-2.0

import Darwin
import Foundation
import IOKit
import SystemConfiguration

/// Quest USB network (CDC-NCM) link. The Quest's USB function is switched to NCM over ADB; macOS
/// binds its built-in NCM driver and both ends get fixed addresses. The Quest gets no gateway, so
/// its default network stays Wi-Fi and the headset client binds its sockets to the cable network.
struct QuestUsbNetworkStatus: Equatable, Sendable {
    var interfaceName: String?
    var isReady: Bool
    var message: String

    nonisolated static var unknown: QuestUsbNetworkStatus {
        QuestUsbNetworkStatus(interfaceName: nil, isReady: false, message: "USB network status unavailable.")
    }
}

enum QuestUsbNetworkError: LocalizedError {
    case noHeadset
    case accessoryApprovalTimedOut
    case functionSwitchFailed
    case interfaceMissing
    case adbDidNotReturn
    case subnetInUse(String)
    case macConfigurationFailed(String)
    case questConfigurationFailed(String)
    case primaryRouteChanged
    case questUnreachable

    var errorDescription: String? {
        switch self {
        case .noHeadset:
            return "No Quest is connected over USB."
        case .accessoryApprovalTimedOut:
            return "macOS is blocking the headset's USB network. Unlock this Mac, click Allow on the accessory prompt, then try again."
        case .functionSwitchFailed:
            return "The headset did not switch to its USB network mode. Reconnect the cable and try again."
        case .interfaceMissing:
            return "macOS did not create a network interface for the headset. Unlock this Mac, then reconnect the cable."
        case .adbDidNotReturn:
            return "USB debugging did not come back after the headset switched to its USB network mode."
        case let .subnetInUse(interface):
            return "\(QuestUsbNetworkLink.subnetDescription) is already used by \(interface)."
        case let .macConfigurationFailed(detail):
            return "Could not configure this Mac's USB network address: \(detail)"
        case let .questConfigurationFailed(detail):
            return "Could not configure the headset's USB network address: \(detail)"
        case .primaryRouteChanged:
            return "Configuring the USB network changed this Mac's primary network; check Network settings."
        case .questUnreachable:
            return "The headset cannot reach this Mac on the USB network."
        }
    }
}

enum QuestUsbNetworkLink {
    nonisolated static var macAddress: String { "192.168.42.1" }
    nonisolated static var questAddress: String { "192.168.42.2" }
    nonisolated static var subnetDescription: String { "192.168.42.0/24" }
    nonisolated private static var netmask: String { "255.255.255.0" }
    nonisolated private static var questVendorID: Int { 0x2833 }
    nonisolated private static var networksetup: String { "/usr/sbin/networksetup" }

    struct QuestUsbFunctions: Equatable {
        var hasNcmFunction: Bool
        /// The device enumerated but macOS published no interfaces: the accessory approval gate.
        var isAccessoryBlocked: Bool
        var interfaceName: String?
    }

    nonisolated static func status() -> QuestUsbNetworkStatus {
        guard let functions = questUsbFunctions() else {
            return QuestUsbNetworkStatus(interfaceName: nil, isReady: false, message: "No Quest is connected over USB.")
        }
        if functions.isAccessoryBlocked {
            return QuestUsbNetworkStatus(
                interfaceName: nil,
                isReady: false,
                message: QuestUsbNetworkError.accessoryApprovalTimedOut.localizedDescription
            )
        }
        guard functions.hasNcmFunction, let interfaceName = functions.interfaceName else {
            return QuestUsbNetworkStatus(interfaceName: nil, isReady: false, message: "USB network is not set up.")
        }
        // Mac-side checks only: traffic from Home to the headset would need the Local Network
        // permission, and setUp has already verified the headset end over ADB.
        guard isUpWithAddress(interfaceName) else {
            return QuestUsbNetworkStatus(
                interfaceName: interfaceName,
                isReady: false,
                message: "\(interfaceName) does not have \(macAddress) yet."
            )
        }
        return QuestUsbNetworkStatus(
            interfaceName: interfaceName,
            isReady: true,
            message: "USB network ready on \(interfaceName): Mac \(macAddress), headset \(questAddress)."
        )
    }

    nonisolated static func setUp(serial: String, customAdbPath: String?) throws -> QuestUsbNetworkStatus {
        guard var functions = questUsbFunctions() else {
            throw QuestUsbNetworkError.noHeadset
        }
        if !functions.hasNcmFunction && !functions.isAccessoryBlocked {
            // ADB drops while the headset re-enumerates, so the command usually fails after the
            // switch has already happened; the IORegistry is the source of truth.
            _ = try? QuestUsbBridge.shell("svc usb setFunctions ncm", serial: serial, customAdbPath: customAdbPath)
            Thread.sleep(forTimeInterval: 1.5)
            guard let switched = poll(seconds: 20, { () -> QuestUsbFunctions? in
                guard let current = questUsbFunctions(),
                      current.hasNcmFunction || current.isAccessoryBlocked else {
                    return nil
                }
                return current
            }) else {
                throw QuestUsbNetworkError.functionSwitchFailed
            }
            functions = switched
        }
        if functions.isAccessoryBlocked {
            guard let approved = poll(seconds: 120, { () -> QuestUsbFunctions? in
                guard let current = questUsbFunctions(), !current.isAccessoryBlocked else {
                    return nil
                }
                return current
            }) else {
                throw QuestUsbNetworkError.accessoryApprovalTimedOut
            }
            functions = approved
        }
        guard functions.hasNcmFunction else {
            throw QuestUsbNetworkError.functionSwitchFailed
        }
        guard let interfaceName = functions.interfaceName ?? poll(seconds: 10, { questUsbFunctions()?.interfaceName }) else {
            throw QuestUsbNetworkError.interfaceMissing
        }

        let primaryBefore = primaryRoute()
        try configureMac(interfaceName: interfaceName)
        guard poll(seconds: 10, { isUpWithAddress(interfaceName) ? true : nil }) != nil else {
            throw QuestUsbNetworkError.macConfigurationFailed("\(interfaceName) did not come up with \(macAddress).")
        }
        guard primaryRoute() == primaryBefore else {
            throw QuestUsbNetworkError.primaryRouteChanged
        }

        guard poll(seconds: 15, { () -> Bool? in
            let devices = (try? QuestUsbBridge.devices(customAdbPath: customAdbPath)) ?? []
            return devices.contains { $0.serial == serial && $0.isUsable } ? true : nil
        }) != nil else {
            throw QuestUsbNetworkError.adbDidNotReturn
        }
        // Always applied: it also clears a gateway left by other tools, which would make the
        // cable the headset's default network.
        let output = (try? QuestUsbBridge.shell(
            "cmd ethernet set-ip-configuration usb0 static \(questAddress)/24",
            serial: serial,
            customAdbPath: customAdbPath
        )) ?? ""
        guard questStaticConfigurationApplied(output) else {
            throw QuestUsbNetworkError.questConfigurationFailed(
                output.isEmpty ? "no response" : output.trimmingCharacters(in: .whitespacesAndNewlines)
            )
        }

        // The headset pings the Mac, so Home itself sends no local network traffic.
        guard poll(seconds: 15, { () -> Bool? in
            let output = (try? QuestUsbBridge.shell(
                "ping -c 1 -W 1 -I usb0 \(macAddress)",
                serial: serial,
                customAdbPath: customAdbPath
            )) ?? ""
            return questPingSucceeded(output) ? true : nil
        }) != nil else {
            throw QuestUsbNetworkError.questUnreachable
        }
        return status()
    }

    /// Returns the headset to its default USB functions (Quest Link and the rest).
    nonisolated static func restoreDefaultUsbMode(serial: String, customAdbPath: String?) throws {
        guard let functions = questUsbFunctions() else {
            throw QuestUsbNetworkError.noHeadset
        }
        guard functions.hasNcmFunction else {
            return
        }
        _ = try? QuestUsbBridge.shell("svc usb setFunctions", serial: serial, customAdbPath: customAdbPath)
        Thread.sleep(forTimeInterval: 1.5)
        guard poll(seconds: 15, { () -> Bool? in
            guard let current = questUsbFunctions(), !current.hasNcmFunction, !current.isAccessoryBlocked else {
                return nil
            }
            return true
        }) != nil else {
            throw QuestUsbNetworkError.functionSwitchFailed
        }
    }

    // MARK: - Parsing

    /// `cmd ethernet set-ip-configuration` echoes the applied configuration.
    nonisolated static func questStaticConfigurationApplied(_ output: String) -> Bool {
        output.contains("STATIC") && output.contains("\(questAddress)/24")
    }

    /// Summary line of the headset's `ping -c 1`.
    nonisolated static func questPingSucceeded(_ output: String) -> Bool {
        output.contains("1 packets transmitted, 1 received")
    }

    /// Service name for a device from `networksetup -listnetworkserviceorder`.
    nonisolated static func serviceName(forDevice device: String, serviceOrder output: String) -> String? {
        var pendingName: String?
        for rawLine in output.split(whereSeparator: { $0.isNewline }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("(Hardware Port") {
                if let name = pendingName, line.contains("Device: \(device))") {
                    return name
                }
                pendingName = nil
            } else if line.hasPrefix("("), let close = line.firstIndex(of: ")") {
                let name = line[line.index(after: close)...].trimmingCharacters(in: .whitespaces)
                pendingName = name.hasPrefix("*") ? String(name.dropFirst()) : name
            }
        }
        return nil
    }

    /// Hardware port name for a device from `networksetup -listallhardwareports`.
    nonisolated static func hardwarePort(forDevice device: String, hardwarePorts output: String) -> String? {
        var port: String?
        for rawLine in output.split(whereSeparator: { $0.isNewline }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("Hardware Port:") {
                port = String(line.dropFirst("Hardware Port:".count)).trimmingCharacters(in: .whitespaces)
            } else if line == "Device: \(device)" {
                return port
            }
        }
        return nil
    }

    /// True when `networksetup -getinfo` shows the link's manual address and no router.
    nonisolated static func serviceInfoIsConfigured(_ output: String) -> Bool {
        output.contains("Manual Configuration") &&
            output.contains("IP address: \(macAddress)") &&
            serviceInfoHasNoRouter(output)
    }

    nonisolated static func serviceInfoHasNoRouter(_ output: String) -> Bool {
        !output.contains("Router:") || output.contains("Router: (null)")
    }

    /// Interface other than `except` that already has an address in the link subnet, from `ifconfig`.
    nonisolated static func interfaceUsingSubnet(ifconfig output: String, except: String) -> String? {
        var current = ""
        for line in output.split(whereSeparator: { $0.isNewline }) {
            if !line.hasPrefix("\t"), !line.hasPrefix(" "), let colon = line.firstIndex(of: ":") {
                current = String(line[..<colon])
                continue
            }
            let fields = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            if fields.first == "inet", fields.count > 1, fields[1].hasPrefix("192.168.42."), current != except {
                return current
            }
        }
        return nil
    }

    // MARK: - Mac network configuration

    nonisolated private static func configureMac(interfaceName: String) throws {
        // Never touch anything but the headset's own NCM interface.
        guard interfaceName.hasPrefix("en"), interfaceName != "en0" else {
            throw QuestUsbNetworkError.macConfigurationFailed("unexpected interface \(interfaceName).")
        }
        if let other = interfaceUsingSubnet(ifconfig: (try? Shell.run("/sbin/ifconfig", [])) ?? "", except: interfaceName) {
            throw QuestUsbNetworkError.subnetInUse(other)
        }

        do {
            var service = serviceName(
                forDevice: interfaceName,
                serviceOrder: try Shell.run(networksetup, ["-listnetworkserviceorder"])
            )
            if service == nil {
                guard let port = hardwarePort(
                    forDevice: interfaceName,
                    hardwarePorts: try Shell.run(networksetup, ["-listallhardwareports"])
                ) else {
                    throw QuestUsbNetworkError.macConfigurationFailed("no hardware port for \(interfaceName).")
                }
                try Shell.run(networksetup, ["-createnetworkservice", "Quest USB Network", port])
                service = serviceName(
                    forDevice: interfaceName,
                    serviceOrder: try Shell.run(networksetup, ["-listnetworkserviceorder"])
                )
            }
            guard let service else {
                throw QuestUsbNetworkError.macConfigurationFailed("no network service for \(interfaceName).")
            }
            if !serviceInfoIsConfigured(try Shell.run(networksetup, ["-getinfo", service])) {
                // No router argument: the service must never become this Mac's primary network.
                try Shell.run(networksetup, ["-setmanual", service, macAddress, netmask])
                guard serviceInfoHasNoRouter(try Shell.run(networksetup, ["-getinfo", service])) else {
                    throw QuestUsbNetworkError.macConfigurationFailed("network service \(service) has a router.")
                }
            }
        } catch let error as QuestUsbNetworkError {
            throw error
        } catch {
            throw QuestUsbNetworkError.macConfigurationFailed(error.localizedDescription)
        }
    }

    nonisolated private static func isUpWithAddress(_ interfaceName: String) -> Bool {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0 else {
            return false
        }
        defer { freeifaddrs(head) }

        var isUp = false
        var hasAddress = false
        var current = head
        while let entry = current {
            defer { current = entry.pointee.ifa_next }
            guard String(cString: entry.pointee.ifa_name) == interfaceName else {
                continue
            }
            let flags = Int32(entry.pointee.ifa_flags)
            isUp = flags & IFF_UP != 0 && flags & IFF_RUNNING != 0
            guard let address = entry.pointee.ifa_addr, address.pointee.sa_family == UInt8(AF_INET) else {
                continue
            }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count),
                           nil, 0, NI_NUMERICHOST) == 0,
               String(cString: host) == macAddress {
                hasAddress = true
            }
        }
        return isUp && hasAddress
    }

    nonisolated private static func primaryRoute() -> [String] {
        guard let state = SCDynamicStoreCopyValue(nil, "State:/Network/Global/IPv4" as CFString) as? [String: Any] else {
            return []
        }
        return [state["PrimaryInterface"] as? String ?? "", state["Router"] as? String ?? ""]
    }

    // MARK: - IORegistry

    nonisolated static func questUsbFunctions() -> QuestUsbFunctions? {
        guard let matching = IOServiceMatching("IOUSBHostDevice") else {
            return nil
        }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return nil
        }
        defer { IOObjectRelease(iterator) }

        while case let device = IOIteratorNext(iterator), device != 0 {
            defer { IOObjectRelease(device) }
            guard intProperty(device, "idVendor") == questVendorID else {
                continue
            }

            var interfaceCount = 0
            var hasNcmFunction = false
            var interfaceName: String?
            var childIterator: io_iterator_t = 0
            guard IORegistryEntryCreateIterator(device, kIOServicePlane,
                                                IOOptionBits(kIORegistryIterateRecursively),
                                                &childIterator) == KERN_SUCCESS else {
                continue
            }
            while case let child = IOIteratorNext(childIterator), child != 0 {
                if IOObjectConformsTo(child, "IOUSBHostInterface") != 0 {
                    interfaceCount += 1
                    if intProperty(child, "bInterfaceClass") == 2 && intProperty(child, "bInterfaceSubClass") == 13 {
                        hasNcmFunction = true
                    }
                }
                if interfaceName == nil,
                   let name = IORegistryEntryCreateCFProperty(child, "BSD Name" as CFString, kCFAllocatorDefault, 0)?
                       .takeRetainedValue() as? String,
                   name.hasPrefix("en") {
                    interfaceName = name
                }
                IOObjectRelease(child)
            }
            IOObjectRelease(childIterator)
            return QuestUsbFunctions(
                hasNcmFunction: hasNcmFunction,
                isAccessoryBlocked: interfaceCount == 0,
                interfaceName: interfaceName
            )
        }
        return nil
    }

    nonisolated private static func intProperty(_ entry: io_registry_entry_t, _ key: String) -> Int? {
        (IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? NSNumber)?.intValue
    }

    nonisolated private static func poll<T>(seconds: TimeInterval, _ body: () -> T?) -> T? {
        let deadline = Date().addingTimeInterval(seconds)
        repeat {
            if let value = body() {
                return value
            }
            Thread.sleep(forTimeInterval: 0.5)
        } while Date() < deadline
        return body()
    }
}
