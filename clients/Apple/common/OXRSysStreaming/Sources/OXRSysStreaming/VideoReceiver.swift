// SPDX-License-Identifier: MPL-2.0

// VideoReceiver.swift — Receives encoded video packets on UDP 9944,
// reassembles fragmented NAL units, delivers complete frames.
//
// Hot path uses raw memory (UnsafeMutablePointer) to avoid Swift Data COW overhead
// which was causing the recv loop to be too slow → kernel UDP buffer overflow → packet loss.

import Foundation
import os

public final class VideoReceiver: @unchecked Sendable {
    public typealias OnNalUnit = @Sendable (Data, Int64, Int64) -> Void
    public typealias OnEncodedNalUnit = @Sendable (EncodedNalUnit) -> Void
    /// (presentationTimeNs, orientation xyzw) — the head orientation the server rendered this
    /// frame for, echoed back so the client can reproject the frame to the live head pose.
    public typealias OnRenderPose = @Sendable (Int64, (Float, Float, Float, Float)) -> Void

    public struct EncodedNalUnit: Sendable {
        public let data: Data
        public let codec: VideoCodec
        public let presentationTimeNs: Int64
        public let receiveTimeNs: Int64
    }

    private struct State {
        var socket: Int32 = -1
        var running = false
        var packetsReceived: UInt32 = 0
        var nalUnitsDelivered: UInt32 = 0
        var groupsDropped: UInt32 = 0
        var totalFramesSeen: UInt32 = 0
        var lastFrameDeliveryTimeNs: Int64 = 0
        var lastPacketReceivedTimeNs: Int64 = 0
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    public var packetsReceived: UInt32 { state.withLock { $0.packetsReceived } }
    public var framesDelivered: UInt32 { state.withLock { $0.nalUnitsDelivered } }
    public var framesDropped: UInt32 { state.withLock { $0.groupsDropped } }
    public var totalFramesSeen: UInt32 { state.withLock { $0.totalFramesSeen } }
    public var lastFrameDeliveryTimeNs: Int64 { state.withLock { $0.lastFrameDeliveryTimeNs } }
    public var lastPacketReceivedTimeNs: Int64 { state.withLock { $0.lastPacketReceivedTimeNs } }

    public init() {}

    public func start(onNalUnit: @escaping OnNalUnit, onRenderPose: OnRenderPose? = nil) {
        startReceivingEncoded(onNalUnit: { frame in
            onNalUnit(frame.data, frame.presentationTimeNs, frame.receiveTimeNs)
        }, onRenderPose: onRenderPose)
    }

    public func startReceivingEncoded(onNalUnit: @escaping OnEncodedNalUnit, onRenderPose: OnRenderPose? = nil) {
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
                print("[VideoRecv] Failed to create socket: \(errno)")
                return
            }

