// SPDX-License-Identifier: MPL-2.0

import Foundation

public final class LatencyReporter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "oxr.latency.reporter", qos: .userInitiated)
    private var receiveTimesByPresentationNs: [Int64: Int64] = [:]
    private var receiveToDecodeSamplesMs: [Double] = []
    private var totalClientSamplesMs: [Double] = []
    private var lastReportTimeNs: Int64 = 0
    // Measured decode-to-photon time (EMA), fed by the renderer when it first draws each frame.
    // 0 = no measurement yet; the report then falls back to a one-refresh compositor budget.
    private var measuredDisplayLatencyMs: Double = 0

    public init() {}

    /// Record the measured decode-to-photon time for a newly displayed frame. This captures what
    /// the compositor-budget guess cannot: the wait until the render loop picks the frame up, the
    /// in-flight buffer queue, and the compositor's own present pipeline — so the server's pose
    /// prediction horizon covers the real client display path.
    public func noteFrameDisplayed(displayLatencyMs: Double) {
        guard displayLatencyMs > 0, displayLatencyMs < 200 else { return } // reject garbage/startup spikes
        queue.async { [self] in
            measuredDisplayLatencyMs = measuredDisplayLatencyMs == 0
                ? displayLatencyMs
                : measuredDisplayLatencyMs * 0.9 + displayLatencyMs * 0.1
        }
    }

    public func noteFrameReceived(presentationTimeNs: Int64, receiveTimeNs: Int64) {
        guard presentationTimeNs != 0 else { return }

        queue.async { [self] in
            receiveTimesByPresentationNs[presentationTimeNs] = receiveTimeNs

            if receiveTimesByPresentationNs.count > 24,
               let oldestPresentationTimeNs = receiveTimesByPresentationNs.keys.min() {
                receiveTimesByPresentationNs.removeValue(forKey: oldestPresentationTimeNs)
            }
        }
    }

    public func noteFrameDecoded(
        presentationTimeNs: Int64,
        decodeTimeNs: Int64,
        refreshRateHz: Int,
        controlChannel: ControlChannel
    ) {
        guard presentationTimeNs != 0 else { return }

        queue.async { [self] in
            guard let receiveTimeNs = receiveTimesByPresentationNs.removeValue(forKey: presentationTimeNs),
                  decodeTimeNs >= receiveTimeNs else {
                return
            }

            let receiveToDecodeMs = Double(decodeTimeNs - receiveTimeNs) / 1_000_000.0
            let compositorBudgetMs = 1_000.0 / Double(max(refreshRateHz, 1))
            // Prefer the measured decode-to-photon time over the one-refresh guess: it includes
            // the pickup wait, the in-flight buffer queue, and the compositor present pipeline.
            let displayMs = measuredDisplayLatencyMs > 0 ? measuredDisplayLatencyMs : compositorBudgetMs

            receiveToDecodeSamplesMs.append(receiveToDecodeMs)
            totalClientSamplesMs.append(receiveToDecodeMs + displayMs)

            if lastReportTimeNs == 0 {
                lastReportTimeNs = decodeTimeNs
                return
            }

            if decodeTimeNs - lastReportTimeNs < 1_000_000_000 {
                return
            }

            guard !receiveToDecodeSamplesMs.isEmpty, !totalClientSamplesMs.isEmpty else {
                lastReportTimeNs = decodeTimeNs
                return
            }

            var report = LatencyReport()
            report.receiveToDecoderSubmitMs = 0
            report.decodeLatencyMs = Float(Self.average(receiveToDecodeSamplesMs))
            report.compositorLatencyMs = Float(displayMs)
            report.displayedFrameAgeMs = Float(measuredDisplayLatencyMs)
            report.totalClientLatencyMs = Float(Self.average(totalClientSamplesMs))
            controlChannel.sendLatencyReport(report)

            receiveToDecodeSamplesMs.removeAll(keepingCapacity: true)
            totalClientSamplesMs.removeAll(keepingCapacity: true)
            lastReportTimeNs = decodeTimeNs
        }
    }

    public func reset() {
        queue.async { [self] in
            receiveTimesByPresentationNs.removeAll(keepingCapacity: false)
            receiveToDecodeSamplesMs.removeAll(keepingCapacity: false)
            totalClientSamplesMs.removeAll(keepingCapacity: false)
            lastReportTimeNs = 0
            measuredDisplayLatencyMs = 0
        }
    }

    private static func average(_ values: [Double]) -> Double {
        values.reduce(0, +) / Double(values.count)
    }
}
