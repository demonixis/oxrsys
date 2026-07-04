// SPDX-License-Identifier: MPL-2.0

// VTDecoderBase.swift — shared VideoToolbox decode plumbing for the H.264 (AVC)
// and H.265 (HEVC) decoders. The codecs differ only in their parameter-set NAL
// layout and format-description constructor; everything else — the NSLock-guarded
// state, start-code splitting, decompression-session creation, AVCC packing, and
// the async decode callback — is shared here.
//
// One deliberate behavior change came with the shared path: it requests a
// FullRange output pixel format (see the decoderAttrs comment below), whereas the
// old standalone H.265 decoder requested VideoRange. This is not a no-op copy of
// the previous plumbing — it fixes a pre-existing range/shader mismatch.
//
// Subclasses implement `decode(nalData:presentationTimeNs:)`: they classify NALs
// with their own bit layout, cache parameter sets, build a CMFormatDescription and
// hand it to `installFormatDescription(_:)`, then feed VCL NALs to
// `decodeAccessUnit(_:presentationTimeNs:)` (one NAL at a time for HEVC, or a whole
// batched access unit for AVC).

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

public class VTDecoderBase: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    let lock = NSLock()
    var session: VTDecompressionSession?
    var formatDesc: CMFormatDescription?
    var onFrame: OnFrame?
    var onDecodeErrorCallback: (@Sendable () -> Void)?

    var sliceCount: Int = 0
    var decodeErrorCount: Int = 0

    /// Log prefix, e.g. "H264" / "H265".
    let tag: String

    init(tag: String) { self.tag = tag }

    /// Called on decode errors (from VT callback thread) for keyframe recovery.
    public var onDecodeError: (@Sendable () -> Void)? {
        get { locked { onDecodeErrorCallback } }
        set { locked { onDecodeErrorCallback = newValue } }
    }

    public var totalDecodeErrors: Int { locked { decodeErrorCount } }

    public func configure(callback: @escaping OnFrame) {
        locked { onFrame = callback }
    }

    public func invalidate() {
        let oldSession = locked { () -> VTDecompressionSession? in
            let old = session
            session = nil
            formatDesc = nil
            resetParameterSetsLocked()
            return old
        }
        if let oldSession { VTDecompressionSessionInvalidate(oldSession) }
    }

    // MARK: - Subclass hooks

    /// Clear cached parameter sets. Called with `lock` held.
    func resetParameterSetsLocked() {}

    // MARK: - Shared session management

    /// Install a freshly built format description as the active decode session.
    /// Reuses the existing session if the format is unchanged. Returns true when a
    /// usable session exists afterwards.
    @discardableResult
    func installFormatDescription(_ fmt: CMFormatDescription) -> Bool {
        let canKeepExistingSession = locked { () -> Bool in
            if let existingFmt = formatDesc, session != nil,
               CMFormatDescriptionEqual(existingFmt, otherFormatDescription: fmt) {
                return true
            }
            return false
        }
        if canKeepExistingSession { return true }

        // FullRange (not VideoRange): VideoToolbox range-converts to the requested
        // format, and Shaders.metal's stereoFragmentYCbCr does full-range BT.709 math
        // (luma used directly, no 16..235 -> 0..255 expansion). Requesting FullRange
        // here makes the decoded buffers match that shader. The old standalone H.265
        // decoder requested VideoRange, which rendered washed-out through the same
        // shader; this pairing fixes that pre-existing mismatch. Keep them in sync.
        let decoderAttrs: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        ]

        var outputCallback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: vtDecoderOutputCallback,
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
            print("[\(tag)] Failed to create decompression session: \(sessionStatus)")
            return false
        }
        let oldSession = locked { () -> VTDecompressionSession? in
            let old = session
            session = newSession
            formatDesc = fmt
            sliceCount = 0
            decodeErrorCount = 0
            return old
        }
        if let oldSession { VTDecompressionSessionInvalidate(oldSession) }

        let dim = CMVideoFormatDescriptionGetDimensions(fmt)
        print("[\(tag)] Decoder session created — \(dim.width)x\(dim.height)")
        return true
    }

    /// Decode one access unit made of one or more VCL slice NALs. All slices are
    /// packed into a single CMSampleBuffer (4-byte length-prefixed, AVCC) and
    /// submitted as one frame. HEVC passes one NAL per call; AVC passes a batch.
    func decodeAccessUnit(_ vclNals: [Data], presentationTimeNs: Int64) {
        guard !vclNals.isEmpty else { return }

        let snapshot = locked { () -> (VTDecompressionSession, CMFormatDescription, Int)? in
            guard let session, let formatDesc else { return nil }
            sliceCount += 1
            return (session, formatDesc, sliceCount)
        }
        guard let (session, formatDesc, frameNumber) = snapshot else { return }

        // Build AVCC-format buffer: per NAL a 4-byte big-endian length + body.
        // The buffer must outlive the async decode, so we allocate with malloc
        // and let CoreMedia own it via kCFAllocatorMalloc.
        var totalSize = 0
        for nal in vclNals { totalSize += 4 + nal.count }
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: totalSize)
        var offset = 0
        for nal in vclNals {
            let len = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: len) { src in
                (buf + offset).update(from: src.baseAddress!.assumingMemoryBound(to: UInt8.self), count: 4)
            }
            offset += 4
            nal.copyBytes(to: buf + offset, count: nal.count)
            offset += nal.count
        }

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
                print("[\(tag)] DecodeFrame error: \(decStatus) (frame #\(frameNumber), \(vclNals.count) slice(s), \(totalSize) bytes)")
            }
        } else if frameNumber <= 3 || frameNumber % 200 == 0 {
            print("[\(tag)] Decoded frame #\(frameNumber) — \(vclNals.count) slice(s), \(totalSize) bytes")
        }
    }

    /// Split a byte stream into individual NAL units by scanning for start codes.
    func splitNalUnits(_ data: Data) -> [Data] {
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

    func invokeDecodeErrorCallback() {
        let callback = locked { onDecodeErrorCallback }
        callback?()
    }

    func invokeFrameCallback(pixelBuffer: CVPixelBuffer, presentationTime: CMTime) {
        let callback = locked { onFrame }
        callback?(pixelBuffer, presentationTime)
    }

    @discardableResult
    func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

// VideoToolbox callback — called on an internal VT thread. The refCon is the
// concrete decoder (an `Unmanaged.passUnretained(self)`); casting to the base
// class is valid since every decoder IS-A VTDecoderBase.
private func vtDecoderOutputCallback(
    decompressionOutputRefCon: UnsafeMutableRawPointer?,
    sourceFrameRefCon: UnsafeMutableRawPointer?,
    status: OSStatus,
    infoFlags: VTDecodeInfoFlags,
    imageBuffer: CVImageBuffer?,
    presentationTimeStamp: CMTime,
    presentationDuration: CMTime
) {
    guard let refCon = decompressionOutputRefCon else { return }
    let decoder = Unmanaged<VTDecoderBase>.fromOpaque(refCon).takeUnretainedValue()

    guard status == noErr, let pixelBuffer = imageBuffer else {
        if status != noErr { decoder.invokeDecodeErrorCallback() }
        return
    }
    decoder.invokeFrameCallback(pixelBuffer: pixelBuffer, presentationTime: presentationTimeStamp)
}
