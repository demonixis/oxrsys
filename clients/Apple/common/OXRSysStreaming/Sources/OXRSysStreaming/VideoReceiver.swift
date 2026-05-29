// SPDX-License-Identifier: MPL-2.0

// VideoReceiver.swift — Receives H.265 video packets on UDP 9944,
// reassembles fragmented NAL units, delivers complete frames.
//
// Hot path uses raw memory (UnsafeMutablePointer) to avoid Swift Data COW overhead
// which was causing the recv loop to be too slow → kernel UDP buffer overflow → packet loss.

import Foundation

public final class VideoReceiver: Sendable {
    public typealias OnNalUnit = @Sendable (Data, Int64, Int64) -> Void

    private nonisolated(unsafe) var socket: Int32 = -1
    private nonisolated(unsafe) var running = false

    // Stats
    private nonisolated(unsafe) var _packetsReceived: UInt32 = 0
    private nonisolated(unsafe) var _nalUnitsDelivered: UInt32 = 0
    private nonisolated(unsafe) var _groupsDropped: UInt32 = 0
    private nonisolated(unsafe) var _totalFramesSeen: UInt32 = 0
    private nonisolated(unsafe) var _lastFrameDeliveryTimeNs: Int64 = 0
    private nonisolated(unsafe) var _lastPacketReceivedTimeNs: Int64 = 0

    public var packetsReceived: UInt32 { _packetsReceived }
    public var framesDelivered: UInt32 { _nalUnitsDelivered }
    public var framesDropped: UInt32 { _groupsDropped }
    public var totalFramesSeen: UInt32 { _totalFramesSeen }
    public var lastFrameDeliveryTimeNs: Int64 { _lastFrameDeliveryTimeNs }
    public var lastPacketReceivedTimeNs: Int64 { _lastPacketReceivedTimeNs }

    public init() {}

    public func start(onNalUnit: @escaping OnNalUnit) {
        guard !running else { return }

        let thread = Thread { [self] in
            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else {
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
                return
            }

            var tv = timeval(tv_sec: 0, tv_usec: 10_000) // 10ms timeout for responsive stop() and frame timeout
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            socket = fd
            running = true
            print("[VideoRecv] Listening on port \(OXRProtocol.videoPort)")

            self.receiveLoop(fd: fd, onNalUnit: onNalUnit)

            close(fd)
            socket = -1
            print("[VideoRecv] Stopped")
        }
        thread.qualityOfService = .userInteractive
        thread.name = "oxr.video.recv"
        thread.start()
    }

    public func stop() {
        running = false
    }

    // MARK: - Receive loop (raw memory, zero-copy hot path)

    private func receiveLoop(fd: Int32, onNalUnit: @escaping OnNalUnit) {
        let headerSize = MemoryLayout<VideoPacketHeader>.size
        let maxPacketSize = headerSize + OXRProtocol.maxPacketPayload

        // Receive buffer — stack-like, reused every iteration
        let recvBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: maxPacketSize)
        defer { recvBuf.deallocate() }