            var yes: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))

            // Request large receive buffer — critical for high-bitrate UDP video
            var rcvBuf: Int32 = 8 * 1024 * 1024
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, socklen_t(MemoryLayout<Int32>.size))

            // Verify actual buffer size
            var actualBuf: Int32 = 0
            var optLen = socklen_t(MemoryLayout<Int32>.size)
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actualBuf, &optLen)
            print("[VideoRecv] Requested \(rcvBuf) byte recv buffer, got \(actualBuf)")

            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = OXRProtocol.videoPort.bigEndian
            addr.sin_addr.s_addr = INADDR_ANY

            let bindResult = withUnsafePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            guard bindResult == 0 else {
                print("[VideoRecv] Bind failed: \(errno)")
                close(fd)
                state.withLock { $0.running = false }
                return
            }

            var tv = timeval(tv_sec: 0, tv_usec: 10_000) // 10ms timeout for responsive stop() and frame timeout
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
            print("[VideoRecv] Listening on port \(OXRProtocol.videoPort)")

            self.receiveLoop(fd: fd, onNalUnit: onNalUnit, onRenderPose: onRenderPose)

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
            print("[VideoRecv] Stopped")
        }
        thread.qualityOfService = .userInteractive
        thread.name = "oxr.video.recv"
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

    private func isRunning() -> Bool {
        state.withLock { $0.running }
    }

    // MARK: - Receive loop (raw memory, zero-copy hot path)

    private func receiveLoop(fd: Int32, onNalUnit: @escaping OnEncodedNalUnit, onRenderPose: OnRenderPose?) {
        let headerSize = MemoryLayout<VideoPacketHeader>.size
        let maxPacketSize = headerSize + OXRProtocol.maxPacketPayload

        // Receive buffer — stack-like, reused every iteration
        let recvBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: maxPacketSize)
        defer { recvBuf.deallocate() }

        // Per-packet tracking. Keep a hard cap so a malformed or oversized
        // NAL cannot grow receiver memory without bound.
        let maxFrameBytes = 16 * 1024 * 1024
        let maxPacketsPerFrame = maxFrameBytes / OXRProtocol.maxPacketPayload
        let frameBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: maxFrameBytes)
        defer { frameBuf.deallocate() }

        let packetSizes = UnsafeMutablePointer<UInt16>.allocate(capacity: maxPacketsPerFrame)
        let packetReceived = UnsafeMutablePointer<Bool>.allocate(capacity: maxPacketsPerFrame)
        defer { packetSizes.deallocate(); packetReceived.deallocate() }

        // FEC parity tracking
        let maxFecGroups = (maxPacketsPerFrame + FEC.groupSize - 1) / FEC.groupSize
        let fecReceived = UnsafeMutablePointer<Bool>.allocate(capacity: maxFecGroups)
        let fecData = UnsafeMutablePointer<UInt8>.allocate(capacity: maxFecGroups * OXRProtocol.maxPacketPayload)
        let fecGroupLastPacketSizes = UnsafeMutablePointer<UInt16>.allocate(capacity: maxFecGroups)
        defer {
            fecReceived.deallocate()
            fecData.deallocate()
            fecGroupLastPacketSizes.deallocate()
        }
        var fecGroupCount: Int = 0
        var _fecRecoveries: UInt32 = 0

        var currentFrameIndex: UInt32 = UInt32.max
        var totalExpected: UInt16 = 0
        var receivedCount: UInt16 = 0
        var frameTimestamp: Int64 = 0
        var frameCodec: VideoCodec = .h265
        var lastGroupPacketTimeNs: Int64 = 0
        var droppedFrameIndex: UInt32?

        // Closure: attempt FEC recovery for all groups, returns true if frame is now complete
        func tryFecRecovery() -> Bool {
            guard totalExpected > 0 else { return false }
            var recovered = false
            for g in 0..<fecGroupCount {
                guard fecReceived[g] else { continue }
                let groupStart = g * FEC.groupSize
                let groupEnd = min(groupStart + FEC.groupSize, Int(totalExpected))
                // Count missing in this group
                var missingIdx = -1
                var missingCount = 0
                for i in groupStart..<groupEnd {
                    if !packetReceived[i] {
                        missingIdx = i
                        missingCount += 1
                    }
                }
                guard missingCount == 1 else { continue }
                // XOR recovery: start with FEC parity, XOR out present packets
                let fecSrc = fecData + g * OXRProtocol.maxPacketPayload
                let dst = frameBuf + missingIdx * OXRProtocol.maxPacketPayload
                memcpy(dst, fecSrc, OXRProtocol.maxPacketPayload)
                for i in groupStart..<groupEnd where i != missingIdx {
                    let src = frameBuf + i * OXRProtocol.maxPacketPayload
                    for j in 0..<Int(packetSizes[i]) {
                        dst[j] ^= src[j]
                    }
                }
                packetReceived[missingIdx] = true
                var recoveredSize = UInt16(OXRProtocol.maxPacketPayload)
                if missingIdx == groupEnd - 1 {
                    let groupLastPacketSize = fecGroupLastPacketSizes[g]
                    if groupLastPacketSize > 0 && Int(groupLastPacketSize) <= OXRProtocol.maxPacketPayload {
                        recoveredSize = groupLastPacketSize
                    }
                }
                packetSizes[missingIdx] = recoveredSize
                receivedCount &+= 1
                recovered = true
                _fecRecoveries &+= 1
                if _fecRecoveries <= 10 || _fecRecoveries % 100 == 0 {
                    print("[VideoRecv] FEC recovered packet \(missingIdx)/\(totalExpected) in frame \(currentFrameIndex) (recovery #\(_fecRecoveries))")
                }
            }
            return recovered && receivedCount == totalExpected
        }

        // Closure: deliver the completed frame
        func deliverFrame() {
            let finalSize = computeFinalSize(packetSizes, Int(totalExpected))
            let deliveryTimeNs = Self.monotonicNs()
            let deliveredCount = state.withLock { state in
                state.nalUnitsDelivered &+= 1
                state.lastFrameDeliveryTimeNs = deliveryTimeNs
                return state.nalUnitsDelivered
            }
            if deliveredCount <= 10 || deliveredCount % 200 == 0 {
                print("[VideoRecv] NAL #\(deliveredCount) (\(finalSize) bytes, frame \(currentFrameIndex))")
            }
            deliverNalUnit(frameBuf, finalSize, frameCodec, frameTimestamp, onNalUnit)
            totalExpected = 0
        }

        while isRunning() {
            let n = recv(fd, recvBuf, maxPacketSize, 0)

            // Idle timeout: drop a partial frame only when no packet has arrived for >500ms.
            if n <= 0 && totalExpected > 0 && receivedCount < totalExpected && lastGroupPacketTimeNs > 0 {
                let idleMs = (Self.monotonicNs() - lastGroupPacketTimeNs) / 1_000_000
                if idleMs > 500 {
                    // Last chance: try FEC recovery before dropping
                    if tryFecRecovery() {
                        deliverFrame()
                    } else {
                        state.withLock { $0.groupsDropped &+= 1 }
                        totalExpected = 0
                        receivedCount = 0
                    }
                }
            }

            guard n > headerSize else { continue }

            state.withLock { $0.packetsReceived &+= 1 }

            // Parse header directly from raw memory (no Data, no copies)
            let header = UnsafeRawPointer(recvBuf).loadUnaligned(as: VideoPacketHeader.self)
            let payloadSize = min(Int(header.payloadSize), n - headerSize)

            // Render pose packet — the head pose the server rendered this frame for. The payload
            // is position xyz then orientation xyzw (7 floats); we use the orientation to reproject.
            if header.flags & VideoFlags.renderPose != 0 {
                if let onRenderPose, payloadSize >= MemoryLayout<Float>.size * 7 {
                    let base = UnsafeRawPointer(recvBuf + headerSize)
                    let ox = base.loadUnaligned(fromByteOffset: 12, as: Float.self)
                    let oy = base.loadUnaligned(fromByteOffset: 16, as: Float.self)
                    let oz = base.loadUnaligned(fromByteOffset: 20, as: Float.self)
                    let ow = base.loadUnaligned(fromByteOffset: 24, as: Float.self)
                    onRenderPose(header.presentationTimeNs, (ox, oy, oz, ow))
                }
                continue
            }

            if let dropped = droppedFrameIndex {
                if header.frameIndex == dropped {
                    continue
                }
                droppedFrameIndex = nil
            }

            // FEC parity packet — store separately
            if header.flags & VideoFlags.fec != 0 {
                if header.frameIndex == currentFrameIndex && totalExpected > 0 {
                    let groupIdx = Int(header.packetIndex)
                    if groupIdx < fecGroupCount && !fecReceived[groupIdx] {
                        let fecOffset = groupIdx * OXRProtocol.maxPacketPayload
                        memcpy(fecData + fecOffset, recvBuf + headerSize,
                               min(payloadSize, OXRProtocol.maxPacketPayload))
                        fecGroupLastPacketSizes[groupIdx] = header.fecGroupLastPacketPayloadSize
                        fecReceived[groupIdx] = true
                        // Try recovery if frame is almost complete
                        if receivedCount + 1 >= totalExpected && tryFecRecovery() {
                            deliverFrame()
                        }
                    }
                }
                continue
            }

            // New NAL unit group?
            let isNewGroup = (header.frameIndex != currentFrameIndex) || (totalExpected == 0)

            if isNewGroup {
                // Handle previous incomplete group
                if header.frameIndex != currentFrameIndex {
                    if receivedCount > 0 && receivedCount == totalExpected && totalExpected > 0 {
                        deliverFrame()
                    } else if totalExpected > 0 && receivedCount < totalExpected {
                        // Try FEC recovery first
                        if tryFecRecovery() {
                            deliverFrame()
                        } else {
                            // Fallback: deliver partial if >=97% complete (zero-fill gaps)
                            let completionPct = (UInt32(receivedCount) * 100) / UInt32(totalExpected)
                            if completionPct >= 97 {
                                let count = min(Int(totalExpected), maxPacketsPerFrame)
                                for i in 0..<count where !packetReceived[i] {
                                    let offset = i * OXRProtocol.maxPacketPayload
                                    if offset < maxFrameBytes {
                                        let fillLen = min(OXRProtocol.maxPacketPayload, maxFrameBytes - offset)
                                        memset(frameBuf + offset, 0, fillLen)
                                        packetSizes[i] = UInt16(OXRProtocol.maxPacketPayload)
                                    }
                                }
                                deliverFrame()
                            } else {
                                let droppedCount = state.withLock { state in
                                    state.groupsDropped &+= 1
                                    return state.groupsDropped
                                }
                                if droppedCount <= 20 || droppedCount % 100 == 0 {
                                    print("[VideoRecv] Dropped: got \(receivedCount)/\(totalExpected) pkts for frame \(currentFrameIndex)")
                                }
                            }
                        }
                    }
                }

                currentFrameIndex = header.frameIndex
                state.withLock { $0.totalFramesSeen &+= 1 }
                receivedCount = 0
                frameTimestamp = header.presentationTimeNs
                let expectedPackets = Int(header.totalPackets)
                if expectedPackets == 0 || expectedPackets > maxPacketsPerFrame {
                    let droppedCount = state.withLock { state in
                        state.groupsDropped &+= 1
                        return state.groupsDropped
                    }
                    if droppedCount <= 20 || droppedCount % 100 == 0 {
                        print("[VideoRecv] Dropping frame \(header.frameIndex): \(expectedPackets) packets exceed receiver cap \(maxPacketsPerFrame)")
                    }
                    totalExpected = 0
                    fecGroupCount = 0
                    droppedFrameIndex = header.frameIndex
                    continue
                }
                totalExpected = header.totalPackets
                guard let codec = VideoCodec(rawValue: UInt32(header.codec)) else {
                    print("[VideoRecv] Dropping frame \(header.frameIndex) with unknown codec \(header.codec)")
                    totalExpected = 0
                    fecGroupCount = 0
                    droppedFrameIndex = header.frameIndex
                    state.withLock { $0.groupsDropped &+= 1 }
                    continue
                }
                frameCodec = codec
                lastGroupPacketTimeNs = 0

                // Zero the tracking arrays
                let count = Int(totalExpected)
                packetReceived.initialize(repeating: false, count: count)
                packetSizes.initialize(repeating: 0, count: count)

                // Initialize FEC tracking
                fecGroupCount = (count + FEC.groupSize - 1) / FEC.groupSize
                fecReceived.initialize(repeating: false, count: fecGroupCount)
                fecGroupLastPacketSizes.initialize(repeating: 0, count: fecGroupCount)
            }

            let idx = Int(header.packetIndex)
            guard idx < Int(totalExpected), idx < maxPacketsPerFrame else { continue }
            guard !packetReceived[idx] else { continue }

            // Copy payload directly into frame buffer — no Swift Data involved
            let dstOffset = idx * OXRProtocol.maxPacketPayload
            guard dstOffset + payloadSize <= maxFrameBytes else { continue }
            memcpy(frameBuf + dstOffset, recvBuf + headerSize, payloadSize)
            packetReceived[idx] = true
            packetSizes[idx] = UInt16(payloadSize)
            receivedCount &+= 1
            let nowNs = Self.monotonicNs()
            lastGroupPacketTimeNs = nowNs
            state.withLock { $0.lastPacketReceivedTimeNs = nowNs }

            // Frame complete — deliver
            if receivedCount == totalExpected {
                deliverFrame()
            }
        }
    }

    private func deliverNalUnit(_ buf: UnsafeMutablePointer<UInt8>, _ size: Int,
                                 _ codec: VideoCodec, _ timestamp: Int64, _ callback: OnEncodedNalUnit) {
        // Single copy from raw buffer → Data for delivery
        let data = Data(bytes: buf, count: size)
        let recvTime = Self.monotonicNs()
        callback(EncodedNalUnit(
            data: data,
            codec: codec,
            presentationTimeNs: timestamp,
            receiveTimeNs: recvTime
        ))
    }

    private func computeFinalSize(_ packetSizes: UnsafeMutablePointer<UInt16>, _ totalExpected: Int) -> Int {
        guard totalExpected > 0 else { return 0 }
        var finalSize = 0
        for i in 0..<totalExpected {
            finalSize += Int(packetSizes[i])
        }
        return finalSize
    }

    public static func monotonicNs() -> Int64 {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        let ticks = mach_absolute_time()
        return Int64(ticks * UInt64(info.numer) / UInt64(info.denom))
    }
}
