// SPDX-License-Identifier: MPL-2.0

#if os(macOS)
import AVFoundation
import CoreGraphics
import CoreImage
import CoreMedia
import OXRSysStreaming
import simd
import Vision

struct DesktopWebcamDevice: Identifiable, Equatable, Sendable {
    let id: String
    let name: String
}

struct DesktopWebcamTrackingConfiguration: Sendable {
    var sourceMode: WebcamTrackingSourceMode
    var primaryDeviceID: String
    var secondaryDeviceID: String
    var captureResolution: WebcamCaptureResolution = .medium
    var calibration: WebcamRigCalibration?
}

enum WebcamCaptureResolution: String, CaseIterable, Identifiable, Sendable {
    case low = "Low"
    case medium = "Medium"
    case hd720 = "720p"
    case hd1080 = "1080p"

    var id: String { rawValue }

    var sessionPreset: AVCaptureSession.Preset {
        switch self {
        case .low:
            return .low
        case .medium:
            return .medium
        case .hd720:
            return .hd1280x720
        case .hd1080:
            return .hd1920x1080
        }
    }

    var fallbackPresets: [AVCaptureSession.Preset] {
        switch self {
        case .low:
            return [.low, .medium]
        case .medium:
            return [.medium, .low]
        case .hd720:
            return [.hd1280x720, .medium, .low]
        case .hd1080:
            return [.hd1920x1080, .hd1280x720, .medium, .low]
        }
    }

    static func label(for preset: AVCaptureSession.Preset) -> String {
        switch preset {
        case .low:
            return WebcamCaptureResolution.low.rawValue
        case .medium:
            return WebcamCaptureResolution.medium.rawValue
        case .hd1280x720:
            return WebcamCaptureResolution.hd720.rawValue
        case .hd1920x1080:
            return WebcamCaptureResolution.hd1080.rawValue
        default:
            return preset.rawValue
        }
    }
}

enum WebcamHandDepthJoint: Hashable, Sendable {
    case wrist
    case middleMCP
    case indexMCP
    case littleMCP
}

struct WebcamHandDepthLandmark: Sendable {
    var normalizedPosition: SIMD2<Float>
    var confidence: Float
}

struct WebcamHandDepthSample: Sendable {
    var depth: Float
    var weight: Float
}

enum WebcamHandDepthEstimator {
    static let minimumPixelDistance: Float = 8
    static let maximumFrameDepthJump: Float = 0.45

    private struct Measurement {
        var first: WebcamHandDepthJoint
        var second: WebcamHandDepthJoint
        var physicalMeters: Float
        var weight: Float
    }

    private static let measurements: [Measurement] = [
        Measurement(first: .wrist, second: .middleMCP, physicalMeters: 0.10, weight: 1.0),
        Measurement(first: .indexMCP, second: .littleMCP, physicalMeters: 0.085, weight: 1.1),
        Measurement(first: .wrist, second: .indexMCP, physicalMeters: 0.095, weight: 0.8),
        Measurement(first: .wrist, second: .littleMCP, physicalMeters: 0.105, weight: 0.8),
    ]

    static func estimateDepth(
        landmarks: [WebcamHandDepthJoint: WebcamHandDepthLandmark],
        intrinsics: WebcamCameraIntrinsics,
        settings: WebcamTrackingSettings
    ) -> Float? {
        let focal = max((intrinsics.fx + intrinsics.fy) * 0.5, 1)
        let samples = measurements.compactMap { measurement -> WebcamHandDepthSample? in
            guard let first = landmarks[measurement.first],
                  let second = landmarks[measurement.second] else {
                return nil
            }

            let dx = (first.normalizedPosition.x - second.normalizedPosition.x) * intrinsics.width
            let dy = (first.normalizedPosition.y - second.normalizedPosition.y) * intrinsics.height
            let pixelDistance = sqrt(dx * dx + dy * dy)
            guard pixelDistance >= minimumPixelDistance else { return nil }

            let confidence = max(0, min(first.confidence, second.confidence))
            let rawDepth = measurement.physicalMeters * focal / pixelDistance
            let calibratedDepth = rawDepth * settings.handDepthScale + settings.handDepthOffset
            return WebcamHandDepthSample(
                depth: min(max(calibratedDepth, 0.25), 2.2),
                weight: confidence * measurement.weight
            )
        }
        return weightedMedian(samples: samples)
    }

    static func weightedMedian(samples: [WebcamHandDepthSample]) -> Float? {
        let validSamples = samples
            .filter { $0.weight > 0 && $0.depth.isFinite }
            .sorted { $0.depth < $1.depth }
        guard !validSamples.isEmpty else { return nil }

        let totalWeight = validSamples.reduce(Float(0)) { $0 + $1.weight }
        guard totalWeight > 0 else { return nil }

        var accumulatedWeight: Float = 0
        for sample in validSamples {
            accumulatedWeight += sample.weight
            if accumulatedWeight >= totalWeight * 0.5 {
                return sample.depth
            }
        }
        return validSamples.last?.depth
    }

