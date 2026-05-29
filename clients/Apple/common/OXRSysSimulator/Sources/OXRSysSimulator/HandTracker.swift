// SPDX-License-Identifier: MPL-2.0

#if os(iOS)
// HandTracker.swift — Detects hand joints via Vision on ARKit frames.

import ARKit
import Vision

struct DetectedHand {
    let isLeft: Bool
    let joints: [SIMD3<Float>]
    let wristPosition: SIMD3<Float>
    let wristRotation: simd_quatf
}

final class HandTracker {
    private let request = VNDetectHumanHandPoseRequest()
    private let visionQueue = DispatchQueue(label: "oxr.vision.hands", qos: .userInitiated)
    private var frameCounter = 0

    static let minConfidence: Float = 0.25
    static let wristToMCPMeters: Float = 0.10
    static let detectionInterval = 4

    private nonisolated(unsafe) var isProcessing = false
    private nonisolated(unsafe) var latestHands: [DetectedHand] = []

    init() {
        request.maximumHandCount = 2
    }

    func detect(frame: ARFrame) -> [DetectedHand] {
        frameCounter += 1
        guard frameCounter % HandTracker.detectionInterval == 0 else { return latestHands }
        guard !isProcessing else { return latestHands }

        isProcessing = true

        let pixelBuffer = frame.capturedImage
        let intrinsics = frame.camera.intrinsics
        let camTransform = frame.camera.transform
        let imageSize = frame.camera.imageResolution

        visionQueue.async { [self] in
            defer { self.isProcessing = false }
            self.latestHands = self.runVision(
                pixelBuffer: pixelBuffer,
                intrinsics: intrinsics,
                camTransform: camTransform,
                imageSize: imageSize
            )
        }

        return latestHands
    }

    private func runVision(
        pixelBuffer: CVPixelBuffer,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize
    ) -> [DetectedHand] {
        let handler = VNImageRequestHandler(
            cvPixelBuffer: pixelBuffer,
            orientation: exifOrientation,
            options: [:]
        )
        guard (try? handler.perform([request])) != nil,
              let observations = request.results,
              !observations.isEmpty else { return [] }

        return observations.compactMap {
            buildHand(
                from: $0,
                intrinsics: intrinsics,
                camTransform: camTransform,
                imageSize: imageSize
            )
        }
    }

    private func buildHand(
        from obs: VNHumanHandPoseObservation,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize
    ) -> DetectedHand? {
        guard let all = try? obs.recognizedPoints(.all) else { return nil }

        let isLeft: Bool
        if #available(iOS 15, *) {
            isLeft = obs.chirality == .left
        } else {
            isLeft = false
        }

        guard let wristPt = all[.wrist], wristPt.confidence > HandTracker.minConfidence,
              let mcpPt = all[.middleMCP], mcpPt.confidence > HandTracker.minConfidence else {
            return nil
        }

        let fx = intrinsics[0][0]
        let wristPixels = visionToPixel(wristPt.location, imageSize: imageSize)
        let mcpPixels = visionToPixel(mcpPt.location, imageSize: imageSize)
        let pixelDist = simd_length(SIMD2<Float>(
            Float(mcpPixels.x - wristPixels.x),
            Float(mcpPixels.y - wristPixels.y)
        ))
        guard pixelDist > 8 else { return nil }
        let depth = HandTracker.wristToMCPMeters * fx / pixelDist

        let joints = buildJoints26(
            all: all,
            depth: depth,
            intrinsics: intrinsics,
            camTransform: camTransform,
            imageSize: imageSize
        )
        let wristWorld = unproject(
            visionPoint: wristPt.location,
            depth: depth,
            intrinsics: intrinsics,
            camTransform: camTransform,
            imageSize: imageSize
        )
        let rotation = buildRotation(
            all: all,
            depth: depth,
            intrinsics: intrinsics,
            camTransform: camTransform,
            imageSize: imageSize,
            wristWorld: wristWorld,
            isLeft: isLeft
        )

