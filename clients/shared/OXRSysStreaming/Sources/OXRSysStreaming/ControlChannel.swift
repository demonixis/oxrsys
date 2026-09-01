// SPDX-License-Identifier: MPL-2.0

// ControlChannel.swift — Bidirectional control on UDP 9946.
// Sends latency reports and keyframe requests to the server.
// Receives haptics commands (future: vibration via CoreHaptics).

import Foundation
import os

public final class ControlChannel: @unchecked Sendable {
    private struct State {
        var socket: Int32 = -1
        var serverAddr = sockaddr_in()
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    public init() {}

    public func connect(serverIP: String, port: UInt16 = OXRProtocol.controlPort) {
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return }

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
        print("[Control] Connected to \(serverIP):\(port)")
    }

    public func sendLatencyReport(_ report: LatencyReport) {
        var r = report
        sendRaw(&r, size: MemoryLayout<LatencyReport>.size)
    }

    public func requestKeyframe(reason: UInt32 = KeyframeReason.frameLoss.rawValue, detail: UInt32 = 0) {
        var req = RequestKeyframe()
        req.reasonFlags = reason
        req.detail = detail
        sendRaw(&req, size: MemoryLayout<RequestKeyframe>.size)
    }

    public func disconnect() {
        let socketToClose = state.withLock { state in
            let socket = state.socket
            state.socket = -1
            return socket
        }
        if socketToClose >= 0 {
            close(socketToClose)
            print("[Control] Disconnected")
        }
    }

    private func sendRaw(_ data: UnsafeMutableRawPointer, size: Int) {
        let snapshot = state.withLock { state in
            (socket: state.socket, serverAddr: state.serverAddr)
        }
        guard snapshot.socket >= 0 else { return }

        var serverAddr = snapshot.serverAddr
        withUnsafePointer(to: &serverAddr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                _ = sendto(snapshot.socket, data, size, 0, sa,
                           socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}