    static func smoothDepth(
        previous: Float?,
        raw: Float,
        settings: WebcamTrackingSettings
    ) -> Float {
        guard let previous else { return raw }
        guard abs(raw - previous) <= maximumFrameDepthJump else { return previous }

        let interpolation = min(max(settings.handDepthSmoothing, 0), 1)
        return previous + (raw - previous) * interpolation
    }
}

final class DesktopWebcamTrackingManager: NSObject, @unchecked Sendable {
    var onTrackingUpdate: (@Sendable (WebcamTrackingSnapshot) -> Void)?
    var onStatusUpdate: (@Sendable (String) -> Void)?
    var onPreviewUpdate: (@Sendable (WebcamPreviewFrame) -> Void)?

    private let queue = DispatchQueue(label: "oxr.simulator.webcam", qos: .userInteractive)
    private let settingsLock = NSLock()
    private var settings = WebcamTrackingSettings()
    private var configuration = DesktopWebcamTrackingConfiguration(
        sourceMode: .singleCamera,
        primaryDeviceID: "",
        secondaryDeviceID: "",
        calibration: nil
    )
    private var sources: [WebcamCaptureSource] = []
    private var latestFrames: [String: WebcamSourceFrame] = [:]
    private var lastSnapshot: WebcamTrackingSnapshot?
    private var lastHeadEuler: SIMD3<Float>?
    private var frameCounter = 0
    private var statusWindowStartNs: Int64 = 0
    private var previewEnabled = false
    private var previewGeneration: UInt64 = 0

    static func availableVideoDevices() -> [DesktopWebcamDevice] {
        availableAVCaptureDevices().map {
            DesktopWebcamDevice(id: $0.uniqueID, name: $0.localizedName)
        }
    }

    func start(deviceID: String?) {
        start(
            configuration: DesktopWebcamTrackingConfiguration(
                sourceMode: .singleCamera,
                primaryDeviceID: deviceID ?? "",
                secondaryDeviceID: "",
                captureResolution: .medium,
                calibration: nil
            )
        )
    }

