// SPDX-License-Identifier: MPL-2.0

import ARKit
import CompositorServices
import CoreVideo
import Metal
import os

nonisolated private enum ImmersiveRendererConstants {
    static let maxBuffersInFlight = 3
}

extension LayerRenderer.Clock.Instant {
    nonisolated var timeInterval: TimeInterval {
        let components = LayerRenderer.Clock.Instant.epoch.duration(to: self).components
        let nanoseconds = TimeInterval(components.attoseconds / 1_000_000_000)
        return TimeInterval(components.seconds) + (nanoseconds / TimeInterval(NSEC_PER_SEC))
    }
}

final class ImmersiveRendererTaskExecutor: TaskExecutor {
    private let queue = DispatchQueue(label: "oxr.visionos.immersive-render", qos: .userInteractive)

    func enqueue(_ job: UnownedJob) {
        queue.async {
            job.runSynchronously(on: self.asUnownedSerialExecutor())
        }
    }

    nonisolated func asUnownedSerialExecutor() -> UnownedTaskExecutor {
        UnownedTaskExecutor(ordinary: self)
    }

    static let shared = ImmersiveRendererTaskExecutor()
}

actor ImmersiveRenderer {
    private let layerRenderer: LayerRenderer
    private unowned let appModel: AppModel
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let worldTracking: WorldTrackingProvider
    private let textureCache: CVMetalTextureCache
    private let pipelineState: MTLRenderPipelineState
    private let endFrameEvent: MTLSharedEvent

    private var committedFrameIndex: UInt64 = UInt64(ImmersiveRendererConstants.maxBuffersInFlight)
    private var didLogProjection = false

    init(layerRenderer: LayerRenderer, appModel: AppModel, worldTracking: WorldTrackingProvider) {
        self.layerRenderer = layerRenderer
        self.appModel = appModel
        self.worldTracking = worldTracking
        self.device = layerRenderer.device
        self.commandQueue = layerRenderer.device.makeCommandQueue()!

        var cache: CVMetalTextureCache?
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache)
        self.textureCache = cache!

        let library = device.makeDefaultLibrary()!
        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.label = "VisionStereoPipeline"
        pipelineDescriptor.vertexFunction = library.makeFunction(name: "stereoImmersiveVertex")
        pipelineDescriptor.fragmentFunction = library.makeFunction(name: "stereoImmersiveFragment")
        pipelineDescriptor.colorAttachments[0].pixelFormat = layerRenderer.configuration.colorFormat
        pipelineDescriptor.depthAttachmentPixelFormat = layerRenderer.configuration.depthFormat
        pipelineDescriptor.maxVertexAmplificationCount = layerRenderer.properties.viewCount
        self.pipelineState = try! device.makeRenderPipelineState(descriptor: pipelineDescriptor)

        self.endFrameEvent = device.makeSharedEvent()!
        self.endFrameEvent.signaledValue = committedFrameIndex
    }

    @MainActor
    static func startRenderLoop(_ layerRenderer: LayerRenderer, appModel: AppModel, worldTracking: WorldTrackingProvider) {
        Task(executorPreference: ImmersiveRendererTaskExecutor.shared) {
            let renderer = ImmersiveRenderer(layerRenderer: layerRenderer, appModel: appModel, worldTracking: worldTracking)
            await renderer.renderLoop()
        }
    }

    private func renderLoop() async {
        while true {
            if layerRenderer.state == .invalidated {
                await MainActor.run {
                    appModel.immersiveSpaceDidClose()
                }
                return
            }

            if layerRenderer.state == .paused {
                await MainActor.run {
                    appModel.immersiveSpaceState = .inTransition
                }
                layerRenderer.waitUntilRunning()
                continue
            }

            autoreleasepool {
                renderFrame()
            }
        }
    }

    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }
        guard endFrameEvent.wait(
            untilSignaledValue: committedFrameIndex - UInt64(ImmersiveRendererConstants.maxBuffersInFlight),
            timeoutMS: 10_000
        ) else {
            return
        }

        frame.startUpdate()
        frame.endUpdate()

        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        guard let commandBuffer = commandQueue.makeCommandBuffer() else { return }

        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else { return }

        frame.startSubmission()
        for drawable in drawables {
            render(drawable: drawable, commandBuffer: commandBuffer)
        }

        committedFrameIndex += 1
        commandBuffer.encodeSignalEvent(endFrameEvent, value: committedFrameIndex)
        commandBuffer.commit()
        frame.endSubmission()
    }

    private func render(drawable: LayerRenderer.Drawable, commandBuffer: MTLCommandBuffer) {
        let presentationTime = drawable.frameTiming.presentationTime.timeInterval
        drawable.deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: presentationTime)

        publishEyeProjection(drawable)

        let renderPassDescriptor = MTLRenderPassDescriptor()
        renderPassDescriptor.colorAttachments[0].texture = drawable.colorTextures[0]
        renderPassDescriptor.colorAttachments[0].loadAction = .clear
        renderPassDescriptor.colorAttachments[0].storeAction = .store
        renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        // visionOS reprojects each presented frame using the depth buffer; with no valid depth
        // the device drops every frame (black) while the simulator does not. Clear the depth to
        // a fixed head-locked distance so the compositor has a real surface to reproject.
        let clip = drawable.computeProjection(viewIndex: 0) * SIMD4<Float>(0, 0, -2.0, 1)
        renderPassDescriptor.depthAttachment.texture = drawable.depthTextures[0]
        renderPassDescriptor.depthAttachment.loadAction = .clear
        renderPassDescriptor.depthAttachment.storeAction = .store
        renderPassDescriptor.depthAttachment.clearDepth = clip.w != 0 ? Double(clip.z / clip.w) : 0
        renderPassDescriptor.rasterizationRateMap = drawable.rasterizationRateMaps.first
        if layerRenderer.configuration.layout == .layered {
            renderPassDescriptor.renderTargetArrayLength = drawable.views.count
        }

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPassDescriptor) else {
            return
        }

        encoder.label = "Vision Stereo Encoder"
        encoder.setRenderPipelineState(pipelineState)

        let viewports = drawable.views.map { $0.textureMap.viewport }
        encoder.setViewports(viewports)

        if drawable.views.count > 1 {
            var viewMappings = (0..<drawable.views.count).map {
                MTLVertexAmplificationViewMapping(
                    viewportArrayIndexOffset: UInt32($0),
                    renderTargetArrayIndexOffset: UInt32($0)
                )
            }
            encoder.setVertexAmplificationCount(viewports.count, viewMappings: &viewMappings)
        }

        if let pixelBuffer = appModel.currentPixelBuffer(),
           let luma = makeTexture(from: pixelBuffer, plane: 0, format: .r8Unorm),
           let chroma = makeTexture(from: pixelBuffer, plane: 1, format: .rg8Unorm) {
            encoder.setFragmentTexture(luma, index: 0)
            encoder.setFragmentTexture(chroma, index: 1)
        }

        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        drawable.encodePresent(commandBuffer: commandBuffer)
    }

    /// Sends the device's real per-eye FOV (radians, OpenXR signed angles) and IPD so the
    /// runtime renders the matching frustum instead of its symmetric fallback FOV. visionOS
    /// view tangents are positive magnitudes in (left, right, up, down) order, so negate the
    /// left and down components to produce OpenXR's signed XrFovf angles.
    private func publishEyeProjection(_ drawable: LayerRenderer.Drawable) {
        guard let leftView = drawable.views.first else { return }
        let t = leftView.tangents
        let fovAngles = SIMD4<Float>(-atan(t.x), atan(t.y), atan(t.z), -atan(t.w))

        let leftEye = leftView.transform.columns.3
        let rightEye = (drawable.views.count > 1 ? drawable.views[1] : leftView).transform.columns.3
        let dx = rightEye.x - leftEye.x, dy = rightEye.y - leftEye.y, dz = rightEye.z - leftEye.z
        let ipd = (dx * dx + dy * dy + dz * dz).squareRoot()

        if !didLogProjection {
            didLogProjection = true
            print("[ProjDiag] L.tangents=(\(t.x), \(t.y), \(t.z), \(t.w))")
            if drawable.views.count > 1 {
                let rt = drawable.views[1].tangents
                print("[ProjDiag] R.tangents=(\(rt.x), \(rt.y), \(rt.z), \(rt.w))")
            }
            print("[ProjDiag] fovAngles(rad)=(\(fovAngles.x), \(fovAngles.y), \(fovAngles.z), \(fovAngles.w)) ipd=\(ipd)")
        }

        appModel.updateEyeProjection(fovAngles: fovAngles, ipd: ipd)
    }

    private func makeTexture(from pixelBuffer: CVPixelBuffer, plane: Int, format: MTLPixelFormat) -> MTLTexture? {
        let width = CVPixelBufferGetWidthOfPlane(pixelBuffer, plane)
        let height = CVPixelBufferGetHeightOfPlane(pixelBuffer, plane)

        var cvTexture: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            textureCache,
            pixelBuffer,
            nil,
            format,
            width,
            height,
            plane,
            &cvTexture
        )
        guard status == kCVReturnSuccess, let cvTexture else {
            return nil
        }
        return CVMetalTextureGetTexture(cvTexture)
    }
}
