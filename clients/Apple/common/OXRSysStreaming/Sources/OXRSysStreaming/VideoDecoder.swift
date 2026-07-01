// SPDX-License-Identifier: MPL-2.0

// Hardware H.264/H.265 decoding via VideoToolbox.
// Takes reassembled NAL unit data from VideoReceiver, outputs CVPixelBuffer.

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

public final class VideoDecoder: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    private let lock = NSLock()
    fileprivate var session: VTDecompressionSession?
    fileprivate var formatDesc: CMFormatDescription?
    fileprivate var onFrame: OnFrame?
    fileprivate var onDecodeErrorCallback: (@Sendable () -> Void)?

    private var activeCodec: VideoCodec = .h265
    private var vps: Data?
    private var sps: Data?
    private var pps: Data?
    private var paramSetsReady = false

    private var sliceCount: Int = 0
    private var decodeErrorCount: Int = 0
    public var totalDecodeErrors: Int { locked { decodeErrorCount } }

    /// Called on decode errors (from VT callback thread) for keyframe recovery.
    public var onDecodeError: (@Sendable () -> Void)? {
        get { locked { onDecodeErrorCallback } }
        set { locked { onDecodeErrorCallback = newValue } }
    }

    public init() {}

    public func configure(callback: @escaping OnFrame) {
        locked { onFrame = callback }
    }

    /// Compatibility path for existing H.265-only callers.
    public func decode(nalData: Data, presentationTimeNs: Int64) {
        decode(nalData: nalData, codec: .h265, presentationTimeNs: presentationTimeNs)
    }

    /// Feed a raw H.264/H.265 byte stream (may contain multiple NAL units with start codes).
    public func decode(nalData: Data, codec: VideoCodec, presentationTimeNs: Int64) {
        guard codec == .h265 || codec == .h264 else {
            print("[VideoDecoder] Dropping unsupported codec \(codec.logName) (\(codec.rawValue))")
            invokeDecodeErrorCallback()
            return
        }

        switchCodecIfNeeded(codec)

        let nalUnits = splitNalUnits(nalData)
        for nal in nalUnits {
            guard !nal.isEmpty else { continue }
            if codec == .h265 {
                decodeH265Nal(nal, presentationTimeNs: presentationTimeNs)
            } else {
                decodeH264Nal(nal, presentationTimeNs: presentationTimeNs)
            }
        }
    }

    public func invalidate() {
        let oldSession = locked { () -> VTDecompressionSession? in
            resetDecoderStateLocked()
        }
        if let oldSession {
            VTDecompressionSessionInvalidate(oldSession)
        }
    }

    private func decodeH265Nal(_ nal: Data, presentationTimeNs: Int64) {
        guard nal.count > 2 else { return }
        let nalType = (nal[0] >> 1) & 0x3F

        switch nalType {
        case 32:
            let changed = locked { () -> Bool in
                guard vps != nal else { return false }
                vps = nal
                paramSetsReady = false
                return true
            }
            if changed { print("[VideoDecoder/H.265] Got VPS (\(nal.count) bytes)") }
        case 33:
            let changed = locked { () -> Bool in
                guard sps != nal else { return false }
                sps = nal
                paramSetsReady = false
                return true
            }
            if changed { print("[VideoDecoder/H.265] Got SPS (\(nal.count) bytes)") }
        case 34:
            let shouldCreateSession = locked { () -> Bool in
                if pps != nal {
                    pps = nal
                    paramSetsReady = false
                    return true
                }
                return !paramSetsReady
            }
            if shouldCreateSession {
                print("[VideoDecoder/H.265] Got PPS (\(nal.count) bytes)")
                tryCreateFormatDescription()
            }
        case 0...31:
            decodeSlice(nal, codec: .h265, presentationTimeNs: presentationTimeNs)
        default:
            break
        }
    }

    private func decodeH264Nal(_ nal: Data, presentationTimeNs: Int64) {
        let nalType = nal[0] & 0x1F

        switch nalType {
        case 7:
            let changed = locked { () -> Bool in
                guard sps != nal else { return false }
                sps = nal
                paramSetsReady = false
                return true
            }
            if changed { print("[VideoDecoder/H.264] Got SPS (\(nal.count) bytes)") }
        case 8:
            let shouldCreateSession = locked { () -> Bool in
                if pps != nal {
                    pps = nal
                    paramSetsReady = false
                    return true
                }
                return !paramSetsReady
            }
            if shouldCreateSession {
                print("[VideoDecoder/H.264] Got PPS (\(nal.count) bytes)")
                tryCreateFormatDescription()
            }
        case 1, 5:
            decodeSlice(nal, codec: .h264, presentationTimeNs: presentationTimeNs)
        default:
            break
        }
    }

    private func switchCodecIfNeeded(_ codec: VideoCodec) {
        let oldSession = locked { () -> VTDecompressionSession? in
            guard activeCodec != codec else { return nil }
            activeCodec = codec
            return resetDecoderStateLocked()
        }
        if let oldSession {
            VTDecompressionSessionInvalidate(oldSession)
        }
    }

    private func resetDecoderStateLocked() -> VTDecompressionSession? {
        let oldSession = session
        session = nil
        formatDesc = nil
        vps = nil
        sps = nil
        pps = nil
        paramSetsReady = false
        sliceCount = 0
        decodeErrorCount = 0
        return oldSession
    }

    private func tryCreateFormatDescription() {
        let snapshot = locked { () -> (VideoCodec, Data?, Data, Data)? in
            guard let currentSps = sps, let currentPps = pps else { return nil }
            if activeCodec == .h265 {
                guard let currentVps = vps else { return nil }
                return (activeCodec, currentVps, currentSps, currentPps)
            }
            return (activeCodec, nil, currentSps, currentPps)
        }
        guard let (codec, currentVps, currentSps, currentPps) = snapshot else { return }

        var fmt: CMFormatDescription?
        let status: OSStatus
        if codec == .h265, let currentVps {
            status = currentVps.withUnsafeBytes { vpsPtr in
                currentSps.withUnsafeBytes { spsPtr in
                    currentPps.withUnsafeBytes { ppsPtr in
                        var ptrs: [UnsafePointer<UInt8>] = [
                            vpsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                            spsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                            ppsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        ]
                        var szs = [currentVps.count, currentSps.count, currentPps.count]
                        return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                            allocator: kCFAllocatorDefault,
                            parameterSetCount: 3,
                            parameterSetPointers: &ptrs,
                            parameterSetSizes: &szs,
                            nalUnitHeaderLength: 4,
                            extensions: nil,
                            formatDescriptionOut: &fmt
                        )
                    }
                }
            }
        } else {
            status = currentSps.withUnsafeBytes { spsPtr in
                currentPps.withUnsafeBytes { ppsPtr in
                    var ptrs: [UnsafePointer<UInt8>] = [
                        spsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        ppsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                    ]
                    var szs = [currentSps.count, currentPps.count]
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
        }

        guard status == noErr, let fmt else {
            print("[VideoDecoder/\(codec.logName)] Failed to create format description: \(status)")
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
        if canKeepExistingSession {
            return
        }

        let decoderAttrs: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]

        var outputCallback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: decompressionCallback,
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
            print("[VideoDecoder/\(codec.logName)] Failed to create decompression session: \(sessionStatus)")
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
        if let oldSession {
            VTDecompressionSessionInvalidate(oldSession)
        }

        let dim = CMVideoFormatDescriptionGetDimensions(fmt)
        print("[VideoDecoder/\(codec.logName)] Decoder session created - \(dim.width)x\(dim.height)")
    }

    private func decodeSlice(_ nalUnit: Data, codec: VideoCodec, presentationTimeNs: Int64) {
        let snapshot = locked { () -> (VTDecompressionSession, CMFormatDescription, Int)? in
            guard activeCodec == codec, let session, let formatDesc else {
                return nil
            }
            sliceCount += 1
            return (session, formatDesc, sliceCount)
        }
        guard let (session, formatDesc, sliceNumber) = snapshot else {
            let nalType = nalTypeDescription(nalUnit, codec: codec)
            let hasSeenSlices = locked { sliceCount > 0 }
            if !hasSeenSlices {
                print("[VideoDecoder/\(codec.logName)] Dropping slice - no session yet (NAL type \(nalType))")
            }
            return
        }

        let totalSize = 4 + nalUnit.count
        guard let rawBuffer = malloc(totalSize) else { return }
        let buf = rawBuffer.assumingMemoryBound(to: UInt8.self)

        let len = UInt32(nalUnit.count).bigEndian
        withUnsafeBytes(of: len) { src in
            buf.initialize(from: src.baseAddress!.assumingMemoryBound(to: UInt8.self), count: 4)
        }
        nalUnit.copyBytes(to: buf + 4, count: nalUnit.count)

        var blockBuffer: CMBlockBuffer?
        let bbStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: rawBuffer,
            blockLength: totalSize,
            blockAllocator: kCFAllocatorMalloc,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: totalSize,
            flags: 0,
            blockBufferOut: &blockBuffer
        )

        guard bbStatus == kCMBlockBufferNoErr, let blockBuffer else {
            free(rawBuffer)
            return
        }

        let pts = CMTime(value: Int64(presentationTimeNs), timescale: 1_000_000_000)
        var timingInfo = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: pts,
            decodeTimeStamp: .invalid
        )

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
            session,
            sampleBuffer: sampleBuffer,
            flags: decodeFlags,
            frameRefcon: nil,
            infoFlagsOut: &infoFlags
        )

        let nalType = nalTypeDescription(nalUnit, codec: codec)
        if decStatus != noErr {
            let errorCount = locked { () -> Int in
                decodeErrorCount += 1
                return decodeErrorCount
            }
            if errorCount <= 5 || errorCount % 100 == 0 {
                print("[VideoDecoder/\(codec.logName)] DecodeFrame error: \(decStatus) (slice #\(sliceNumber), NAL type \(nalType), \(nalUnit.count) bytes)")
            }
        } else if sliceNumber <= 3 || sliceNumber % 200 == 0 {
            print("[VideoDecoder/\(codec.logName)] Decoded slice #\(sliceNumber) - NAL type \(nalType), \(nalUnit.count) bytes")
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
                if nalStart >= 0 {
                    units.append(Data(bytes[nalStart..<i]))
                }
                let startCodeLen = isFourByte ? 4 : 3
                nalStart = i + startCodeLen
                i += startCodeLen
            } else {
                i += 1
            }
        }

        if nalStart >= 0 && nalStart < count {
            units.append(Data(bytes[nalStart..<count]))
        }
        if units.isEmpty && !data.isEmpty {
            units.append(data)
        }
        return units
    }

    private func nalTypeDescription(_ nal: Data, codec: VideoCodec) -> String {
        guard let first = nal.first else { return "empty" }
        if codec == .h264 {
            return String(first & 0x1F)
        }
        return String((first >> 1) & 0x3F)
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
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

public typealias H265Decoder = VideoDecoder

private extension VideoCodec {
    var logName: String {
        switch self {
        case .h264:
            return "H.264"
        case .av1:
            return "AV1"
        case .h265:
            return "H.265"
        }
    }
}

private func decompressionCallback(
    decompressionOutputRefCon: UnsafeMutableRawPointer?,
    sourceFrameRefCon: UnsafeMutableRawPointer?,
    status: OSStatus,
    infoFlags: VTDecodeInfoFlags,
    imageBuffer: CVImageBuffer?,
    presentationTimeStamp: CMTime,
    presentationDuration: CMTime
) {
    guard let refCon = decompressionOutputRefCon else { return }
    let decoder = Unmanaged<VideoDecoder>.fromOpaque(refCon).takeUnretainedValue()

    guard status == noErr, let pixelBuffer = imageBuffer else {
        if status != noErr {
            decoder.invokeDecodeErrorCallback()
        }
        return
    }

    decoder.invokeFrameCallback(pixelBuffer: pixelBuffer, presentationTime: presentationTimeStamp)
}
