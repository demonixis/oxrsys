// SPDX-License-Identifier: MPL-2.0

// H265Decoder.swift — Hardware H.265 (HEVC) decoding via VideoToolbox.
// Takes reassembled NAL unit data from VideoReceiver, outputs CVPixelBuffer.

import CoreMedia
import CoreVideo
import VideoToolbox

public final class H265Decoder: Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    fileprivate nonisolated(unsafe) var session: VTDecompressionSession?
    fileprivate nonisolated(unsafe) var formatDesc: CMFormatDescription?
    fileprivate nonisolated(unsafe) var onFrame: OnFrame?
    fileprivate nonisolated(unsafe) var onDecodeErrorCallback: (@Sendable () -> Void)?

    /// Called on decode errors (from VT callback thread) for keyframe recovery.
    public var onDecodeError: (@Sendable () -> Void)? {
        get { onDecodeErrorCallback }
        set { onDecodeErrorCallback = newValue }
    }

    // Cached parameter sets for HEVC: VPS, SPS, PPS
    private nonisolated(unsafe) var vps: Data?
    private nonisolated(unsafe) var sps: Data?
    private nonisolated(unsafe) var pps: Data?
    private nonisolated(unsafe) var paramSetsReady = false

    private nonisolated(unsafe) var sliceCount: Int = 0
    private nonisolated(unsafe) var decodeErrorCount: Int = 0
    public var totalDecodeErrors: Int { decodeErrorCount }

    public init() {}

    public func configure(callback: @escaping OnFrame) {
        onFrame = callback
    }

    /// Feed a raw H.265 byte stream (may contain multiple NAL units with start codes).
    public func decode(nalData: Data, presentationTimeNs: Int64) {
        let nalUnits = splitNalUnits(nalData)

        for nal in nalUnits {
            guard nal.count > 2 else { continue }

            // HEVC NAL unit type is in bits 1-6 of the first byte
            let nalType = (nal[0] >> 1) & 0x3F

            switch nalType {
            case 32: // VPS
                if vps != nal {
                    vps = nal
                    paramSetsReady = false
                    print("[H265] Got VPS (\(nal.count) bytes)")
                }
            case 33: // SPS
                if sps != nal {
                    sps = nal
                    paramSetsReady = false
                    print("[H265] Got SPS (\(nal.count) bytes)")
                }
            case 34: // PPS
                if pps != nal {
                    pps = nal
                    paramSetsReady = false
                    print("[H265] Got PPS (\(nal.count) bytes)")
                }
                // Only try to create session after PPS (last of the three)
                if !paramSetsReady {
                    tryCreateFormatDescription()
                }
            case 0...31: // VCL NAL units — actual video slices
                decodeSlice(nal, presentationTimeNs: presentationTimeNs)
            default:
                // NAL types 35+ are non-VCL: AUD(35), EOS(36), EOB(37), FD(38), SEI(39,40), etc.
                // Skip them — they are not video data and will cause decode errors.
                break
            }
        }
    }

    public func invalidate() {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        formatDesc = nil
        vps = nil
        sps = nil
        pps = nil
        paramSetsReady = false
    }

    // MARK: - Private

    private func tryCreateFormatDescription() {
        guard let vps, let sps, let pps else { return }

        // Build new format description to see if it matches the current one
        var fmt: CMFormatDescription?

        let status = vps.withUnsafeBytes { vpsPtr in
            sps.withUnsafeBytes { spsPtr in
                pps.withUnsafeBytes { ppsPtr in
                    var ptrs: [UnsafePointer<UInt8>] = [
                        vpsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        spsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        ppsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                    ]
                    var szs = [vps.count, sps.count, pps.count]
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

        guard status == noErr, let fmt else {
            print("[H265] Failed to create format description: \(status)")
            return
        }

        // If we already have a session with the same format, keep it
        if let existingFmt = formatDesc, session != nil,
           CMFormatDescriptionEqual(existingFmt, otherFormatDescription: fmt) {
            paramSetsReady = true
            return
        }

        // Destroy old session before creating new one
        if let oldSession = session {
            VTDecompressionSessionInvalidate(oldSession)
            session = nil
        }
        formatDesc = fmt

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
            print("[H265] Failed to create decompression session: \(sessionStatus)")
            return
        }
        session = newSession
        paramSetsReady = true
        sliceCount = 0
        decodeErrorCount = 0

        let dim = CMVideoFormatDescriptionGetDimensions(fmt)
        print("[H265] Decoder session created — \(dim.width)x\(dim.height)")
    }

    private func decodeSlice(_ nalUnit: Data, presentationTimeNs: Int64) {
        guard let session, let formatDesc else {
            let nalType = (nalUnit[0] >> 1) & 0x3F
            if sliceCount == 0 {
                print("[H265] Dropping slice — no session yet (NAL type \(nalType))")
            }
            return
        }

        sliceCount += 1

        // Build AVCC-format buffer: 4-byte big-endian length + NAL body.
        // The buffer must outlive the async decode, so we allocate with malloc
        // and let CoreMedia own it via kCFAllocatorMalloc.
        let totalSize = 4 + nalUnit.count
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: totalSize)

        // Write 4-byte big-endian NAL length
        let len = UInt32(nalUnit.count).bigEndian
        withUnsafeBytes(of: len) { src in
            buf.initialize(from: src.baseAddress!.assumingMemoryBound(to: UInt8.self), count: 4)
        }

        // Copy NAL body
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
            buf.deallocate()
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

        if decStatus != noErr {
            decodeErrorCount += 1
            if decodeErrorCount <= 5 || decodeErrorCount % 100 == 0 {
                let nalType = (nalUnit[0] >> 1) & 0x3F
                print("[H265] DecodeFrame error: \(decStatus) (slice #\(sliceCount), NAL type \(nalType), \(nalUnit.count) bytes)")
            }
        } else if sliceCount <= 3 || sliceCount % 200 == 0 {
            let nalType = (nalUnit[0] >> 1) & 0x3F
            print("[H265] Decoded slice #\(sliceCount) — NAL type \(nalType), \(nalUnit.count) bytes")
        }
    }

    /// Split a byte stream into individual NAL units by scanning for start codes.
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

        // If no start codes found, treat entire data as a single NAL
        if units.isEmpty && !data.isEmpty {
            units.append(data)
        }

        return units
    }
}

// VideoToolbox callback — called on an internal VT thread
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
    let decoder = Unmanaged<H265Decoder>.fromOpaque(refCon).takeUnretainedValue()

    guard status == noErr, let pixelBuffer = imageBuffer else {
        if status != noErr {
            decoder.onDecodeErrorCallback?()
        }
        return
    }

    decoder.onFrame?(pixelBuffer, presentationTimeStamp)
}
