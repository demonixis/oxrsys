// SPDX-License-Identifier: MPL-2.0

// H264Decoder.swift — Hardware H.264 (AVC) decoding via VideoToolbox.
// Mirror of H265Decoder for the CrossOver/Rosetta path: oxrsys must encode H.264
// there because HEVC hardware encode is unavailable under Rosetta 2.

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

public final class H264Decoder: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    private let lock = NSLock()
    fileprivate var session: VTDecompressionSession?
    fileprivate var formatDesc: CMFormatDescription?
    fileprivate var onFrame: OnFrame?
    fileprivate var onDecodeErrorCallback: (@Sendable () -> Void)?

    public var onDecodeError: (@Sendable () -> Void)? {
        get { locked { onDecodeErrorCallback } }
        set { locked { onDecodeErrorCallback = newValue } }
    }

    // H.264 parameter sets: SPS (7), PPS (8). No VPS.
    private var sps: Data?
    private var pps: Data?
    private var paramSetsReady = false

    private var sliceCount: Int = 0
    private var decodeErrorCount: Int = 0
    public var totalDecodeErrors: Int { locked { decodeErrorCount } }

    public init() {}

    public func configure(callback: @escaping OnFrame) {
        locked { onFrame = callback }
    }

    /// Feed a raw H.264 byte stream (may contain multiple NAL units with start codes).
    public func decode(nalData: Data, presentationTimeNs: Int64) {
        let nalUnits = splitNalUnits(nalData)

        for nal in nalUnits {
            guard nal.count > 1 else { continue }

            // H.264 NAL unit type is bits 0-4 of the first byte.
            let nalType = nal[0] & 0x1F

            switch nalType {
            case 7: // SPS
                let changed = locked { () -> Bool in
                    guard sps != nal else { return false }
                    sps = nal
                    paramSetsReady = false
                    return true
                }
                if changed { print("[H264] Got SPS (\(nal.count) bytes)") }
            case 8: // PPS
                let shouldCreate = locked { () -> Bool in
                    if pps != nal {
                        pps = nal
                        paramSetsReady = false
                        return true
                    }
                    return !paramSetsReady
                }
                if shouldCreate {
                    print("[H264] Got PPS (\(nal.count) bytes)")
                    tryCreateFormatDescription()
                }
            case 1, 2, 3, 4, 5: // VCL slices (non-IDR + IDR)
                decodeSlice(nal, presentationTimeNs: presentationTimeNs)
            default:
                // AUD(9), SEI(6), etc. — not video data.
                break
            }
        }
    }

    public func invalidate() {
        let oldSession = locked { () -> VTDecompressionSession? in
            let oldSession = session
            session = nil
            formatDesc = nil
            sps = nil
            pps = nil
            paramSetsReady = false
            return oldSession
        }
        if let oldSession { VTDecompressionSessionInvalidate(oldSession) }
    }

    // MARK: - Private

    private func tryCreateFormatDescription() {
        let parameterSets = locked { () -> (Data, Data)? in
            guard let currentSps = self.sps, let currentPps = self.pps else { return nil }
            return (currentSps, currentPps)
        }
        guard let (sps, pps) = parameterSets else { return }

        var fmt: CMFormatDescription?
        let status = sps.withUnsafeBytes { spsPtr in
            pps.withUnsafeBytes { ppsPtr in
                var ptrs: [UnsafePointer<UInt8>] = [
                    spsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                    ppsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                ]
                var szs = [sps.count, pps.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: 2,
                    parameterSetPointers: &ptrs,
                    parameterSetSizes: &szs,
                    nalUnitHeaderLength: 4,
                    formatDescriptionOut: &fmt
                )
            }
        }

        guard status == noErr, let fmt else {
            print("[H264] Failed to create format description: \(status)")
            return
        }

        let canKeepExistingSession = locked { () -> Bool in
            if let existingFmt = formatDesc, session != nil,
               CMFormatDescriptionEqual(existingFmt, otherFormatDescription: fmt) {
                paramSetsReady = true
                return true
            }
            return false
        }
        if canKeepExistingSession { return }

        let decoderAttrs: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]

        var outputCallback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: h264DecompressionCallback,
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )

        var newSession: VTDecompressionSession?
        let sessionStatus = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: fmt,
            decoderSpecification: nil,
            imageBufferAttributes: decoderAttrs as CFDictionary,
            outputCallback: &outputCallback,
            decompressionSessionOut: &newSession
        )

        guard sessionStatus == noErr, let newSession else {
            print("[H264] Failed to create decompression session: \(sessionStatus)")
            return
        }
        let oldSession = locked { () -> VTDecompressionSession? in
            let oldSession = session
            session = newSession
            formatDesc = fmt
            paramSetsReady = true
            sliceCount = 0
            decodeErrorCount = 0
            return oldSession
        }
        if let oldSession { VTDecompressionSessionInvalidate(oldSession) }

        let dim = CMVideoFormatDescriptionGetDimensions(fmt)
        print("[H264] Decoder session created — \(dim.width)x\(dim.height)")
    }

    private func decodeSlice(_ nalUnit: Data, presentationTimeNs: Int64) {
        let snapshot = locked { () -> (VTDecompressionSession, CMFormatDescription, Int)? in
            guard let session, let formatDesc else { return nil }
            sliceCount += 1
            return (session, formatDesc, sliceCount)
        }
        guard let (session, formatDesc, sliceNumber) = snapshot else { return }

        let totalSize = 4 + nalUnit.count
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: totalSize)
        let len = UInt32(nalUnit.count).bigEndian
        withUnsafeBytes(of: len) { src in
            buf.initialize(from: src.baseAddress!.assumingMemoryBound(to: UInt8.self), count: 4)
        }
        nalUnit.copyBytes(to: buf + 4, count: nalUnit.count)

        var blockBuffer: CMBlockBuffer?
        let bbStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: buf,
            blockLength: totalSize,
            blockAllocator: kCFAllocatorMalloc,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: totalSize,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard bbStatus == kCMBlockBufferNoErr, let blockBuffer else {
            buf.deallocate(); return
        }

        let pts = CMTime(value: Int64(presentationTimeNs), timescale: 1_000_000_000)
        var timingInfo = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: pts, decodeTimeStamp: .invalid)
        var sampleSize = totalSize
        var sampleBuffer: CMSampleBuffer?
        let sbStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDesc,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timingInfo,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard sbStatus == noErr, let sampleBuffer else { return }

        let decodeFlags: VTDecodeFrameFlags = [._EnableAsynchronousDecompression]
        var infoFlags: VTDecodeInfoFlags = []
        let decStatus = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sampleBuffer, flags: decodeFlags,
            frameRefcon: nil, infoFlagsOut: &infoFlags
        )
        if decStatus != noErr {
            let errorCount = locked { () -> Int in decodeErrorCount += 1; return decodeErrorCount }
            if errorCount <= 5 || errorCount % 100 == 0 {
                print("[H264] DecodeFrame error: \(decStatus) (slice #\(sliceNumber), \(nalUnit.count) bytes)")
            }
        } else if sliceNumber <= 3 || sliceNumber % 200 == 0 {
            print("[H264] Decoded slice #\(sliceNumber) — \(nalUnit.count) bytes")
        }
    }

    private func splitNalUnits(_ data: Data) -> [Data] {
        var units = [Data]()
        let bytes = [UInt8](data)
        let count = bytes.count
        var i = 0
        var nalStart = -1
        while i < count - 2 {
            let isFourByte = (i < count - 3 && bytes[i] == 0 && bytes[i + 1] == 0 &&
                              bytes[i + 2] == 0 && bytes[i + 3] == 1)
            let isThreeByte = !isFourByte && (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
            if isThreeByte || isFourByte {
                if nalStart >= 0 { units.append(Data(bytes[nalStart..<i])) }
                let startCodeLen = isFourByte ? 4 : 3
                nalStart = i + startCodeLen
                i += startCodeLen
            } else {
                i += 1
            }
        }
        if nalStart >= 0 && nalStart < count { units.append(Data(bytes[nalStart..<count])) }
        if units.isEmpty && !data.isEmpty { units.append(data) }
        return units
    }

    fileprivate func invokeDecodeErrorCallback() {
        let callback = locked { onDecodeErrorCallback }
        callback?()
    }
    fileprivate func invokeFrameCallback(pixelBuffer: CVPixelBuffer, presentationTime: CMTime) {
        let callback = locked { onFrame }
        callback?(pixelBuffer, presentationTime)
    }

    @discardableResult
    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock(); defer { lock.unlock() }
        return try body()
    }
}

private func h264DecompressionCallback(
    decompressionOutputRefCon: UnsafeMutableRawPointer?,
    sourceFrameRefCon: UnsafeMutableRawPointer?,
    status: OSStatus,
    infoFlags: VTDecodeInfoFlags,
    imageBuffer: CVImageBuffer?,
    presentationTimeStamp: CMTime,
    presentationDuration: CMTime
) {
    guard let refCon = decompressionOutputRefCon else { return }
    let decoder = Unmanaged<H264Decoder>.fromOpaque(refCon).takeUnretainedValue()
    guard status == noErr, let pixelBuffer = imageBuffer else {
        if status != noErr { decoder.invokeDecodeErrorCallback() }
        return
    }
    decoder.invokeFrameCallback(pixelBuffer: pixelBuffer, presentationTime: presentationTimeStamp)
}
