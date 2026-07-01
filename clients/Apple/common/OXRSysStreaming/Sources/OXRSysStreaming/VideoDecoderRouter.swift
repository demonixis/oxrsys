// SPDX-License-Identifier: MPL-2.0

// VideoDecoderRouter.swift — routes a NAL stream to the H.265 or H.264 decoder.
// The macOS simulator historically decoded only H.265, but oxrsys streams H.264 on the
// CrossOver/Rosetta path (no HEVC hardware encode under Rosetta). This picks the decoder
// by sniffing the parameter-set NAL types, so both native (H.265) and Wine (H.264) work.

import CoreMedia
import CoreVideo
import Foundation

public final class VideoDecoderRouter: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    private let h265 = H265Decoder()
    private let h264 = H264Decoder()
    private let lock = NSLock()
    private var codec: Int = 0 // 0 = unknown, 264, 265

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

    public func decode(nalData: Data, presentationTimeNs: Int64) {
        detectCodecIfNeeded(nalData)
        let c = lock.withLock { codec }
        if c == 264 {
            h264.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        } else {
            // Default to H.265 until proven otherwise (preserves prior behavior).
            h265.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        }
    }

    public func invalidate() {
        h265.invalidate()
        h264.invalidate()
        lock.withLock { codec = 0 }
    }

    // MARK: - Private

    private func detectCodecIfNeeded(_ data: Data) {
        if lock.withLock({ codec }) != 0 { return }
        let bytes = [UInt8](data)
        let n = bytes.count
        var i = 0
        while i < n - 3 {
            let four = bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 0 && bytes[i+3] == 1
            let three = !four && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1
            if four || three {
                let hdr = i + (four ? 4 : 3)
                if hdr < n {
                    let b = bytes[hdr]
                    let h264Type = b & 0x1F              // 7=SPS, 8=PPS
                    let h265Type = (b >> 1) & 0x3F       // 32=VPS,33=SPS,34=PPS
                    if h265Type == 32 || h265Type == 33 || h265Type == 34 {
                        lock.withLock { codec = 265 }; print("[Router] codec = H.265"); return
                    }
                    if h264Type == 7 || h264Type == 8 {
                        lock.withLock { codec = 264 }; print("[Router] codec = H.264"); return
                    }
                }
                i = hdr
            } else {
                i += 1
            }
        }
    }
}

private extension NSLock {
    func withLock<T>(_ body: () -> T) -> T {
        lock(); defer { unlock() }
        return body()
    }
}
