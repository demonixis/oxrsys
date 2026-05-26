// SPDX-License-Identifier: MPL-2.0

// TrackingSender.swift — Sends 6DOF tracking data back to the server on UDP 9945.
// Called from the ARKit frame callback at display refresh rate.

import Foundation

public final class TrackingSender: Sendable {
    private let queue = DispatchQueue(label: "oxr.tracking.send", qos: .userInteractive)
    private nonisolated(unsafe) var socket: Int32 = -1
    private nonisolated(unsafe) var serverAddr = sockaddr_in()

    public init() {}

    public func connect(serverIP: String, port: UInt16 = OXRProtocol.trackingPort) {
        queue.async { [self] in
            if socket >= 0 { close(socket) }

            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else {
                print("[TrackingSend] Failed to create socket: \(errno)")
                return
            }

            serverAddr = sockaddr_in()
            serverAddr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            serverAddr.sin_family = sa_family_t(AF_INET)
            serverAddr.sin_port = port.bigEndian
            inet_pton(AF_INET, serverIP, &serverAddr.sin_addr)

            socket = fd
            print("[TrackingSend] Connected to \(serverIP):\(port)")
        }
    }

    public func send(_ packet: TrackingPacket) {
        // Called at high frequency — send inline on calling thread for lowest latency
        guard socket >= 0 else { return }

        var pkt = packet
        withUnsafeBytes(of: &pkt) { raw in
            withUnsafePointer(to: serverAddr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    _ = sendto(socket, raw.baseAddress, raw.count, 0, sa,
                               socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    public func disconnect() {
        queue.async { [self] in
            if socket >= 0 {
                close(socket)
                socket = -1
                print("[TrackingSend] Disconnected")
            }
        }
    }
}
