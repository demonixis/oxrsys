// SPDX-License-Identifier: MPL-2.0

// ControlChannel.swift — Bidirectional control on UDP 9946.
// Sends latency reports and keyframe requests to the server.
// Receives haptics commands (future: vibration via CoreHaptics).

import Foundation

public final class ControlChannel: Sendable {
    private let queue = DispatchQueue(label: "oxr.control", qos: .userInitiated)
    private nonisolated(unsafe) var socket: Int32 = -1
    private nonisolated(unsafe) var serverAddr = sockaddr_in()

    public init() {}

    public func connect(serverIP: String, port: UInt16 = OXRProtocol.controlPort) {
        queue.async { [self] in
            if socket >= 0 { close(socket) }

            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else { return }

            serverAddr = sockaddr_in()
            serverAddr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            serverAddr.sin_family = sa_family_t(AF_INET)
            serverAddr.sin_port = port.bigEndian
            inet_pton(AF_INET, serverIP, &serverAddr.sin_addr)

            socket = fd
            print("[Control] Connected to \(serverIP):\(port)")
        }
    }

    public func sendLatencyReport(_ report: LatencyReport) {
        guard socket >= 0 else { return }
        var r = report
        sendRaw(&r, size: MemoryLayout<LatencyReport>.size)
    }

    public func requestKeyframe(reason: UInt32 = KeyframeReason.frameLoss.rawValue, detail: UInt32 = 0) {
        guard socket >= 0 else { return }
        var req = RequestKeyframe()
        req.reasonFlags = reason
        req.detail = detail
        sendRaw(&req, size: MemoryLayout<RequestKeyframe>.size)
    }

    public func disconnect() {
        queue.async { [self] in
            if socket >= 0 {
                close(socket)
                socket = -1
                print("[Control] Disconnected")
            }
        }
    }

    private func sendRaw(_ data: UnsafeMutableRawPointer, size: Int) {
        withUnsafePointer(to: serverAddr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                _ = sendto(socket, data, size, 0, sa,
                           socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}
