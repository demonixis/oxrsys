// SPDX-License-Identifier: MPL-2.0

// DiscoveryClient.swift — Listens for ServerAnnounce broadcasts on UDP 9943,
// sends ClientConnect back to the server.

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
    private struct State {
        var socket: Int32 = -1
        var running = false
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    public init() {}

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
                guard n >= MemoryLayout<ServerAnnounce>.size else { continue }

                let announce = buf.withUnsafeBytes { raw in
                    raw.loadUnaligned(as: ServerAnnounce.self)
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
        refreshRateHz: UInt32 = 0
    ) {
        // Send on a separate socket — no dependency on the discovery thread.
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else {
            print("[Discovery] Failed to create socket for connect: \(errno)")
            return
        }

        var connect = ClientConnect()
        connect.maxBitrateMbps = maxBitrateMbps
        connect.refreshRateHz = refreshRateHz
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
}
