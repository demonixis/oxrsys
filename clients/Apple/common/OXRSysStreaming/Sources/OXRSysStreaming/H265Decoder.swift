// SPDX-License-Identifier: MPL-2.0

// H265Decoder.swift — Hardware H.265 (HEVC) decoding via VideoToolbox.
// Takes reassembled NAL unit data from VideoReceiver, outputs CVPixelBuffer.
// Shares its VideoToolbox plumbing with H264Decoder via VTDecoderBase; only the
// parameter-set layout (VPS/SPS/PPS) and the HEVC format-description constructor
// are codec-specific. Each VCL NAL is decoded as its own access unit, matching
// the prior per-slice behavior.

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

public final class H265Decoder: VTDecoderBase, @unchecked Sendable {
    // Cached parameter sets for HEVC: VPS, SPS, PPS
    private var vps: Data?
    private var sps: Data?
    private var pps: Data?
    private var paramSetsReady = false

    public init() { super.init(tag: "H265") }

    /// Feed a raw H.265 byte stream (may contain multiple NAL units with start codes).
    public func decode(nalData: Data, presentationTimeNs: Int64) {
        let nalUnits = splitNalUnits(nalData)

        for nal in nalUnits {
            guard nal.count > 2 else { continue }

            // HEVC NAL unit type is in bits 1-6 of the first byte
            let nalType = (nal[0] >> 1) & 0x3F

            switch nalType {
            case 32: // VPS
                let changed = locked { () -> Bool in
                    guard vps != nal else { return false }
                    vps = nal
                    paramSetsReady = false
                    return true
                }
                if changed { print("[H265] Got VPS (\(nal.count) bytes)") }
            case 33: // SPS
                let changed = locked { () -> Bool in
                    guard sps != nal else { return false }
                    sps = nal
                    paramSetsReady = false
                    return true
                }
                if changed { print("[H265] Got SPS (\(nal.count) bytes)") }
            case 34: // PPS
                let shouldCreate = locked { () -> Bool in
                    if pps != nal {
                        pps = nal
                        paramSetsReady = false
                        return true
                    }
                    return !paramSetsReady
                }
                if shouldCreate {
                    print("[H265] Got PPS (\(nal.count) bytes)")
                    // Only try to create session after PPS (last of the three).
                    tryCreateFormatDescription()
                }
            case 0...31: // VCL NAL units — actual video slices
                decodeAccessUnit([nal], presentationTimeNs: presentationTimeNs)
            default:
                // NAL types 35+ are non-VCL: AUD(35), EOS(36), EOB(37), FD(38), SEI(39,40), etc.
                // Skip them — they are not video data and will cause decode errors.
                break
            }
        }
    }

    override func resetParameterSetsLocked() {
        vps = nil
        sps = nil
        pps = nil
        paramSetsReady = false
    }

    // MARK: - Private

    private func tryCreateFormatDescription() {
        let parameterSets = locked { () -> (Data, Data, Data)? in
            guard let currentVps = self.vps,
                  let currentSps = self.sps,
                  let currentPps = self.pps else { return nil }
            return (currentVps, currentSps, currentPps)
        }
        guard let (vps, sps, pps) = parameterSets else { return }

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

        if installFormatDescription(fmt) {
            locked { paramSetsReady = true }
        }
    }
}