        return DetectedHand(
            isLeft: isLeft,
            joints: joints,
            wristPosition: wristWorld,
            wristRotation: rotation
        )
    }

    private func buildJoints26(
        all: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint],
        depth: Float,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize
    ) -> [SIMD3<Float>] {
        func joint(_ name: VNHumanHandPoseObservation.JointName) -> SIMD3<Float> {
            wp(all[name], depth: depth, intrinsics: intrinsics, camTransform: camTransform, imageSize: imageSize)
        }

        let wrist = joint(.wrist)
        func meta(_ name: VNHumanHandPoseObservation.JointName) -> SIMD3<Float> {
            wrist + (joint(name) - wrist) * 0.5
        }
        let middleMCP = joint(.middleMCP)
        let palm = (wrist + middleMCP) * 0.5

        return [
            palm,
            wrist,
            joint(.thumbCMC),
            joint(.thumbMP),
            joint(.thumbIP),
            joint(.thumbTip),
            meta(.indexMCP),
            joint(.indexMCP),
            joint(.indexPIP),
            joint(.indexDIP),
            joint(.indexTip),
            meta(.middleMCP),
            joint(.middleMCP),
            joint(.middlePIP),
            joint(.middleDIP),
            joint(.middleTip),
            meta(.ringMCP),
            joint(.ringMCP),
            joint(.ringPIP),
            joint(.ringDIP),
            joint(.ringTip),
            meta(.littleMCP),
            joint(.littleMCP),
            joint(.littlePIP),
            joint(.littleDIP),
            joint(.littleTip),
        ]
    }

    private func buildRotation(
        all: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint],
        depth: Float,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize,
        wristWorld: SIMD3<Float>,
        isLeft: Bool
    ) -> simd_quatf {
        let indexMCP = wp(all[.indexMCP], depth: depth, intrinsics: intrinsics, camTransform: camTransform, imageSize: imageSize)
        let littleMCP = wp(all[.littleMCP], depth: depth, intrinsics: intrinsics, camTransform: camTransform, imageSize: imageSize)
        let middleTip = wp(all[.middleTip], depth: depth, intrinsics: intrinsics, camTransform: camTransform, imageSize: imageSize)

        let forward = simd_normalize(middleTip - wristWorld)
        let sign: Float = isLeft ? -1 : 1
        let across = simd_normalize(indexMCP - littleMCP) * sign
        let up = simd_normalize(simd_cross(across, forward))
        let right = simd_normalize(simd_cross(forward, up))

        return simd_quatf(simd_float3x3(columns: (right, up, forward)))
    }

    private func wp(
        _ point: VNRecognizedPoint?,
        depth: Float,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize
    ) -> SIMD3<Float> {
        guard let point, point.confidence > HandTracker.minConfidence else {
            return unproject(
                visionPoint: CGPoint(x: 0.5, y: 0.5),
                depth: depth,
                intrinsics: intrinsics,
                camTransform: camTransform,
                imageSize: imageSize
            )
        }

        return unproject(
            visionPoint: point.location,
            depth: depth,
            intrinsics: intrinsics,
            camTransform: camTransform,
            imageSize: imageSize
        )
    }

    private func unproject(
        visionPoint: CGPoint,
        depth: Float,
        intrinsics: simd_float3x3,
        camTransform: simd_float4x4,
        imageSize: CGSize
    ) -> SIMD3<Float> {
        let px = visionToPixel(visionPoint, imageSize: imageSize)
        let fx = intrinsics[0][0]
        let fy = intrinsics[1][1]
        let cx = intrinsics[2][0]
        let cy = intrinsics[2][1]

        let xCam = (Float(px.x) - cx) * depth / fx
        let yCam = -(Float(px.y) - cy) * depth / fy
        let camPoint = SIMD4<Float>(xCam, yCam, -depth, 1)

        let world4 = camTransform * camPoint
        return SIMD3<Float>(world4.x, world4.y, world4.z)
    }

    private func visionToPixel(_ point: CGPoint, imageSize: CGSize) -> CGPoint {
        CGPoint(
            x: point.x * imageSize.width,
            y: (1.0 - point.y) * imageSize.height
        )
    }

    private var exifOrientation: CGImagePropertyOrientation { .right }
}
#endif
