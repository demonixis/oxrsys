// SPDX-License-Identifier: MPL-2.0

// DiscoveryClient.swift — Receives ServerAnnounce through UDP broadcast or a bounded
// direct unicast request, then sends ClientConnect back to the server.

import Foundation
import os

public struct DiscoveredServer: Sendable {
    public let announce: ServerAnnounce
    public let address: String // Server IP

    public var name: String { announce.serverNameString }
    public var resolution: String { "\(announce.encodedWidth)x\(announce.encodedHeight)" }
    public var refreshRate: UInt32 { announce.refreshRateHz }

    public init(announce: ServerAnnounce, address: String) {
        self.announce = announce
        self.address = address
    }
}

public final class DiscoveryClient: @unchecked Sendable {
    private static let directDiscoveryAttempts = 3
    private static let directDiscoveryResolutionTimeout = DispatchTimeInterval.seconds(2)
    private static let directDiscoveryTimeoutMicroseconds: Int32 = 750_000

    private final class IPv4Resolution: @unchecked Sendable {
        private let result = OSAllocatedUnfairLock<sockaddr_in?>(initialState: nil)
        let completed = DispatchSemaphore(value: 0)

        func finish(with address: sockaddr_in?) {
            result.withLock { $0 = address }
            completed.signal()
        }

        func address() -> sockaddr_in? {
            result.withLock { $0 }
        }
    }

    private struct State {
        var socket: Int32 = -1
        var running = false
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    public init() {}

    /// Returns a canonical IPv4 address or DNS hostname suitable for direct discovery.
    /// Ports and URL syntax are rejected because the runtime control port is fixed by protocol.
    public static func normalizedDirectHost(_ rawHost: String) -> String? {
        var host = rawHost.trimmingCharacters(in: .whitespacesAndNewlines)
        if host.hasSuffix(".") {
            host.removeLast()
        }

        guard !host.isEmpty, host.utf8.count <= 253 else { return nil }
        guard !host.contains("://"), !host.contains(":"),
              !host.contains("/"), !host.contains("\\") else { return nil }
        guard !host.unicodeScalars.contains(where: CharacterSet.whitespacesAndNewlines.contains) else {
            return nil
        }

        let labels = host.split(separator: ".", omittingEmptySubsequences: false)
        guard !labels.isEmpty else { return nil }
        for label in labels {
            let bytes = label.utf8
            guard !bytes.isEmpty, bytes.count <= 63,
                  bytes.first != 45, bytes.last != 45 else { return nil }
            guard bytes.allSatisfy({ byte in
                (48...57).contains(byte) ||
                (65...90).contains(byte) ||
                (97...122).contains(byte) ||
                byte == 45
            }) else { return nil }
        }
        return host
    }

    public func start(onServerFound: @escaping @Sendable (DiscoveredServer) -> Void) {
        let shouldStart = state.withLock { state in
            if state.running {
                return false
            }
            state.running = true
            return true
        }
        guard shouldStart else { return }

        let thread = Thread { [self] in
            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else {
                state.withLock { $0.running = false }
                print("[Discovery] Failed to create socket: \(errno)")
                return
            }

            var yes: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, socklen_t(MemoryLayout<Int32>.size))

            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = OXRProtocol.discoveryPort.bigEndian
            addr.sin_addr.s_addr = INADDR_ANY

            let bindResult = withUnsafePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            guard bindResult == 0 else {
                print("[Discovery] Bind failed: \(errno)")
                close(fd)
                state.withLock { $0.running = false }
                return
            }

            var tv = timeval(tv_sec: 1, tv_usec: 0)
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            let shouldContinue = state.withLock { state in
                guard state.running else { return false }
                state.socket = fd
                return true
            }
            guard shouldContinue else {
                close(fd)
                return
            }
            print("[Discovery] Listening on port \(OXRProtocol.discoveryPort)")

            var buf = [UInt8](repeating: 0, count: 2048)
            var senderAddr = sockaddr_in()
            var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

            while isRunning() {
                senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &senderAddr) { ptr in
                    ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        recvfrom(fd, &buf, buf.count, 0, sa, &senderLen)
                    }
                }

                guard n > 0 else { continue }
                guard buf[0] == MessageType.serverAnnounce.rawValue else { continue }
                guard n >= OXRProtocol.serverAnnounceBaseSize else { continue }