    func start(configuration: DesktopWebcamTrackingConfiguration) {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            queue.async { [weak self] in
                self?.startCapture(configuration: configuration)
            }
        case .notDetermined:
            emitStatus("Requesting camera access")
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                guard let self else { return }
                if granted {
                    self.queue.async { [weak self] in
                        self?.startCapture(configuration: configuration)
                    }
                } else {
                    self.emitStatus("Camera access denied")
                }
            }
        case .denied, .restricted:
            emitStatus("Camera access denied")
        @unknown default:
            emitStatus("Camera unavailable")
        }
    }

    func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.stopCaptureOnQueue()
            self.emitStatus("Webcam tracking off")
        }
    }

    func resetHeadPose() {
        queue.async { [weak self] in
            guard let self else { return }
            self.lastSnapshot = nil
            self.lastHeadEuler = nil
            for source in self.sources {
                source.resetHeadPose()
            }
        }
    }

    func updateSettings(_ settings: WebcamTrackingSettings) {
        settingsLock.lock()
        self.settings = settings
        settingsLock.unlock()
        queue.async { [weak self] in
            guard let self else { return }
            for source in self.sources {
                source.updateSettings(settings)
            }
        }
    }

    func setPreviewEnabled(_ enabled: Bool, generation: UInt64 = 0) {
        queue.async { [weak self] in
            guard let self else { return }
            self.previewEnabled = enabled
            self.previewGeneration = generation
            for source in self.sources {
                source.setPreviewEnabled(enabled, generation: generation)
            }
        }
    }

    private func startCapture(configuration: DesktopWebcamTrackingConfiguration) {
        stopCaptureOnQueue()
        self.configuration = configuration

        let selectedDevices = selectDevices(configuration: configuration)
        guard !selectedDevices.isEmpty else {
            emitStatus("No camera found")
            return
        }

        let currentSettings = currentSettings()
        var startedSources: [WebcamCaptureSource] = []
        for device in selectedDevices {
            let source = WebcamCaptureSource(
                device: device,
                captureResolution: configuration.captureResolution,
                settings: currentSettings,
                previewEnabled: previewEnabled,
                previewGeneration: previewGeneration,
                onFrame: { [weak self] frame in
                    self?.queue.async { [weak self] in
                        self?.handleFrame(frame)
                    }
                },
                onPreview: { [weak self] frame in
                    self?.onPreviewUpdate?(frame)
                },
                onStatus: { [weak self] status in
                    self?.emitStatus(status)
                }
            )
            if source.start() {
                startedSources.append(source)
            }
        }

        guard !startedSources.isEmpty else {
            emitStatus("Camera start failed")
            return
        }

        sources = startedSources
        latestFrames = [:]
        lastSnapshot = nil
        lastHeadEuler = nil
        frameCounter = 0
        statusWindowStartNs = VideoReceiver.monotonicNs()
        emitStatus(startedStatus(deviceCount: startedSources.count, requestedCount: selectedDevices.count))
    }

    private func stopCaptureOnQueue() {
        for source in sources {
            source.stop()
        }
        sources = []
        latestFrames = [:]
        lastSnapshot = nil
        lastHeadEuler = nil
        frameCounter = 0
        statusWindowStartNs = 0
    }

    private func handleFrame(_ frame: WebcamSourceFrame) {
        latestFrames[frame.cameraID] = frame
        let frames = Array(latestFrames.values)
        let result = WebcamRigFusion.fuse(
            frames: frames,
            mode: effectiveSourceMode(for: frames.count),
            calibration: configuration.calibration,
            previous: lastSnapshot
        )
        let smoothedSnapshot = smoothSnapshot(result.snapshot, settings: currentSettings())
        lastSnapshot = smoothedSnapshot
        if let smoothedSnapshot {
            onTrackingUpdate?(smoothedSnapshot)
        } else {
            onTrackingUpdate?(
                WebcamTrackingSnapshot(
                    timestampNs: VideoReceiver.monotonicNs(),
                    head: nil,
                    leftHand: nil,
                    rightHand: nil
                )
            )
        }
        updateStatusIfNeeded(result: result, snapshot: smoothedSnapshot)
    }

    private func selectDevices(configuration: DesktopWebcamTrackingConfiguration) -> [AVCaptureDevice] {
        let devices = Self.availableAVCaptureDevices()
        guard !devices.isEmpty else { return [] }

        let primary = device(with: configuration.primaryDeviceID, in: devices) ?? devices[0]
        guard configuration.sourceMode != .singleCamera else { return [primary] }

        let secondary =
            device(with: configuration.secondaryDeviceID, in: devices, excluding: primary.uniqueID) ??
            devices.first { $0.uniqueID != primary.uniqueID }

        if let secondary {
            return [primary, secondary]
        }
        return [primary]
    }

    private func effectiveSourceMode(for activeCameraCount: Int) -> WebcamTrackingSourceMode {
        activeCameraCount >= 2 ? configuration.sourceMode : .singleCamera
    }

    private func device(
        with deviceID: String,
        in devices: [AVCaptureDevice],
        excluding excludedDeviceID: String? = nil
    ) -> AVCaptureDevice? {
        guard !deviceID.isEmpty, deviceID != excludedDeviceID else { return nil }
        return devices.first { $0.uniqueID == deviceID }
    }

    private func startedStatus(deviceCount: Int, requestedCount: Int) -> String {
        if configuration.sourceMode == .singleCamera || deviceCount == 1 {
            let deviceName = sources.first?.deviceName ?? "camera"
            let resolutionLabel = sources.first?.activeCaptureResolutionLabel ??
                configuration.captureResolution.rawValue
            if configuration.sourceMode == .singleCamera {
                return "Webcam tracking: \(deviceName) @ \(resolutionLabel)"
            }
            return "Webcam tracking: \(deviceName) @ \(resolutionLabel) (single-camera fallback)"
        }
        if deviceCount < requestedCount {
            return "Webcam tracking: \(deviceCount) cameras active @ \(configuration.captureResolution.rawValue) (partial rig)"
        }
        return "Webcam tracking: \(deviceCount) cameras active @ \(configuration.captureResolution.rawValue)"
    }

    private func updateStatusIfNeeded(result: WebcamFusionResult, snapshot: WebcamTrackingSnapshot?) {
        frameCounter += 1
        let now = VideoReceiver.monotonicNs()
        if statusWindowStartNs == 0 {
            statusWindowStartNs = now
        }
        let elapsedNs = now - statusWindowStartNs
        guard elapsedNs >= 1_000_000_000 else { return }

        let fps = Double(frameCounter) / (Double(elapsedNs) / 1_000_000_000.0)
        frameCounter = 0
        statusWindowStartNs = now

        var modeLabel = effectiveSourceMode(for: result.activeCameraCount).rawValue
        if configuration.sourceMode == .calibratedMultiCamera {
            if result.usedCalibratedFusion {
                modeLabel = "Calibrated Multi-Camera"
            } else if result.calibratedCameraCount < 2 {
                modeLabel = "Best View (calibration unavailable)"
            } else {
                modeLabel = "Best View (calibration rejected)"
            }
        }
        emitStatus(
            String(
                format: "%@ - %d cam, %.0f fps, %.0f%% confidence%@",
                modeLabel,
                result.activeCameraCount,
                fps,
                Double(result.confidence * 100),
                handDepthStatus(snapshot)
            )
        )
    }

    private func handDepthStatus(_ snapshot: WebcamTrackingSnapshot?) -> String {
        guard let snapshot else { return "" }
        var parts: [String] = []
        if let depth = snapshot.leftHand?.estimatedDepth {
            parts.append(String(format: "L %.2fm", Double(depth)))
        }
        if let depth = snapshot.rightHand?.estimatedDepth {
            parts.append(String(format: "R %.2fm", Double(depth)))
        }
        return parts.isEmpty ? "" : ", " + parts.joined(separator: " ")
    }

    private func smoothSnapshot(
        _ snapshot: WebcamTrackingSnapshot?,
        settings: WebcamTrackingSettings
    ) -> WebcamTrackingSnapshot? {
        guard let snapshot else {
            lastHeadEuler = nil
            return nil
        }

        let previous = lastSnapshot
        let head = smoothHead(snapshot.head, previous: previous?.head, settings: settings)
        let leftHand = smoothHand(
            snapshot.leftHand,
            previous: previous?.leftHand,
            snapshotTimestampNs: snapshot.timestampNs,
            previousTimestampNs: previous?.timestampNs,
            settings: settings
        )
        let rightHand = smoothHand(
            snapshot.rightHand,
            previous: previous?.rightHand,
            snapshotTimestampNs: snapshot.timestampNs,
            previousTimestampNs: previous?.timestampNs,
            settings: settings
        )
        return WebcamTrackingSnapshot(
            timestampNs: snapshot.timestampNs,
            head: head,
            leftHand: leftHand,
            rightHand: rightHand,
            usesCalibratedTrackingSpace: snapshot.usesCalibratedTrackingSpace
        )
    }

    private func smoothHead(
        _ head: WebcamHeadPose?,
        previous: WebcamHeadPose?,
        settings: WebcamTrackingSettings
    ) -> WebcamHeadPose? {
        guard let head else {
            lastHeadEuler = nil
            return nil
        }
        guard let previous else {
            lastHeadEuler = nil
            return head
        }

        let smoothedPosition = WebcamTrackingMath.smoothVector(
            previous: previous.position,
            raw: head.position,
            interpolation: settings.headPositionInterpolation,
            deadzone: settings.movementDeadzone
        )
        let previousEuler = lastHeadEuler ??
            previous.yawPitchRoll ??
            WebcamTrackingMath.eulerAngles(from: previous.orientation)
        let rawEuler = head.yawPitchRoll ?? WebcamTrackingMath.eulerAngles(from: head.orientation)
        let smoothedEuler = WebcamTrackingMath.smoothAngleVector(
            previous: previousEuler,
            raw: rawEuler,
            interpolation: settings.headRotationInterpolation,
            deadzone: WebcamTrackingMath.radians(settings.headRotationDeadzoneDegrees)
        )
        lastHeadEuler = smoothedEuler
        return WebcamHeadPose(
            position: smoothedPosition,
            orientation: WebcamTrackingMath.headQuaternion(fromYawPitchRoll: smoothedEuler),
            yawPitchRoll: smoothedEuler
        )
    }

    private func smoothHand(
        _ hand: WebcamTrackedHand?,
        previous: WebcamTrackedHand?,
        snapshotTimestampNs: Int64,
        previousTimestampNs: Int64?,
        settings: WebcamTrackingSettings
    ) -> WebcamTrackedHand? {
        guard let hand else {
            guard let previous,
                  let previousTimestampNs,
                  snapshotTimestampNs - previousTimestampNs <= 150_000_000 else {
                return nil
            }
            return previous
        }
        guard let previous, previous.isLeft == hand.isLeft else { return hand }
        guard simd_length(hand.wristPosition - previous.wristPosition) <= 0.45 else {
            return previous
        }

        let joints = zip(previous.joints, hand.joints).map { previousJoint, rawJoint in
            WebcamTrackingMath.smoothVector(
                previous: previousJoint,
                raw: rawJoint,
                interpolation: settings.handPositionInterpolation,
                deadzone: settings.movementDeadzone
            )
        }
        let wristPosition = WebcamTrackingMath.smoothVector(
            previous: previous.wristPosition,
            raw: hand.wristPosition,
            interpolation: settings.handPositionInterpolation,
            deadzone: settings.movementDeadzone
        )
        let rawRotation = WebcamTrackingMath.sameHemisphere(
            hand.wristRotation,
            reference: previous.wristRotation
        )
        let wristRotation = simd_slerp(
            previous.wristRotation,
            rawRotation,
            min(max(settings.handRotationInterpolation, 0), 1)
        )

        return WebcamTrackedHand(
            isLeft: hand.isLeft,
            joints: joints,
            wristPosition: wristPosition,
            wristRotation: wristRotation,
            estimatedDepth: hand.estimatedDepth
        )
    }

    private func currentSettings() -> WebcamTrackingSettings {
        settingsLock.lock()
        let current = settings
        settingsLock.unlock()
        return current
    }

    private func emitStatus(_ status: String) {
        onStatusUpdate?(status)
    }

    private static func availableAVCaptureDevices() -> [AVCaptureDevice] {
        let discovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInWideAngleCamera, .external],
            mediaType: .video,
            position: .unspecified
        )
        return discovery.devices
    }
}

