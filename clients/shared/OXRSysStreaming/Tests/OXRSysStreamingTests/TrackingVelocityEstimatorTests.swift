// SPDX-License-Identifier: MPL-2.0

import simd
import XCTest
@testable import OXRSysStreaming

final class TrackingVelocityEstimatorTests: XCTestCase {
    private let identity = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)

    func testFirstSampleIsUnavailableAndStationaryMeasurementIsValid() {
        var estimator = TrackingVelocityEstimator()
        XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true))
        let sample = estimator.update(position: .zero, orientation: identity, timestamp: 1.01, isTracking: true)
        XCTAssertNotNil(sample)
        XCTAssertEqual(sample?.linearVelocity, .zero)
        XCTAssertEqual(sample?.angularVelocity, .zero)
    }

    func testMeasuredTranslationAndRotationRetainExistingSmoothing() throws {
        var estimator = TrackingVelocityEstimator()
        XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true))
        let sample = try XCTUnwrap(estimator.update(
            position: SIMD3<Float>(0.1, 0, 0),
            orientation: simd_quatf(angle: .pi / 2, axis: SIMD3<Float>(0, 1, 0)),
            timestamp: 1.05, isTracking: true
        ))
        XCTAssertEqual(sample.linearVelocity.x, 1, accuracy: 1e-5)
        XCTAssertEqual(sample.linearVelocity.y, 0)
        XCTAssertEqual(sample.linearVelocity.z, 0)
        XCTAssertEqual(sample.angularVelocity.x, 0)
        XCTAssertEqual(sample.angularVelocity.y, 5 * .pi, accuracy: 1e-4)
        XCTAssertEqual(sample.angularVelocity.z, 0)
    }

    func testTrackingLossAndMissingAnchorResetMeasurements() {
        var estimator = TrackingVelocityEstimator()
        _ = estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true)
        XCTAssertNotNil(estimator.update(position: SIMD3<Float>(0.1, 0, 0), orientation: identity, timestamp: 1.01, isTracking: true))
        XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1.02, isTracking: false))
        XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1.03, isTracking: true))
        XCTAssertEqual(estimator.update(position: .zero, orientation: identity, timestamp: 1.04, isTracking: true)?.linearVelocity, .zero)
        estimator.reset()
        XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1.05, isTracking: true))
    }

    func testInvalidSampleIntervalsDoNotReuseOldVelocity() {
        for dt in [0, 0.0005, 0.1, 1, -0.01, Double.infinity, Double.nan] {
            var estimator = TrackingVelocityEstimator()
            _ = estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true)
            XCTAssertNotNil(estimator.update(position: SIMD3<Float>(0.1, 0, 0), orientation: identity, timestamp: 1.01, isTracking: true))
            XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1.01 + dt, isTracking: true))
        }
    }

    func testInvalidPosesInvalidateTheNextMeasurement() {
        let invalidPoses: [(SIMD3<Float>, simd_quatf)] = [
            (SIMD3<Float>(.nan, 0, 0), identity),
            (SIMD3<Float>(0, .infinity, 0), identity),
            (.zero, simd_quatf(vector: SIMD4<Float>(0, 0, 0, 0))),
            (.zero, simd_quatf(vector: SIMD4<Float>(0, 0, 0, .nan)))
        ]
        for (position, orientation) in invalidPoses {
            var estimator = TrackingVelocityEstimator()
            _ = estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true)
            XCTAssertNil(estimator.update(position: position, orientation: orientation, timestamp: 1.01, isTracking: true))
            XCTAssertNil(estimator.update(position: .zero, orientation: identity, timestamp: 1.02, isTracking: true))
        }
    }

    func testQuaternionSignChangeDoesNotCreateAngularVelocity() {
        var estimator = TrackingVelocityEstimator()
        _ = estimator.update(position: .zero, orientation: identity, timestamp: 1, isTracking: true)
        let sample = estimator.update(
            position: .zero, orientation: simd_quatf(vector: -identity.vector),
            timestamp: 1.01, isTracking: true
        )
        XCTAssertNotNil(sample)
        XCTAssertEqual(sample?.angularVelocity, .zero)
    }
}
