// SPDX-License-Identifier: MPL-2.0

// TrackingSender.swift — Sends 6DOF tracking data back to the server on UDP 9945.
// Called from the ARKit frame callback at display refresh rate.

import Foundation
import os

public final class TrackingSender: @unchecked Sendable {
    private struct State {
        var socket: Int32 = -1
        var serverAddr = sockaddr_in()
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    public init() {}

    public func connect(serverIP: String, port: UInt16 = OXRProtocol.trackingPort) {
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else {
            print("[TrackingSend] Failed to create socket: \(errno)")
            return
        }

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        inet_pton(AF_INET, serverIP, &addr.sin_addr)
        let serverAddr = addr

        let oldSocket = state.withLock { state in
            let previous = state.socket
            state.socket = fd
            state.serverAddr = serverAddr
            return previous
        }
        if oldSocket >= 0 {
            close(oldSocket)
        }
        print("[TrackingSend] Connected to \(serverIP):\(port)")
    }

    public func send(_ packet: TrackingPacket) {
        // Called at high frequency — send inline on calling thread for lowest latency
        let snapshot = state.withLock { state in
            (socket: state.socket, serverAddr: state.serverAddr)
        }
        guard snapshot.socket >= 0 else { return }

        var pkt = packet
        var serverAddr = snapshot.serverAddr
        withUnsafeBytes(of: &pkt) { raw in
            withUnsafePointer(to: &serverAddr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    _ = sendto(snapshot.socket, raw.baseAddress, raw.count, 0, sa,
                               socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    public func disconnect() {
        let socketToClose = state.withLock { state in
            let socket = state.socket
            state.socket = -1
            return socket
        }
        if socketToClose >= 0 {
            close(socketToClose)
            print("[TrackingSend] Disconnected")
        }
    }
}
