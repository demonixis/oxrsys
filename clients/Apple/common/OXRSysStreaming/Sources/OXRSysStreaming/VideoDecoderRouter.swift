// SPDX-License-Identifier: MPL-2.0

// VideoDecoderRouter.swift — routes a NAL stream to the H.265 or H.264 decoder.
// The macOS simulator historically decoded only H.265, but oxrsys streams H.264 on the
// CrossOver/Rosetta path (no HEVC hardware encode under Rosetta). The decoder is chosen
// from the server-stamped codec byte in every VideoPacketHeader (the same byte the
// Android client routes on) — not sniffed — so both native (H.265) and Wine (H.264) work.

import CoreMedia
import CoreVideo
import Foundation

public final class VideoDecoderRouter: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    private let h265 = H265Decoder()
    private let h264 = H264Decoder()

    public init() {}

    public func configure(callback: @escaping OnFrame) {
        h265.configure(callback: callback)
        h264.configure(callback: callback)
    }

    public var onDecodeError: (@Sendable () -> Void)? {
        didSet {
            h265.onDecodeError = onDecodeError
            h264.onDecodeError = onDecodeError
        }
    }

    public var totalDecodeErrors: Int { h265.totalDecodeErrors + h264.totalDecodeErrors }

    /// Route by the wire codec byte (VideoCodec cast to u8: 0 = H.265, 1 = H.264).
    /// Anything other than H.264 defaults to H.265, matching the server default.
    public func decode(nalData: Data, presentationTimeNs: Int64, codec: UInt8) {
        if codec == UInt8(VideoCodec.h264.rawValue) {
            h264.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        } else {
            h265.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        }
    }

    public func invalidate() {
        h265.invalidate()
        h264.invalidate()
    }
}
