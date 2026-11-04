// SPDX-License-Identifier: MPL-2.0

import simd
import XCTest
@testable import OXRSysStreaming

final class RenderPoseReprojectorTests: XCTestCase {
    private let pose = RenderPose(position: .zero, orientation: simd_quatf(ix: 0, iy: 0, iz: 0, r: 1))

    func testExactPoseWinsOverNewerMetadata() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 100_000_000, pose: pose)
        let newerPose = RenderPose(position: SIMD3(0.01, 0, 0), orientation: pose.orientation)
        reprojector.note(presentationTimeNs: 110_000_000, pose: newerPose)

        XCTAssertEqual(selected(reprojector, timestamp: 100_000_000)?.position, pose.position)
        XCTAssertEqual(selected(reprojector, timestamp: 110_000_000)?.position, newerPose.position)
    }

    func testFallbackRejectsFutureAndOldMetadata() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 100_000_000, pose: pose)

        XCTAssertNil(selected(reprojector, timestamp: 99_999_999))
        XCTAssertEqual(selected(reprojector, timestamp: 150_000_000)?.position, pose.position)
        XCTAssertNil(selected(reprojector, timestamp: 150_000_001))
    }

    func testReorderedMetadataCannotRegressFallback() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 110_000_000, pose: pose)
        XCTAssertNotNil(selected(reprojector, timestamp: 120_000_000))
        let olderPose = RenderPose(position: SIMD3(0.01, 0, 0), orientation: pose.orientation)
        reprojector.note(presentationTimeNs: 100_000_000, pose: olderPose)

        XCTAssertEqual(selected(reprojector, timestamp: 125_000_000)?.position, pose.position)
        XCTAssertNil(selected(reprojector, timestamp: 105_000_000))
        XCTAssertEqual(selected(reprojector, timestamp: 100_000_000)?.position, olderPose.position)
    }

    func testStaleFramesAndRecoveryDisableExactPose() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 100_000_000, pose: pose)

        XCTAssertNotNil(selected(reprojector, timestamp: 100_000_000, now: 175_000_000))
        XCTAssertNil(selected(reprojector, timestamp: 100_000_000, now: 175_000_001))
        XCTAssertNil(selected(reprojector, timestamp: 100_000_000, now: 99_999_999))
        XCTAssertNil(selected(reprojector, timestamp: 100_000_000, recovering: true))
        XCTAssertNil(reprojector.pose(forPresentationTimeNs: 100_000_000,
                                     decodeTimeNs: 0, nowNs: 100_000_000, isRecovering: false))
    }

    func testDisconnectAndOffClearPoseHistory() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 100_000_000, pose: pose)
        reprojector.reset()
        reprojector.note(presentationTimeNs: 110_000_000, pose: pose)
        XCTAssertNil(selected(reprojector, timestamp: 110_000_000))

        reprojector.reset(enabled: true)
        XCTAssertNil(selected(reprojector, timestamp: 100_000_000))
        XCTAssertNil(selected(reprojector, timestamp: 110_000_000))
        reprojector.note(presentationTimeNs: 120_000_000, pose: pose)
        XCTAssertNotNil(selected(reprojector, timestamp: 120_000_000))
    }

    func testHistoryRemainsBounded() {
        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        for timestamp in 1...241 {
            reprojector.note(presentationTimeNs: Int64(timestamp), pose: pose)
        }

        XCTAssertNil(selected(reprojector, timestamp: 1))
        XCTAssertNotNil(selected(reprojector, timestamp: 2))
        XCTAssertNotNil(selected(reprojector, timestamp: 241))
    }

    func testStrongTranslationAndInvalidPosesDisableWarp() {
        XCTAssertTrue(pose.canReproject(from: pose))
        XCTAssertTrue(pose.canReproject(from: RenderPose(
            position: SIMD3(0.08, 0, 0), orientation: pose.orientation
        )))
        XCTAssertFalse(pose.canReproject(from: RenderPose(
            position: SIMD3(0.0801, 0, 0), orientation: pose.orientation
        )))
        XCTAssertFalse(pose.canReproject(from: RenderPose(
            position: SIMD3(.nan, 0, 0), orientation: pose.orientation
        )))
        XCTAssertFalse(pose.canReproject(from: RenderPose(
            position: .zero, orientation: simd_quatf(vector: .zero)
        )))

        let reprojector = RenderPoseReprojector()
        reprojector.reset(enabled: true)
        reprojector.note(presentationTimeNs: 100_000_000, pose: RenderPose(
            position: .zero, orientation: simd_quatf(vector: SIMD4(.infinity, 0, 0, 1))
        ))
        XCTAssertNil(selected(reprojector, timestamp: 100_000_000))
    }

    func testDecoderExposesRecoveryUntilReset() {
        let decoder = VideoDecoder()
        XCTAssertFalse(decoder.isRecovering)
        decoder.decode(nalData: Data([0]), codec: .av1, presentationTimeNs: 100_000_000)
        XCTAssertTrue(decoder.isRecovering)
        decoder.invalidate()
        XCTAssertFalse(decoder.isRecovering)
    }

    private func selected(
        _ reprojector: RenderPoseReprojector,
        timestamp: Int64,
        now: Int64 = 100_000_000,
        recovering: Bool = false
    ) -> RenderPose? {
        reprojector.pose(forPresentationTimeNs: timestamp,
                         decodeTimeNs: 100_000_000, nowNs: now, isRecovering: recovering)
    }
}
