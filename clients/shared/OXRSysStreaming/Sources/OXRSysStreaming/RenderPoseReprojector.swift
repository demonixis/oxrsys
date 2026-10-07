// SPDX-License-Identifier: MPL-2.0

import Foundation
import simd

public struct RenderPose: Sendable {
    public var position: SIMD3<Float>
    public var orientation: simd_quatf

    public init(position: SIMD3<Float>, orientation: simd_quatf) {
        self.position = position
        self.orientation = orientation
    }

    fileprivate var isValid: Bool {
        position.x.isFinite && position.y.isFinite && position.z.isFinite &&
            orientation.vector.x.isFinite && orientation.vector.y.isFinite &&
            orientation.vector.z.isFinite && orientation.vector.w.isFinite &&
            simd_length_squared(orientation.vector).isFinite &&
            simd_length_squared(orientation.vector) > 0.0001
    }

    public func canReproject(from currentPose: RenderPose) -> Bool {
        isValid && currentPose.isValid && simd_distance(position, currentPose.position) <= 0.08
    }
}

public final class RenderPoseReprojector: @unchecked Sendable {
    private let lock = NSLock()
    private var poseByPresentationNs: [Int64: RenderPose] = [:]
    private let capacity = 240
    private var latestKey: Int64 = 0
    private var latestPose: RenderPose?
    private var lastSelectedKey: Int64 = 0
    private var enabled = false

    public init() {}

    public func reset(enabled: Bool = false) {
        lock.lock()
        defer { lock.unlock() }
        poseByPresentationNs.removeAll(keepingCapacity: true)
        latestKey = 0
        latestPose = nil
        lastSelectedKey = 0
        self.enabled = enabled
    }

    public func note(presentationTimeNs: Int64, pose: RenderPose) {
        lock.lock()
        defer { lock.unlock() }
        guard enabled, presentationTimeNs > 0, pose.isValid else { return }
        poseByPresentationNs[presentationTimeNs] = pose
        if presentationTimeNs >= latestKey {
            latestKey = presentationTimeNs
            latestPose = pose
        }
        if poseByPresentationNs.count > capacity,
           let oldest = poseByPresentationNs.keys.min() {
            poseByPresentationNs.removeValue(forKey: oldest)
        }
    }

    public func pose(
        forPresentationTimeNs presentationTimeNs: Int64,
        decodeTimeNs: Int64,
        nowNs: Int64,
        isRecovering: Bool
    ) -> RenderPose? {
        lock.lock()
        defer { lock.unlock() }
        guard enabled, !isRecovering, presentationTimeNs > 0,
              decodeTimeNs > 0, nowNs >= decodeTimeNs,
              nowNs - decodeTimeNs <= 75_000_000 else { return nil }

        if let pose = poseByPresentationNs[presentationTimeNs] {
            lastSelectedKey = max(lastSelectedKey, presentationTimeNs)
            return pose
        }

        guard latestKey > 0, latestKey <= presentationTimeNs,
              latestKey >= lastSelectedKey,
              presentationTimeNs - latestKey <= 50_000_000 else { return nil }
        lastSelectedKey = latestKey
        return latestPose
    }
}