private final class WebcamCaptureSource: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let deviceName: String
    private(set) var activeCaptureResolutionLabel: String

    private let device: AVCaptureDevice
    private let captureResolution: WebcamCaptureResolution
    private let queue: DispatchQueue
    private let handRequest = VNDetectHumanHandPoseRequest()
    private let faceRequest = VNDetectFaceLandmarksRequest()
    private let settingsLock = NSLock()
    private let previewLock = NSLock()
    private let onFrame: @Sendable (WebcamSourceFrame) -> Void
    private let onPreview: @Sendable (WebcamPreviewFrame) -> Void
    private let onStatus: @Sendable (String) -> Void
    private let imageContext = CIContext()
    private var settings: WebcamTrackingSettings
    private var previewEnabled: Bool
    private var previewGeneration: UInt64
    private var session: AVCaptureSession?
    private var neutralFace: FaceSample?
    private var previousLeftHandDepth: Float?
    private var previousRightHandDepth: Float?
    private var pendingHeadReset = false

    init(
        device: AVCaptureDevice,
        captureResolution: WebcamCaptureResolution,
        settings: WebcamTrackingSettings,
        previewEnabled: Bool,
        previewGeneration: UInt64,
        onFrame: @escaping @Sendable (WebcamSourceFrame) -> Void,
        onPreview: @escaping @Sendable (WebcamPreviewFrame) -> Void,
        onStatus: @escaping @Sendable (String) -> Void
    ) {
        self.device = device
        self.deviceName = device.localizedName
        self.captureResolution = captureResolution
        self.activeCaptureResolutionLabel = captureResolution.rawValue
        self.settings = settings
        self.previewEnabled = previewEnabled
        self.previewGeneration = previewGeneration
        self.onFrame = onFrame
        self.onPreview = onPreview
        self.onStatus = onStatus
        self.queue = DispatchQueue(
            label: "oxr.simulator.webcam.\(device.uniqueID)",
            qos: .userInteractive
        )
        super.init()
        handRequest.maximumHandCount = 2
    }

    func start() -> Bool {
        do {
            let input = try AVCaptureDeviceInput(device: device)
            let output = AVCaptureVideoDataOutput()
            output.alwaysDiscardsLateVideoFrames = true
            output.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            ]
            output.setSampleBufferDelegate(self, queue: queue)

            let session = AVCaptureSession()
            session.beginConfiguration()
            let activePreset = applyCaptureResolution(to: session)
            guard session.canAddInput(input), session.canAddOutput(output) else {
                session.commitConfiguration()
                onStatus("Camera format unavailable: \(device.localizedName)")
                return false
            }
            session.addInput(input)
            session.addOutput(output)
            if let connection = output.connection(with: .video), connection.isVideoMirroringSupported {
                connection.automaticallyAdjustsVideoMirroring = false
                connection.isVideoMirrored = false
            }
            session.commitConfiguration()
            session.startRunning()

            self.session = session
            self.activeCaptureResolutionLabel = WebcamCaptureResolution.label(for: activePreset)
            self.neutralFace = nil
            self.previousLeftHandDepth = nil
            self.previousRightHandDepth = nil
            return true
        } catch {
            onStatus("Camera start failed: \(device.localizedName)")
            return false
        }
    }

    private func applyCaptureResolution(to session: AVCaptureSession) -> AVCaptureSession.Preset {
        for preset in captureResolution.fallbackPresets where session.canSetSessionPreset(preset) {
            session.sessionPreset = preset
            return preset
        }
        return session.sessionPreset
    }

    func stop() {
        session?.stopRunning()
        session = nil
        neutralFace = nil
        previousLeftHandDepth = nil
        previousRightHandDepth = nil
        pendingHeadReset = false
    }

    func resetHeadPose() {
        queue.async { [weak self] in
            self?.pendingHeadReset = true
        }
    }

    func updateSettings(_ settings: WebcamTrackingSettings) {
        settingsLock.lock()
        self.settings = settings
        settingsLock.unlock()
    }

    func setPreviewEnabled(_ enabled: Bool, generation: UInt64) {
        previewLock.lock()
        previewEnabled = enabled
        previewGeneration = generation
        previewLock.unlock()
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        let handler = VNImageRequestHandler(
            cmSampleBuffer: sampleBuffer,
            orientation: .up,
            options: [:]
        )

        do {
            try handler.perform([handRequest, faceRequest])
        } catch {
            return
        }

        let settings = currentSettings()
        let dimensions = sampleDimensions(sampleBuffer)
        let intrinsics = WebcamCameraIntrinsics.estimated(
            width: dimensions.width,
            height: dimensions.height
        )
        let handObservations = handRequest.results ?? []
        var leftHand: WebcamHandObservation?
        var rightHand: WebcamHandObservation?
        for observation in handObservations {
            guard let hand = buildHand(
                from: observation,
                intrinsics: intrinsics,
                settings: settings
            ) else { continue }
            if hand.hand.isLeft {
                leftHand = chooseBetterHand(current: leftHand, candidate: hand)
            } else {
                rightHand = chooseBetterHand(current: rightHand, candidate: hand)
            }
        }
        if leftHand == nil {
            previousLeftHandDepth = nil
        }
        if rightHand == nil {
            previousRightHandDepth = nil
        }

        let head = buildHead(from: faceRequest.results?.first, settings: settings)
        let frame = WebcamSourceFrame(
            cameraID: device.uniqueID,
            cameraName: device.localizedName,
            timestampNs: VideoReceiver.monotonicNs(),
            intrinsics: intrinsics,
            head: head,
            leftHand: leftHand,
            rightHand: rightHand
        )
        onFrame(frame)

        if let previewGeneration = currentPreviewGenerationIfEnabled() {
            onPreview(
                WebcamPreviewFrame(
                    cameraID: device.uniqueID,
                    cameraName: device.localizedName,
                    previewGeneration: previewGeneration,
                    timestampNs: frame.timestampNs,
                    image: makePreviewImage(from: sampleBuffer),
                    imageWidth: dimensions.width,
                    imageHeight: dimensions.height,
                    points: WebcamPreviewFrame.trackingPoints(
                        head: head,
                        leftHand: leftHand,
                        rightHand: rightHand
                    ),
                    debugText: WebcamPreviewFrame.depthDebugText(leftHand: leftHand, rightHand: rightHand)
                )
            )
        }
    }

    private func buildHand(
        from observation: VNHumanHandPoseObservation,
        intrinsics: WebcamCameraIntrinsics,
        settings: WebcamTrackingSettings
    ) -> WebcamHandObservation? {
        guard let points = try? observation.recognizedPoints(.all),
              let wrist = points[.wrist],
              wrist.confidence > 0.25,
              let middleMCP = points[.middleMCP],
              middleMCP.confidence > 0.25 else {
            return nil
        }

        let isLeft = WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: detectedHandedness(observation),
            wristNormalizedX: Float(wrist.location.x),
            cameraFacing: settings.cameraFacing
        )
        guard let rawDepth = estimatedHandDepth(
            points: points,
            intrinsics: intrinsics,
            settings: settings
        ) else {
            return nil
        }
        let depth = smoothHandDepth(rawDepth: rawDepth, isLeft: isLeft, settings: settings)
        let joints = buildJoints(points: points, depth: depth, settings: settings)
        let normalizedJoints = buildNormalizedJoints(points: points)
        let hand = WebcamTrackedHand(isLeft: isLeft, joints: joints, estimatedDepth: depth)
        return WebcamHandObservation(
            cameraID: device.uniqueID,
            hand: hand,
            normalizedJoints: normalizedJoints,
            quality: handQuality(points: points, wrist: wrist, middleMCP: middleMCP)
        )
    }

    private func detectedHandedness(_ observation: VNHumanHandPoseObservation) -> WebcamVisionHandedness {
        if #available(macOS 12.0, *) {
            switch observation.chirality {
            case .left:
                return .left
            case .right:
                return .right
            case .unknown:
                break
            @unknown default:
                break
            }
        }
        return .unknown
    }

    private func estimatedHandDepth(
        points: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint],
        intrinsics: WebcamCameraIntrinsics,
        settings: WebcamTrackingSettings
    ) -> Float? {
        func landmark(_ name: VNHumanHandPoseObservation.JointName) -> WebcamHandDepthLandmark? {
            guard let point = points[name], point.confidence > 0.15 else { return nil }
            return WebcamHandDepthLandmark(
                normalizedPosition: SIMD2<Float>(
                    Float(point.location.x),
                    Float(point.location.y)
                ),
                confidence: Float(point.confidence)
            )
        }

        var landmarks: [WebcamHandDepthJoint: WebcamHandDepthLandmark] = [:]
        landmarks[.wrist] = landmark(.wrist)
        landmarks[.middleMCP] = landmark(.middleMCP)
        landmarks[.indexMCP] = landmark(.indexMCP)
        landmarks[.littleMCP] = landmark(.littleMCP)
        return WebcamHandDepthEstimator.estimateDepth(
            landmarks: landmarks,
            intrinsics: intrinsics,
            settings: settings
        )
    }

    private func smoothHandDepth(
        rawDepth: Float,
        isLeft: Bool,
        settings: WebcamTrackingSettings
    ) -> Float {
        let previous = isLeft ? previousLeftHandDepth : previousRightHandDepth
        let smoothed = WebcamHandDepthEstimator.smoothDepth(
            previous: previous,
            raw: rawDepth,
            settings: settings
        )
        if isLeft {
            previousLeftHandDepth = smoothed
        } else {
            previousRightHandDepth = smoothed
        }
        return smoothed
    }

    private func buildJoints(
        points: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint],
        depth: Float,
        settings: WebcamTrackingSettings
    ) -> [SIMD3<Float>] {
        func rawPoint(_ name: VNHumanHandPoseObservation.JointName) -> SIMD3<Float>? {
            worldPoint(points[name], depth: depth, settings: settings)
        }

        let wrist = rawPoint(.wrist) ?? WebcamCameraToTrackingMapper(settings: settings).handPoint(
            normalizedPoint: nil,
            depth: depth
        )
        func point(
            _ name: VNHumanHandPoseObservation.JointName,
            fallback: SIMD3<Float>
        ) -> SIMD3<Float> {
            rawPoint(name) ?? fallback
        }
        func midpoint(_ lhs: SIMD3<Float>, _ rhs: SIMD3<Float>) -> SIMD3<Float> {
            (lhs + rhs) * 0.5
        }

        let thumbCMC = point(.thumbCMC, fallback: wrist)
        let thumbMP = point(.thumbMP, fallback: thumbCMC)
        let thumbIP = point(.thumbIP, fallback: thumbMP)
        let thumbTip = point(.thumbTip, fallback: thumbIP)

        let indexMCP = point(.indexMCP, fallback: wrist)
        let indexPIP = point(.indexPIP, fallback: indexMCP)
        let indexDIP = point(.indexDIP, fallback: indexPIP)
        let indexTip = point(.indexTip, fallback: indexDIP)

        let middleMCP = point(.middleMCP, fallback: midpoint(wrist, indexMCP))
        let middlePIP = point(.middlePIP, fallback: middleMCP)
        let middleDIP = point(.middleDIP, fallback: middlePIP)
        let middleTip = point(.middleTip, fallback: middleDIP)

        let ringMCP = point(.ringMCP, fallback: middleMCP)
        let ringPIP = point(.ringPIP, fallback: ringMCP)
        let ringDIP = point(.ringDIP, fallback: ringPIP)
        let ringTip = point(.ringTip, fallback: ringDIP)

        let littleMCP = point(.littleMCP, fallback: ringMCP)
        let littlePIP = point(.littlePIP, fallback: littleMCP)
        let littleDIP = point(.littleDIP, fallback: littlePIP)
        let littleTip = point(.littleTip, fallback: littleDIP)

        let palm = midpoint(wrist, middleMCP)

        return [
            palm,
            wrist,
            thumbCMC,
            thumbMP,
            thumbIP,
            thumbTip,
            midpoint(wrist, indexMCP),
            indexMCP,
            indexPIP,
            indexDIP,
            indexTip,
            midpoint(wrist, middleMCP),
            middleMCP,
            middlePIP,
            middleDIP,
            middleTip,
            midpoint(wrist, ringMCP),
            ringMCP,
            ringPIP,
            ringDIP,
            ringTip,
            midpoint(wrist, littleMCP),
            littleMCP,
            littlePIP,
            littleDIP,
            littleTip,
        ]
    }

    private func buildNormalizedJoints(
        points: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint]
    ) -> [SIMD2<Float>?] {
        func point(_ name: VNHumanHandPoseObservation.JointName) -> SIMD2<Float>? {
            guard let point = points[name], point.confidence > 0.15 else { return nil }
            return SIMD2<Float>(Float(point.location.x), Float(point.location.y))
        }

        let wrist = point(.wrist)
        func midpoint(_ lhs: SIMD2<Float>?, _ rhs: SIMD2<Float>?) -> SIMD2<Float>? {
            guard let lhs, let rhs else { return nil }
            return (lhs + rhs) * 0.5
        }

        let middleMCP = point(.middleMCP)
        return [
            midpoint(wrist, middleMCP),
            wrist,
            point(.thumbCMC),
            point(.thumbMP),
            point(.thumbIP),
            point(.thumbTip),
            midpoint(wrist, point(.indexMCP)),
            point(.indexMCP),
            point(.indexPIP),
            point(.indexDIP),
            point(.indexTip),
            midpoint(wrist, middleMCP),
            middleMCP,
            point(.middlePIP),
            point(.middleDIP),
            point(.middleTip),
            midpoint(wrist, point(.ringMCP)),
            point(.ringMCP),
            point(.ringPIP),
            point(.ringDIP),
            point(.ringTip),
            midpoint(wrist, point(.littleMCP)),
            point(.littleMCP),
            point(.littlePIP),
            point(.littleDIP),
            point(.littleTip),
        ]
    }

    private func worldPoint(
        _ point: VNRecognizedPoint?,
        depth: Float,
        settings: WebcamTrackingSettings
    ) -> SIMD3<Float>? {
        guard let point, point.confidence > 0.15 else {
            return nil
        }

        return WebcamCameraToTrackingMapper(settings: settings).handPoint(
            normalizedPoint: SIMD2<Float>(Float(point.location.x), Float(point.location.y)),
            depth: depth
        )
    }

    private func handQuality(
        points: [VNHumanHandPoseObservation.JointName: VNRecognizedPoint],
        wrist: VNRecognizedPoint,
        middleMCP: VNRecognizedPoint
    ) -> Float {
        let confidences = points.values.map { Float($0.confidence) }
        let averageConfidence = confidences.reduce(0, +) / max(Float(confidences.count), 1)
        let dx = Float(middleMCP.location.x - wrist.location.x)
        let dy = Float(middleMCP.location.y - wrist.location.y)
        let scaleScore = min(max(sqrt(dx * dx + dy * dy) / 0.18, 0), 1)
        return min(max(averageConfidence * 0.75 + scaleScore * 0.25, 0), 1)
    }

    private func chooseBetterHand(
        current: WebcamHandObservation?,
        candidate: WebcamHandObservation
    ) -> WebcamHandObservation {
        guard let current else { return candidate }
        return candidate.quality > current.quality ? candidate : current
    }

    private func buildHead(
        from observation: VNFaceObservation?,
        settings: WebcamTrackingSettings
    ) -> WebcamHeadObservation? {
        guard let observation else { return nil }

        let sample = FaceSample(
            center: SIMD2<Float>(
                Float(observation.boundingBox.midX),
                Float(observation.boundingBox.midY)
            ),
            width: Float(observation.boundingBox.width),
            yaw: observation.yaw?.floatValue ?? 0,
            pitch: facePitch(from: observation),
            roll: observation.roll?.floatValue ?? 0
        )

        if neutralFace == nil || pendingHeadReset {
            neutralFace = sample
            pendingHeadReset = false
        }
        guard let neutralFace else { return nil }

        let mapper = WebcamCameraToTrackingMapper(settings: settings)
        let rawPosition = mapper.headPosition(
            center: sample.center,
            neutralCenter: neutralFace.center,
            faceWidth: sample.width,
            neutralFaceWidth: neutralFace.width
        )
        let rawEuler = mapper.headYawPitchRoll(
            yawPitchRoll: SIMD3<Float>(sample.yaw, sample.pitch, sample.roll),
            neutralYawPitchRoll: SIMD3<Float>(
                neutralFace.yaw,
                neutralFace.pitch,
                neutralFace.roll
            )
        )

        let area = Float(observation.boundingBox.width * observation.boundingBox.height)
        let quality = min(max(Float(observation.confidence) * 0.7 + min(area * 10, 1) * 0.3, 0), 1)
        return WebcamHeadObservation(
            cameraID: device.uniqueID,
            head: WebcamHeadPose(
                position: rawPosition,
                orientation: WebcamTrackingMath.headQuaternion(fromYawPitchRoll: rawEuler),
                yawPitchRoll: rawEuler
            ),
            normalizedCenter: sample.center,
            quality: quality
        )
    }

    private func facePitch(from observation: VNFaceObservation) -> Float {
        let visionPitch = observation.pitch?.floatValue ?? 0
        guard abs(visionPitch) < 0.001 else { return visionPitch }
        return landmarkPitch(from: observation)
    }

    private func landmarkPitch(from observation: VNFaceObservation) -> Float {
        guard let landmarks = observation.landmarks,
              let leftEye = averagePoint(landmarks.leftEye),
              let rightEye = averagePoint(landmarks.rightEye),
              let nose = averagePoint(landmarks.nose),
              let mouth = averagePoint(landmarks.outerLips) ?? averagePoint(landmarks.innerLips) else {
            return 0
        }

        let eyeY = (leftEye.y + rightEye.y) * 0.5
        let mouthY = mouth.y
        let faceSpan = max(abs(eyeY - mouthY), 0.05)
        let noseCenterY = (eyeY + mouthY) * 0.5
        return ((nose.y - noseCenterY) / faceSpan) * 0.45
    }

    private func averagePoint(_ region: VNFaceLandmarkRegion2D?) -> SIMD2<Float>? {
        guard let region, region.pointCount > 0 else { return nil }

        var sum = SIMD2<Float>(0, 0)
        for index in 0..<region.pointCount {
            let point = region.normalizedPoints[index]
            sum += SIMD2<Float>(Float(point.x), Float(point.y))
        }
        return sum / Float(region.pointCount)
    }

    private func sampleDimensions(_ sampleBuffer: CMSampleBuffer) -> (width: Int, height: Int) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return (640, 480)
        }
        return (CVPixelBufferGetWidth(pixelBuffer), CVPixelBufferGetHeight(pixelBuffer))
    }

    private func makePreviewImage(from sampleBuffer: CMSampleBuffer) -> CGImage? {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return nil }
        let image = CIImage(cvPixelBuffer: pixelBuffer)
        return imageContext.createCGImage(image, from: image.extent)
    }

    private func currentPreviewGenerationIfEnabled() -> UInt64? {
        previewLock.lock()
        let enabled = previewEnabled
        let generation = previewGeneration
        previewLock.unlock()
        return enabled ? generation : nil
    }

    private func currentSettings() -> WebcamTrackingSettings {
        settingsLock.lock()
        let current = settings
        settingsLock.unlock()
        return current
    }
}

private struct FaceSample {
    var center: SIMD2<Float>
    var width: Float
    var yaw: Float
    var pitch: Float
    var roll: Float
}
#endif