        // Frame assembly buffer — raw memory, no COW
        let maxFrameBytes = 1024 * 1024 // 1MB — more than enough for any single NAL unit
        let frameBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: maxFrameBytes)
        defer { frameBuf.deallocate() }

        // Per-packet tracking
        let maxPacketsPerGroup: Int = 1024
        let packetSizes = UnsafeMutablePointer<UInt16>.allocate(capacity: maxPacketsPerGroup)
        let packetReceived = UnsafeMutablePointer<Bool>.allocate(capacity: maxPacketsPerGroup)
        defer { packetSizes.deallocate(); packetReceived.deallocate() }

        // FEC parity tracking
        let maxFecGroups = (maxPacketsPerGroup + FEC.groupSize - 1) / FEC.groupSize
        let fecReceived = UnsafeMutablePointer<Bool>.allocate(capacity: maxFecGroups)
        let fecData = UnsafeMutablePointer<UInt8>.allocate(capacity: maxFecGroups * OXRProtocol.maxPacketPayload)
        defer { fecReceived.deallocate(); fecData.deallocate() }
        var fecGroupCount: Int = 0
        var _fecRecoveries: UInt32 = 0

        var currentFrameIndex: UInt32 = UInt32.max
        var totalExpected: UInt16 = 0
        var receivedCount: UInt16 = 0
        var frameTimestamp: Int64 = 0
        var lastGroupPacketTimeNs: Int64 = 0

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
                packetSizes[missingIdx] = UInt16(OXRProtocol.maxPacketPayload)
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
            _nalUnitsDelivered &+= 1
            _lastFrameDeliveryTimeNs = Self.monotonicNs()
            if _nalUnitsDelivered <= 10 || _nalUnitsDelivered % 200 == 0 {
                print("[VideoRecv] NAL #\(_nalUnitsDelivered) (\(finalSize) bytes, frame \(currentFrameIndex))")
            }
            deliverNalUnit(frameBuf, finalSize, frameTimestamp, onNalUnit)
            totalExpected = 0
        }

        while running {
            let n = recv(fd, recvBuf, maxPacketSize, 0)

            // Idle timeout: drop a partial frame only when no packet has arrived for >500ms.
            if n <= 0 && totalExpected > 0 && receivedCount < totalExpected && lastGroupPacketTimeNs > 0 {
                let idleMs = (Self.monotonicNs() - lastGroupPacketTimeNs) / 1_000_000
                if idleMs > 500 {
                    // Last chance: try FEC recovery before dropping
                    if tryFecRecovery() {
                        deliverFrame()
                    } else {
                        _groupsDropped &+= 1
                        totalExpected = 0
                        receivedCount = 0
                    }
                }
            }

            guard n > headerSize else { continue }

            _packetsReceived &+= 1

            // Parse header directly from raw memory (no Data, no copies)
            let header = UnsafeRawPointer(recvBuf).load(as: VideoPacketHeader.self)
            let payloadSize = min(Int(header.payloadSize), n - headerSize)

            // Render pose packet — skip (handled separately if needed)
            if header.flags & VideoFlags.renderPose != 0 {
                continue
            }

            // FEC parity packet — store separately
            if header.flags & VideoFlags.fec != 0 {
                if header.frameIndex == currentFrameIndex && totalExpected > 0 {
                    let groupIdx = Int(header.packetIndex)
                    if groupIdx < fecGroupCount && !fecReceived[groupIdx] {
                        let fecOffset = groupIdx * OXRProtocol.maxPacketPayload
                        memcpy(fecData + fecOffset, recvBuf + headerSize,
                               min(payloadSize, OXRProtocol.maxPacketPayload))
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
                                let count = min(Int(totalExpected), maxPacketsPerGroup)
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
                                _groupsDropped &+= 1
                                if _groupsDropped <= 20 || _groupsDropped % 100 == 0 {
                                    print("[VideoRecv] Dropped: got \(receivedCount)/\(totalExpected) pkts for frame \(currentFrameIndex)")
                                }
                            }
                        }
                    }
                }

                currentFrameIndex = header.frameIndex
                _totalFramesSeen &+= 1
                totalExpected = header.totalPackets
                receivedCount = 0
                frameTimestamp = header.presentationTimeNs
                lastGroupPacketTimeNs = 0

                // Zero the tracking arrays
                let count = min(Int(totalExpected), maxPacketsPerGroup)
                packetReceived.initialize(repeating: false, count: count)
                packetSizes.initialize(repeating: 0, count: count)

                // Initialize FEC tracking
                fecGroupCount = (count + FEC.groupSize - 1) / FEC.groupSize
                fecReceived.initialize(repeating: false, count: fecGroupCount)
            }

            let idx = Int(header.packetIndex)
            guard idx < Int(totalExpected), idx < maxPacketsPerGroup else { continue }
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
            _lastPacketReceivedTimeNs = nowNs

            // Frame complete — deliver
            if receivedCount == totalExpected {
                deliverFrame()
            }
        }
    }

    private func deliverNalUnit(_ buf: UnsafeMutablePointer<UInt8>, _ size: Int,
                                 _ timestamp: Int64, _ callback: OnNalUnit) {
        // Single copy from raw buffer → Data for delivery
        let data = Data(bytes: buf, count: size)
        let recvTime = Self.monotonicNs()
        callback(data, timestamp, recvTime)
    }

    private func computeFinalSize(_ packetSizes: UnsafeMutablePointer<UInt16>, _ totalExpected: Int) -> Int {
        guard totalExpected > 0 else { return 0 }
        if totalExpected == 1 {
            return Int(packetSizes[0])
        }
        return (totalExpected - 1) * OXRProtocol.maxPacketPayload + Int(packetSizes[totalExpected - 1])
    }

    public static func monotonicNs() -> Int64 {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        let ticks = mach_absolute_time()
        return Int64(ticks * UInt64(info.numer) / UInt64(info.denom))
    }
}
