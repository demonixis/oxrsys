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
    private var prefer10Bit = false
    // After a decode error, inter frames reference data we no longer have, so decoding them
    // smears corruption forward. Drop VCL slices until a keyframe (IRAP/IDR) arrives while nudging
    // the server for one — turning packet loss into a brief clean freeze instead of a green smear.
    private var awaitingKeyframe = false

    private var sliceCount: Int = 0
    private var decodeErrorCount: Int = 0
    public var totalDecodeErrors: Int { locked { decodeErrorCount } }

    /// Called on decode errors (from VT callback thread) for keyframe recovery.
    public var onDecodeError: (@Sendable () -> Void)? {
        get { locked { onDecodeErrorCallback } }
        set { locked { onDecodeErrorCallback = newValue } }
    }

    public init() {}

    /// Requests a 10-bit VideoToolbox output surface for HEVC Main10 streams.
    /// H.264 remains on the standard 8-bit output path.
    public func setPrefer10Bit(_ value: Bool) {
        locked { prefer10Bit = value }
    }

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
        if codec == .h265 {
            for nal in nalUnits {
                guard !nal.isEmpty else { continue }
                decodeH265Nal(nal, presentationTimeNs: presentationTimeNs)
            }
        } else {
            decodeH264AccessUnit(nalUnits, presentationTimeNs: presentationTimeNs)
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
            // IRAP (16-23: BLA/IDR/CRA) are independently decodable random-access points.
            let isIrap = nalType >= 16 && nalType <= 23
            if shouldDropWhileRecovering(isKeyframe: isIrap) {
                invokeDecodeErrorCallback() // keep nudging for a keyframe (cooldown rate-limits)
            } else {
                decodeAccessUnit([nal], codec: .h265, presentationTimeNs: presentationTimeNs)
            }
        default:
            break
        }
    }

    /// One decode() call carries one reassembled access unit (frame). Low-latency rate control can
    /// split a frame into MULTIPLE VCL slice NALs; VideoToolbox must receive them all in a single
    /// CMSampleBuffer, otherwise decoding each slice alone yields corrupt (green) pictures with no
    /// decode error. So collect all VCL NALs and decode them together, and make keyframe-recovery
    /// decisions per access unit.
    private func decodeH264AccessUnit(_ nalUnits: [Data], presentationTimeNs: Int64) {
        var vclNals: [Data] = []
        var hasIdrSlice = false

        for nal in nalUnits {
            guard nal.count > 1 else { continue }
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
            case 1, 2, 3, 4, 5:
                // VCL slices (non-IDR + IDR). H.264 IDR (type 5) is the random-access keyframe;
                // the access unit is a keyframe if ANY of its slices is IDR.
                if nalType == 5 { hasIdrSlice = true }
                vclNals.append(nal)
            default:
                // AUD(9), SEI(6), etc. - not video data.
                break
            }
        }

        guard !vclNals.isEmpty else { return }
        if shouldDropWhileRecovering(isKeyframe: hasIdrSlice) {
            invokeDecodeErrorCallback() // keep nudging for a keyframe (cooldown rate-limits)
        } else {
            decodeAccessUnit(vclNals, codec: .h264, presentationTimeNs: presentationTimeNs)
        }
    }

    /// While recovering from a decode error, drop inter slices until a keyframe arrives. Clears the
    /// recovering state on the keyframe so normal decoding resumes. Returns true if this slice
    /// should be dropped.
    private func shouldDropWhileRecovering(isKeyframe: Bool) -> Bool {
        locked { () -> Bool in
            guard awaitingKeyframe else { return false }
            if isKeyframe {
                awaitingKeyframe = false
                return false
            }
            return true
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
        awaitingKeyframe = false
        sliceCount = 0
        decodeErrorCount = 0
        return oldSession
    }

    /// Creates a decompression session that outputs the given Metal-compatible pixel format.
    /// Returns the session and the creation status so callers can fall back to another format.
    private func makeDecompressionSession(
        formatDescription fmt: CMFormatDescription,
        pixelFormat: OSType
    ) -> (VTDecompressionSession?, OSStatus) {
        let decoderAttrs: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:],
            kCVPixelBufferPixelFormatTypeKey as String: pixelFormat
        ]

        // Prefer the hardware decoder — always present on Apple silicon — so a real-time stream is
        // never paced by a software decoder. `Enable` (not `Require`) still permits a fallback
        // rather than failing session creation on a configuration that lacks one.
        let decoderSpec: [String: Any] = [
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: true
        ]

        var outputCallback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: decompressionCallback,
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )

        var newSession: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: fmt,
            decoderSpecification: decoderSpec as CFDictionary,
            imageBufferAttributes: decoderAttrs as CFDictionary,
            outputCallback: &outputCallback,
            decompressionSessionOut: &newSession
        )

        // Low-latency decode: tell VideoToolbox latency matters more than throughput so it does not
        // batch or hold frames. Never combine with MaximizePowerEfficiency (undefined behavior).
        if status == noErr, let session = newSession {
            VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        }
        return (newSession, status)
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

        // Prefer a 10-bit output surface for HEVC Main10, but fall back to 8-bit if the decoder
        // refuses one — e.g. an 8-bit Main stream from a server with 10-bit disabled (the default).
        // Without this fallback a rejected 10-bit request fails session creation outright → black
        // screen. The renderer picks its color conversion from the buffer's actual format, so
        // whichever surface we get displays correctly.
        //
        // The 8-bit surface is FullRange (not VideoRange): VideoToolbox range-converts to the
        // requested format, and Shaders.metal's stereoFragmentYCbCr does full-range BT.709 math
        // (luma used directly, no 16..235 -> 0..255 expansion). Requesting FullRange here makes
        // the decoded buffers match that shader; VideoRange rendered washed-out through it. Keep
        // them in sync. The 10-bit Main10 surface stays VideoRange.
        let want10Bit = locked { prefer10Bit && codec == .h265 }
        let candidateFormats: [OSType] = want10Bit
            ? [kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
               kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
            : [kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]

        var newSession: VTDecompressionSession?
        var chosenFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        var lastStatus: OSStatus = noErr
        for pixelFormat in candidateFormats {
            let (created, status) = makeDecompressionSession(formatDescription: fmt, pixelFormat: pixelFormat)
            if let created {
                newSession = created
                chosenFormat = pixelFormat
                break
            }
            lastStatus = status
            print("[VideoDecoder/\(codec.logName)] Decompression session unavailable for pixel format '\(pixelFormat)' (\(status)); trying next")
        }

        guard let newSession else {
            print("[VideoDecoder/\(codec.logName)] Failed to create decompression session: \(lastStatus)")
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
        let bitLabel = chosenFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? "10-bit" : "8-bit"
        print("[VideoDecoder/\(codec.logName)] Decoder session created - \(dim.width)x\(dim.height) (\(bitLabel))")
    }

    /// Decode one access unit made of one or more VCL slice NALs. All slices are packed into a
    /// single CMSampleBuffer (4-byte length-prefixed, AVCC) and submitted as one frame. H.265
    /// passes one NAL per call; H.264 passes a whole batched access unit.
    private func decodeAccessUnit(_ vclNals: [Data], codec: VideoCodec, presentationTimeNs: Int64) {
        guard let firstNal = vclNals.first else { return }

        let snapshot = locked { () -> (VTDecompressionSession, CMFormatDescription, Int)? in
            guard activeCodec == codec, let session, let formatDesc else {
                return nil
            }
            sliceCount += 1
            return (session, formatDesc, sliceCount)
        }
        guard let (session, formatDesc, frameNumber) = snapshot else {
            let nalType = nalTypeDescription(firstNal, codec: codec)
            let hasSeenSlices = locked { sliceCount > 0 }
            if !hasSeenSlices {
                print("[VideoDecoder/\(codec.logName)] Dropping slice - no session yet (NAL type \(nalType))")
            }
            return
        }

        var totalSize = 0
        for nal in vclNals { totalSize += 4 + nal.count }
        guard let rawBuffer = malloc(totalSize) else { return }
        let buf = rawBuffer.assumingMemoryBound(to: UInt8.self)

        var offset = 0
        for nal in vclNals {
            let len = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: len) { src in
                (buf + offset).initialize(from: src.baseAddress!.assumingMemoryBound(to: UInt8.self), count: 4)
            }
            offset += 4
            nal.copyBytes(to: buf + offset, count: nal.count)
            offset += nal.count
        }

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

        let nalType = nalTypeDescription(firstNal, codec: codec)
        if decStatus != noErr {
            let errorCount = locked { () -> Int in
                decodeErrorCount += 1
                return decodeErrorCount
            }
            if errorCount <= 5 || errorCount % 100 == 0 {
                print("[VideoDecoder/\(codec.logName)] DecodeFrame error: \(decStatus) (frame #\(frameNumber), NAL type \(nalType), \(vclNals.count) slice(s), \(totalSize) bytes)")
            }
        } else if frameNumber <= 3 || frameNumber % 200 == 0 {
            print("[VideoDecoder/\(codec.logName)] Decoded frame #\(frameNumber) - NAL type \(nalType), \(vclNals.count) slice(s), \(totalSize) bytes")
        }
    }

    private func splitNalUnits(_ data: Data) -> [Data] {
        // Scan for Annex-B start codes directly over the frame's bytes instead of first copying the
        // whole frame into a [UInt8] array. Each emitted NAL is copied out exactly once into an
        // owned, 0-based Data — so callers can index nal[0] and param sets can be retained safely —
        // which removes one full-frame heap copy and the per-byte bounds checking on the hot path.
        let count = data.count
        guard count > 0 else { return [] }

        var units = [Data]()
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            guard let base = raw.baseAddress else { return }
            var i = 0
            var nalStart = -1

            while i < count - 2 {
                let isFourByte = (i < count - 3 && raw[i] == 0 && raw[i + 1] == 0 &&
                                  raw[i + 2] == 0 && raw[i + 3] == 1)
                let isThreeByte = !isFourByte && (raw[i] == 0 && raw[i + 1] == 0 && raw[i + 2] == 1)

                if isThreeByte || isFourByte {
                    if nalStart >= 0 {
                        units.append(Data(bytes: base + nalStart, count: i - nalStart))
                    }
                    let startCodeLen = isFourByte ? 4 : 3
                    nalStart = i + startCodeLen
                    i += startCodeLen
                } else {
                    i += 1
                }
            }

            if nalStart >= 0 && nalStart < count {
                units.append(Data(bytes: base + nalStart, count: count - nalStart))
            }
        }

        // No start codes found → treat the whole buffer as a single NAL.
        if units.isEmpty {
            return [data]
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
        // Enter recovery: drop inter slices until a keyframe so corruption can't propagate.
        let callback = locked { () -> (@Sendable () -> Void)? in
            awaitingKeyframe = true
            return onDecodeErrorCallback
        }
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
