// SPDX-License-Identifier: MPL-2.0

import simd

public struct TrackingVelocityEstimator: Sendable {
    private var lastSamplePosition: SIMD3<Float>?
    private var lastSampleOrientation: simd_quatf?
    private var lastSampleTime: Double = 0
    private var smoothedLinearVelocity: SIMD3<Float> = .zero
    private var smoothedAngularVelocity: SIMD3<Float> = .zero

    public init() {}

    public mutating func reset() {
        lastSamplePosition = nil
        lastSampleOrientation = nil
        lastSampleTime = 0
        smoothedLinearVelocity = .zero
        smoothedAngularVelocity = .zero
    }

    public mutating func update(
        position: SIMD3<Float>,
        orientation: simd_quatf,
        timestamp: Double,
        isTracking: Bool
    ) -> (linearVelocity: SIMD3<Float>, angularVelocity: SIMD3<Float>)? {
        let orientationLength = simd_length(orientation.vector)
        guard isTracking, timestamp.isFinite,
              position.x.isFinite, position.y.isFinite, position.z.isFinite,
              orientationLength.isFinite, orientationLength > 1e-6 else {
            reset()
            return nil
        }

        let prevPosition = lastSamplePosition
        let prevOrientation = lastSampleOrientation
        let dt = Float(timestamp - lastSampleTime)
        lastSamplePosition = position
        lastSampleOrientation = simd_normalize(orientation)
        lastSampleTime = timestamp
        guard let prevPosition, let prevOrientation, dt > 0.001, dt < 0.1 else {
            smoothedLinearVelocity = .zero
            smoothedAngularVelocity = .zero
            return nil
        }

        let rawLinear = (position - prevPosition) / dt
        // World-frame angular velocity: q_now = delta * q_previous.
        var delta = simd_normalize(orientation * prevOrientation.inverse)
        if delta.real < 0 { delta = simd_quatf(vector: -delta.vector) }
        var rawAngular = SIMD3<Float>.zero
        let imagLength = simd_length(delta.imag)
        if imagLength > 1e-6 {
            let angle = 2 * atan2(imagLength, delta.real)
            rawAngular = (delta.imag / imagLength) * (angle / dt)
        }
        guard rawLinear.x.isFinite, rawLinear.y.isFinite, rawLinear.z.isFinite,
              rawAngular.x.isFinite, rawAngular.y.isFinite, rawAngular.z.isFinite else {
            reset()
            return nil
        }

        // Retain the existing two-sample smoothing of measured head motion.
        let alpha: Float = 0.5
        smoothedLinearVelocity = smoothedLinearVelocity * (1 - alpha) + rawLinear * alpha
        smoothedAngularVelocity = smoothedAngularVelocity * (1 - alpha) + rawAngular * alpha
        return (smoothedLinearVelocity, smoothedAngularVelocity)
    }
}
