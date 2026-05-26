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
    private let worldTracking = WorldTrackingProvider()
    private let textureCache: CVMetalTextureCache
    private let pipelineState: MTLRenderPipelineState
    private let endFrameEvent: MTLSharedEvent

    private var committedFrameIndex: UInt64 = UInt64(ImmersiveRendererConstants.maxBuffersInFlight)

    init(layerRenderer: LayerRenderer, appModel: AppModel) {
        self.layerRenderer = layerRenderer
        self.appModel = appModel
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
        pipelineDescriptor.maxVertexAmplificationCount = layerRenderer.properties.viewCount
        self.pipelineState = try! device.makeRenderPipelineState(descriptor: pipelineDescriptor)

        self.endFrameEvent = device.makeSharedEvent()!
        self.endFrameEvent.signaledValue = committedFrameIndex
    }

    private func startARSession(_ arSession: ARKitSession) async {
        do {
            try await arSession.run([worldTracking])
        } catch {
            print("[VisionImmersive] Failed to start ARKitSession: \(error)")
        }
    }

    @MainActor
    static func startRenderLoop(_ layerRenderer: LayerRenderer, appModel: AppModel, arSession: ARKitSession) {
        Task(executorPreference: ImmersiveRendererTaskExecutor.shared) {
            let renderer = ImmersiveRenderer(layerRenderer: layerRenderer, appModel: appModel)
            await renderer.startARSession(arSession)
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

        let renderPassDescriptor = MTLRenderPassDescriptor()
        renderPassDescriptor.colorAttachments[0].texture = drawable.colorTextures[0]
        renderPassDescriptor.colorAttachments[0].loadAction = .clear
        renderPassDescriptor.colorAttachments[0].storeAction = .store
        renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
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