                var announce = ServerAnnounce()
                withUnsafeMutableBytes(of: &announce) { announceBytes in
                    buf.withUnsafeBytes { raw in
                        announceBytes.copyBytes(from: raw.prefix(min(n, announceBytes.count)))
                    }
                }

                let ip = withUnsafePointer(to: senderAddr.sin_addr) { ptr in
                    var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                    inet_ntop(AF_INET, ptr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
                    return String(cString: ipBuf)
                }

                let server = DiscoveredServer(announce: announce, address: ip)
                print("[Discovery] Found server: \(server.name) at \(ip)")
                onServerFound(server)
            }

            let shouldClose = state.withLock { state in
                state.running = false
                if state.socket == fd {
                    state.socket = -1
                    return true
                }
                return false
            }
            if shouldClose {
                close(fd)
            }
            print("[Discovery] Stopped")
        }
        thread.qualityOfService = .userInitiated
        thread.name = "oxr.discovery"
        thread.start()
    }

    /// Requests a real ServerAnnounce from a known runtime address. The request is a one-byte,
    /// append-only protocol message sent to UDP 9946 from an ephemeral source port. Retries and
    /// receive waits are bounded so a missing runtime cannot leave the UI spinning indefinitely.
    public func requestServer(
        at rawHost: String,
        onServerFound: @escaping @Sendable (DiscoveredServer) -> Void,
        onError: @escaping @Sendable (String) -> Void
    ) {
        guard let host = Self.normalizedDirectHost(rawHost) else {
            onError("Enter a valid IPv4 address or hostname without a port.")
            return
        }

        let shouldStart = state.withLock { state in
            if state.running {
                return false
            }
            state.running = true
            return true
        }
        guard shouldStart else {
            onError("A server search is already in progress.")
            return
        }

        let thread = Thread { [self] in
            let resolution = IPv4Resolution()
            DispatchQueue.global(qos: .userInitiated).async {
                resolution.finish(with: Self.resolveIPv4Address(host))
            }

            let resolutionResult = resolution.completed.wait(
                timeout: .now() + Self.directDiscoveryResolutionTimeout
            )
            guard resolutionResult == .success else {
                if finishPendingStart() {
                    onError("Timed out resolving \(host) to an IPv4 address.")
                }
                return
            }
            guard var targetAddress = resolution.address() else {
                if finishPendingStart() {
                    onError("Could not resolve \(host) to an IPv4 address.")
                }
                return
            }
            targetAddress.sin_port = OXRProtocol.controlPort.bigEndian

            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else {
                if finishPendingStart() {
                    onError("Could not create the direct discovery socket.")
                }
                return
            }

            var timeout = timeval(tv_sec: 0, tv_usec: Self.directDiscoveryTimeoutMicroseconds)
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       socklen_t(MemoryLayout<timeval>.size))

            let shouldContinue = state.withLock { state in
                guard state.running else { return false }
                state.socket = fd
                return true
            }
            guard shouldContinue else {
                close(fd)
                return
            }

            var request = MessageType.discoveryRequest.rawValue
            var buffer = [UInt8](repeating: 0, count: 2048)

            for _ in 0..<Self.directDiscoveryAttempts where isRunning() {
                let sent = withUnsafePointer(to: &targetAddress) { pointer in
                    pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                        sendto(fd, &request, MemoryLayout<UInt8>.size, 0, socketAddress,
                               socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
                guard sent == MemoryLayout<UInt8>.size else { continue }

                var senderAddress = sockaddr_in()
                var senderLength = socklen_t(MemoryLayout<sockaddr_in>.size)
                let received = withUnsafeMutablePointer(to: &senderAddress) { pointer in
                    pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                        recvfrom(fd, &buffer, buffer.count, 0, socketAddress, &senderLength)
                    }
                }

                guard received >= OXRProtocol.serverAnnounceBaseSize,
                      buffer[0] == MessageType.serverAnnounce.rawValue,
                      senderAddress.sin_family == sa_family_t(AF_INET),
                      senderAddress.sin_port == targetAddress.sin_port,
                      senderAddress.sin_addr.s_addr == targetAddress.sin_addr.s_addr else {
                    continue
                }

                var announce = ServerAnnounce()
                withUnsafeMutableBytes(of: &announce) { announceBytes in
                    buffer.withUnsafeBytes { raw in
                        announceBytes.copyBytes(from: raw.prefix(min(received, announceBytes.count)))
                    }
                }

                guard let address = Self.ipv4String(senderAddress.sin_addr) else { continue }
                let server = DiscoveredServer(announce: announce, address: address)
                guard finish(socket: fd) else { return }
                print("[Discovery] Directly found server: \(server.name) at \(address)")
                onServerFound(server)
                return
            }

            guard finish(socket: fd) else { return }
            onError(
                "No OXRSys runtime responded at \(host):\(OXRProtocol.controlPort). " +
                "Check the address, runtime state, and local network."
            )
        }
        thread.qualityOfService = .userInitiated
        thread.name = "oxr.discovery.direct"
        thread.start()
    }

    public func stop() {
        let socketToClose = state.withLock { state in
            state.running = false
            let socket = state.socket
            state.socket = -1
            return socket
        }
        if socketToClose >= 0 {
            close(socketToClose)
        }
    }

    /// Send ClientConnect and stop discovery. Does NOT block — sends immediately.
    public func sendConnect(
        to server: DiscoveredServer,
        deviceName: String = "OXRSys Client",
        maxBitrateMbps: UInt32 = OXRProtocol.clientMaxBitrateUseServerConfig,
        refreshRateHz: UInt32 = 0,
        clientCapabilities: UInt32 = 0
    ) {
        // Send on a separate socket — no dependency on the discovery thread.
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else {
            print("[Discovery] Failed to create socket for connect: \(errno)")
            return
        }

        var connect = ClientConnect()
        connect.preferredCodec = VideoCodec.h265.rawValue
        connect.supportedCodecs = ClientCodecCapability.h265 | ClientCodecCapability.h264
        connect.maxBitrateMbps = maxBitrateMbps
        connect.refreshRateHz = refreshRateHz
        connect.clientCapabilities = clientCapabilities
        connect.setDeviceName(deviceName)

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = OXRProtocol.controlPort.bigEndian
        inet_pton(AF_INET, server.address, &addr.sin_addr)

        let sent = withUnsafeBytes(of: &connect) { raw in
            withUnsafePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    sendto(fd, raw.baseAddress, raw.count, 0, sa,
                           socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }

        close(fd)

        if sent > 0 {
            print("[Discovery] Sent ClientConnect to \(server.address) (\(sent) bytes)")
        } else {
            print("[Discovery] Failed to send ClientConnect: \(errno)")
        }

        stop()
    }

    private func isRunning() -> Bool {
        state.withLock { $0.running }
    }

    private func finishPendingStart() -> Bool {
        state.withLock { state in
            guard state.running, state.socket < 0 else { return false }
            state.running = false
            return true
        }
    }

    /// Clears and closes a socket only when this worker still owns it. Returns false when stop()
    /// already cancelled and closed it, which also suppresses stale callbacks.
    private func finish(socket fd: Int32) -> Bool {
        let shouldClose = state.withLock { state in
            guard state.running, state.socket == fd else { return false }
            state.running = false
            state.socket = -1
            return true
        }
        if shouldClose {
            close(fd)
        }
        return shouldClose
    }

    private static func resolveIPv4Address(_ host: String) -> sockaddr_in? {
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_DGRAM
        hints.ai_protocol = IPPROTO_UDP

        var results: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &results) == 0, let results else { return nil }
        defer { freeaddrinfo(results) }

        var current: UnsafeMutablePointer<addrinfo>? = results
        while let entry = current {
            if entry.pointee.ai_family == AF_INET,
               entry.pointee.ai_addrlen >= socklen_t(MemoryLayout<sockaddr_in>.size),
               let address = entry.pointee.ai_addr {
                return UnsafeRawPointer(address).load(as: sockaddr_in.self)
            }
            current = entry.pointee.ai_next
        }
        return nil
    }

    private static func ipv4String(_ address: in_addr) -> String? {
        var address = address
        var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        guard inet_ntop(AF_INET, &address, &buffer, socklen_t(INET_ADDRSTRLEN)) != nil else {
            return nil
        }
        return String(cString: buffer)
    }
}
