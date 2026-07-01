// SPDX-License-Identifier: MPL-2.0

// H264Decoder.swift — Hardware H.264 (AVC) decoding via VideoToolbox.
// For the CrossOver/Rosetta path: oxrsys must encode H.264 there because HEVC
// hardware encode is unavailable under Rosetta 2. Shares its VideoToolbox
// plumbing with H265Decoder via VTDecoderBase; only the parameter-set layout
// (SPS/PPS, no VPS) and the H.264 format-description constructor are codec-specific.

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

public final class H264Decoder: VTDecoderBase, @unchecked Sendable {
    // H.264 parameter sets: SPS (7), PPS (8). No VPS.
    private var sps: Data?
    private var pps: Data?
    private var paramSetsReady = false

    public init() { super.init(tag: "H264") }

    /// Feed a raw H.264 byte stream (may contain multiple NAL units with start codes).
    /// One call carries one reassembled access unit (frame). Low-latency rate control can
    /// split a frame into MULTIPLE VCL slice NALs; VideoToolbox must receive them all in a
    /// single CMSampleBuffer, otherwise decoding each slice alone yields corrupt (green)
    /// pictures with no decode error. So collect all VCL NALs and decode them together.
    public func decode(nalData: Data, presentationTimeNs: Int64) {
        let nalUnits = splitNalUnits(nalData)
        var vclNals: [Data] = []

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
                vclNals.append(nal)
            default:
                // AUD(9), SEI(6), etc. — not video data.
                break
            }
        }

        if !vclNals.isEmpty {
            decodeAccessUnit(vclNals, presentationTimeNs: presentationTimeNs)
        }
    }

    override func resetParameterSetsLocked() {
        sps = nil
        pps = nil
        paramSetsReady = false
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

        if installFormatDescription(fmt) {
            locked { paramSetsReady = true }
        }
    }
}
