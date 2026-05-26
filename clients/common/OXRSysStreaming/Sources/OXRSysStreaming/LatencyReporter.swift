// SPDX-License-Identifier: MPL-2.0

import Foundation

public final class LatencyReporter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "oxr.latency.reporter", qos: .userInitiated)
    private var receiveTimesByPresentationNs: [Int64: Int64] = [:]
    private var receiveToDecodeSamplesMs: [Double] = []
    private var totalClientSamplesMs: [Double] = []
    private var lastReportTimeNs: Int64 = 0

    public init() {}

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

            receiveToDecodeSamplesMs.append(receiveToDecodeMs)
            totalClientSamplesMs.append(receiveToDecodeMs + compositorBudgetMs)

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
            report.compositorLatencyMs = Float(compositorBudgetMs)
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
        }
    }

    private static func average(_ values: [Double]) -> Double {
        values.reduce(0, +) / Double(values.count)
    }
}
