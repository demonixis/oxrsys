// SPDX-License-Identifier: MPL-2.0

// StereoRenderer.swift — Metal renderer that displays decoded H.265 video frames.
// Renders a full-screen textured quad with the stereo side-by-side video.

import CoreVideo
import Metal
import MetalKit

public enum StereoDisplayMode: UInt32, Sendable {
    case stereoView = 0
    case monoPreview = 1
}

private struct FragmentUniforms {
    var ipdOffset: Float
    var displayMode: UInt32
}

public final class StereoRenderer: NSObject, @unchecked Sendable {
    public let device: MTLDevice
    public let commandQueue: MTLCommandQueue

    private var pipelineYCbCr: MTLRenderPipelineState?
    private var textureCache: CVMetalTextureCache?

    // Latest decoded frame (written by decoder thread, read by render thread)
    private var unfairLock = os_unfair_lock()
    private var latestPixelBuffer: CVPixelBuffer?
    private var frameCount: UInt64 = 0

    // IPD offset in UV space: written infrequently by main thread, read by render thread.
    // Float reads/writes are atomic on ARM64 so no lock needed.
    public nonisolated(unsafe) var ipdOffset: Float = 0
    public nonisolated(unsafe) var displayMode: StereoDisplayMode = .stereoView

    public init?(device: MTLDevice) {
        self.device = device
        guard let queue = device.makeCommandQueue() else { return nil }
        self.commandQueue = queue
        super.init()

        setupPipeline()
        setupTextureCache()
    }

    /// Called from the H265Decoder callback (background thread).
    public func submitFrame(_ pixelBuffer: CVPixelBuffer) {
        os_unfair_lock_lock(&unfairLock)
        latestPixelBuffer = pixelBuffer
        frameCount += 1
        os_unfair_lock_unlock(&unfairLock)
    }

    // MARK: - Setup

    private func setupPipeline() {
        // Xcode compiles Metal files in Swift Package targets into default.metallib
        // and places it inside the package's auto-generated resource bundle (Bundle.module).
        // makeDefaultLibrary() would look in the main app bundle and miss it.
        let library: MTLLibrary?
        if let url = Bundle.module.url(forResource: "default", withExtension: "metallib") {
            library = try? device.makeLibrary(URL: url)
        } else {
            library = device.makeDefaultLibrary()
        }
        guard let library else {
            print("[Renderer] Failed to create Metal library")
            return
        }

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "stereoVertex")
        desc.fragmentFunction = library.makeFunction(name: "stereoFragmentYCbCr")
        desc.colorAttachments[0].pixelFormat = .bgra8Unorm

        do {
            pipelineYCbCr = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            print("[Renderer] Pipeline error: \(error)")
        }
    }

    private func setupTextureCache() {
        var cache: CVMetalTextureCache?
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache)
        textureCache = cache
    }

    // MARK: - Texture from CVPixelBuffer

    private func makeTexture(from pixelBuffer: CVPixelBuffer, plane: Int,
                              format: MTLPixelFormat) -> MTLTexture? {
        guard let cache = textureCache else { return nil }

        let width = CVPixelBufferGetWidthOfPlane(pixelBuffer, plane)
        let height = CVPixelBufferGetHeightOfPlane(pixelBuffer, plane)

        var cvTexture: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pixelBuffer, nil,
            format, width, height, plane, &cvTexture
        )
        guard status == kCVReturnSuccess, let cvTexture else { return nil }
        return CVMetalTextureGetTexture(cvTexture)
    }
}

extension StereoRenderer: MTKViewDelegate {
    public nonisolated func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // No-op for now
    }

    public nonisolated func draw(in view: MTKView) {
        os_unfair_lock_lock(&unfairLock)
        let pixelBuffer = latestPixelBuffer
        os_unfair_lock_unlock(&unfairLock)

        guard let pixelBuffer,
              let pipeline = pipelineYCbCr,
              let drawable = view.currentDrawable,
              let passDesc = view.currentRenderPassDescriptor else { return }

        // Create Y and CbCr textures from the biplanar pixel buffer
        guard let texY = makeTexture(from: pixelBuffer, plane: 0, format: .r8Unorm),
              let texCbCr = makeTexture(from: pixelBuffer, plane: 1, format: .rg8Unorm) else {
            return
        }

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: passDesc) else {
            return
        }

        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(texY, index: 0)
        encoder.setFragmentTexture(texCbCr, index: 1)
        var uniforms = FragmentUniforms(
            ipdOffset: ipdOffset,
            displayMode: displayMode.rawValue
        )
        encoder.setFragmentBytes(&uniforms, length: MemoryLayout<FragmentUniforms>.size, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
        encoder.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}
