// SPDX-License-Identifier: MPL-2.0

#import "VideoEncoder.h"
#import "Config.h"
#import "encoder/EncoderIpcProtocol.h"
#import "encoder/InProcessEncoderTransport.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <simd/simd.h>

#import <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace
{

struct MetalFoveationUniforms
{
    vector_float2 centerSize;
    vector_float2 centerShift;
    vector_float2 edgeRatio;
    vector_float2 eyeSizeRatio;
};

// Encoder compute library: axis-aligned foveated encoding shader logic adapted
// from ALVR's AADT compression shader (MIT licensed).
constexpr const char* kFoveationMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct FoveationUniforms
{
    float2 centerSize;
    float2 centerShift;
    float2 edgeRatio;
    float2 eyeSizeRatio;
};

static float compress_axis(float eyeUv, float centerSize, float centerShift, float edgeRatio)
{
    float c0 = (1.0 - centerSize) * 0.5;
    float c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    float c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    float loBound = c0 * (centerShift + 1.0) / c2;
    float hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;

    float center = eyeUv * c2 / edgeRatio + c1;
    float d2 = eyeUv * c2;
    float d3 = (eyeUv - 1.0) * c2 + 1.0;
    float g1 = loBound > 0.0 ? eyeUv / loBound : 1.0;
    float g2 = (1.0 - hiBound) > 0.0 ? (1.0 - eyeUv) / (1.0 - hiBound) : 1.0;
    float leftEdge = g1 * center + (1.0 - g1) * d2;
    float rightEdge = g2 * center + (1.0 - g2) * d3;

    if (eyeUv < loBound)
    {
        return leftEdge;
    }
    if (eyeUv > hiBound)
    {
        return rightEdge;
    }
    return center;
}

kernel void foveation_kernel(texture2d<float, access::sample> leftTexture [[texture(0)]],
                             texture2d<float, access::sample> rightTexture [[texture(1)]],
                             texture2d<float, access::write> outputTexture [[texture(2)]],
                             sampler linearSampler [[sampler(0)]],
                             constant FoveationUniforms& params [[buffer(0)]],
                             uint2 gid [[thread_position_in_grid]])
{
    uint outputWidth = outputTexture.get_width();
    uint outputHeight = outputTexture.get_height();
    if (gid.x >= outputWidth || gid.y >= outputHeight)
    {
        return;
    }

    uint eyeWidth = max(outputWidth / 2, 1u);
    bool rightEye = gid.x >= eyeWidth;
    uint localX = rightEye ? gid.x - eyeWidth : gid.x;
    float2 uv = (float2(localX, gid.y) + float2(0.5)) / float2(eyeWidth, outputHeight);
    float2 eyeUv = uv / max(params.eyeSizeRatio, float2(0.0001));
    float2 compressedUv;
    compressedUv.x = compress_axis(eyeUv.x, params.centerSize.x, params.centerShift.x, params.edgeRatio.x);
    compressedUv.y = compress_axis(eyeUv.y, params.centerSize.y, params.centerShift.y, params.edgeRatio.y);
    compressedUv = clamp(compressedUv, float2(0.0), float2(1.0));

    float4 color = rightEye
        ? rightTexture.sample(linearSampler, compressedUv)
        : leftTexture.sample(linearSampler, compressedUv);
    outputTexture.write(color, gid);
}
)METAL";

const char* VideoCodecName(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return "H.264";
        case oxr::protocol::VideoCodec::AV1:
            return "AV1";
        case oxr::protocol::VideoCodec::H265:
        default:
            return "H.265";
    }
}

void EncodeWaitForFrameImage(id<MTLCommandBuffer> commandBuffer, const FrameImageSource& source)
{
    if (commandBuffer == nil ||
        source.sync.api != GraphicsApi::Metal ||
        !source.sync.IsValid())
    {
        return;
    }

    id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)source.sync.waitObject.get();
    if (event != nil)
    {
        [commandBuffer encodeWaitForEvent:event value:source.sync.waitValue];
    }
}

bool IsFiniteRatio(float value)
{
    return std::isfinite(value) && value > 0.0f && value <= 1.0f;
}

bool IsFiniteNormalized(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool IsFoveationSettingsValid(const VideoEncoder::FoveationSettings& settings,
                              uint32_t sourceEyeWidth,
                              uint32_t sourceEyeHeight)
{
    return settings.enabled &&
           settings.targetEyeWidth == sourceEyeWidth &&
           settings.targetEyeHeight == sourceEyeHeight &&
           IsFiniteRatio(settings.eyeWidthRatio) &&
           IsFiniteRatio(settings.eyeHeightRatio) &&
           IsFiniteNormalized(settings.centerSizeX) &&
           IsFiniteNormalized(settings.centerSizeY) &&
           std::isfinite(settings.centerShiftX) &&
           std::isfinite(settings.centerShiftY) &&
           std::isfinite(settings.edgeRatioX) &&
           std::isfinite(settings.edgeRatioY) &&
           settings.edgeRatioX > 1.0f &&
           settings.edgeRatioY > 1.0f;
}

bool TextureAllowsUsage(id<MTLTexture> texture, MTLTextureUsage requiredUsage)
{
    if (texture == nil)
    {
        return false;
    }
    const MTLTextureUsage declaredUsage = texture.usage;
    return declaredUsage == MTLTextureUsageUnknown ||
           (declaredUsage & requiredUsage) == requiredUsage;
}

id<MTLComputePipelineState> CreateComputePipeline(id<MTLDevice> device, NSString* functionName)
{
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kFoveationMetalSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil)
    {
        spdlog::error("VideoEncoder: Failed to compile encoder shader library: {}",
                      error != nil ? error.localizedDescription.UTF8String : "unknown error");
        return nil;
    }

    id<MTLFunction> kernelFunction = [library newFunctionWithName:functionName];
    if (kernelFunction == nil)
    {
        spdlog::error("VideoEncoder: Failed to load compute shader entry point {}",
                      functionName.UTF8String);
        [library release];
        return nil;
    }

    error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:kernelFunction error:&error];
    if (pipeline == nil)
    {
        spdlog::error("VideoEncoder: Failed to create compute pipeline {}: {}",
                      functionName.UTF8String,
                      error != nil ? error.localizedDescription.UTF8String : "unknown error");
    }

    [kernelFunction release];
    [library release];
    return pipeline;
}

id<MTLSamplerState> CreateLinearClampSampler(id<MTLDevice> device)
{
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    [descriptor release];
    return sampler;
}

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& graphicsContext)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)graphicsContext.metalDevice;
    if (device == nil)
    {
        return false;
    }

    id<MTLComputePipelineState> pipeline = CreateComputePipeline(device, @"foveation_kernel");
    id<MTLSamplerState> sampler = CreateLinearClampSampler(device);
    const bool supported = pipeline != nil && sampler != nil;
    [pipeline release];
    [sampler release];
    return supported;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                               uint32_t bitrateMbps, const GraphicsContext& graphicsContext,
                               oxr::protocol::VideoCodec codec)
{
    static_assert(SlotCount == oxrsys::encoder::ipc::kSlotCount,
                  "compose ring depth must match the IPC surface-slot count");

    Shutdown();

    if (codec == oxr::protocol::VideoCodec::AV1)
    {
        spdlog::error("VideoEncoder: AV1 is not implemented in the VideoToolbox path");
        return false;
    }

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    // 4:2:0 H.264 output and the side-by-side eye split both want even
    // dimensions (2496x1312 in practice).
    if (width_ == 0 || height_ == 0 || (width_ % 2u) != 0 || (height_ % 2u) != 0)
    {
        spdlog::error("VideoEncoder: encoding requires even dimensions, got {}x{}",
                      width_, height_);
        return false;
    }
    fps_ = fps;
    bitrateMbps_ = bitrateMbps;
    codec_ = codec;
    graphicsContext_ = graphicsContext;
    videoToolbox_.metalDevice = graphicsContext.metalDevice;
    shuttingDown_.store(false);
    foveationValidationWarningLogged_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);
    frameCount_ = 0;

    id<MTLDevice> device = (__bridge id<MTLDevice>)graphicsContext.metalDevice;
    if (device == nil)
    {
        spdlog::error("VideoEncoder: No Metal device");
        return false;
    }

    videoToolbox_.commandQueue = (void*)[device newCommandQueue];
    videoToolbox_.scaler = (void*)[[MPSImageBilinearScale alloc] initWithDevice:device];
    if (foveationSettings_.enabled)
    {
        videoToolbox_.foveationPipeline = (void*)CreateComputePipeline(device, @"foveation_kernel");
        videoToolbox_.foveationSampler = (void*)CreateLinearClampSampler(device);
        if (videoToolbox_.foveationPipeline == nullptr || videoToolbox_.foveationSampler == nullptr)
        {
            spdlog::error("VideoEncoder: Foveated encoding was negotiated but the shader is unavailable");
            Shutdown();
            return false;
        }
    }
    CVMetalTextureCacheRef cache = nullptr;
    CVReturn cvResult = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, device, nullptr, &cache);
    if (cvResult != kCVReturnSuccess)
    {
        spdlog::error("VideoEncoder: Failed to create Metal texture cache: {}", cvResult);
        return false;
    }
    videoToolbox_.textureCache = cache;

    NSDictionary* poolConfig = @{
        (NSString*)kCVPixelBufferPoolMinimumBufferCountKey: @(SlotCount),
    };
    NSDictionary* poolAttrs = @{
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
    };

    CVPixelBufferPoolRef pool = nullptr;
    cvResult = CVPixelBufferPoolCreate(
        kCFAllocatorDefault,
        (__bridge CFDictionaryRef)poolConfig,
        (__bridge CFDictionaryRef)poolAttrs,
        &pool);
    if (cvResult != kCVReturnSuccess)
    {
        spdlog::error("VideoEncoder: Failed to create pixel buffer pool: {}", cvResult);
        Shutdown();
        return false;
    }
    videoToolbox_.pixelBufferPool = pool;

    MTLTextureDescriptor* tmpDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:MAX((uint32_t)1, eyeWidth_)
                                    height:MAX((uint32_t)1, height_)
                                 mipmapped:NO];
    tmpDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    tmpDesc.storageMode = MTLStorageModePrivate;

    MTLTextureDescriptor* foveatedScratchDesc = nil;
    if (foveationSettings_.enabled)
    {
        foveatedScratchDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:MAX((uint32_t)1, width_)
                                        height:MAX((uint32_t)1, height_)
                                     mipmapped:NO];
        foveatedScratchDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        foveatedScratchDesc.storageMode = MTLStorageModePrivate;
    }

    // Compose target: all paths write BGRA here. The texture is a view of the
    // slot's CVPixelBuffer (IOSurface-backed), so composing IS producing the
    // encoder input — no conversion or copy afterwards. Write-only: blit
    // destinations need no usage bit, shaderWrite covers the MPS
    // mono-downscale path, and nothing shader-reads it (the rgb_to_nv12
    // kernel was its last reader).
    NSDictionary* compositeTexAttrs = @{
        (NSString*)kCVMetalTextureUsage: @(MTLTextureUsageShaderWrite),
    };

    for (size_t i = 0; i < SlotCount; i++)
    {
        CVPixelBufferRef pixelBuffer = nullptr;
        cvResult = CVPixelBufferPoolCreatePixelBuffer(
            kCFAllocatorDefault, (CVPixelBufferPoolRef)videoToolbox_.pixelBufferPool, &pixelBuffer);
        if (cvResult != kCVReturnSuccess || pixelBuffer == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to preallocate pixel buffer slot {}", i);
            Shutdown();
            return false;
        }

        // BGRA input: these tags declare the color space VT's internal
        // RGB->YCbCr conversion must target (BT.709 video-range), matching the
        // session properties and the client's decode contract.
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
            kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
            kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
            kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);

        CVMetalTextureRef compositeTexture = nullptr;
        cvResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            (CVMetalTextureCacheRef)videoToolbox_.textureCache,
            pixelBuffer,
            (__bridge CFDictionaryRef)compositeTexAttrs,
            MTLPixelFormatBGRA8Unorm,
            width_,
            height_,
            0,
            &compositeTexture);
        if (cvResult != kCVReturnSuccess || compositeTexture == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to create composite texture for slot {}", i);
            CVPixelBufferRelease(pixelBuffer);
            Shutdown();
            return false;
        }

        slots_[i].pixelBuffer = pixelBuffer;
        slots_[i].compositeTexture = compositeTexture;
        slots_[i].tmpLeftTexture = (void*)[device newTextureWithDescriptor:tmpDesc];
        slots_[i].tmpRightTexture = (void*)[device newTextureWithDescriptor:tmpDesc];
        if (foveatedScratchDesc != nil)
        {
            slots_[i].foveatedScratchTexture =
                (void*)[device newTextureWithDescriptor:foveatedScratchDesc];
            if (slots_[i].foveatedScratchTexture == nullptr)
            {
                spdlog::error("VideoEncoder: Failed to create foveated compute scratch texture for slot {}", i);
                Shutdown();
                return false;
            }
        }
        slots_[i].inUse = false;
    }

    // Session creation, the full LL-RC property set, and the output path live
    // in the VideoToolbox encode engine behind the in-process transport seam.
    const ConfigValues config = Config::Get().GetValues();
    oxrsys::encoder::EncoderConfig engineConfig;
    engineConfig.width = width_;
    engineConfig.height = height_;
    engineConfig.fps = fps_;
    engineConfig.bitrateMbps = bitrateMbps_;
    engineConfig.codec = codec_;
    engineConfig.tenBit = tenBit_;
    engineConfig.encoderPreset = config.encoderPreset;
    engineConfig.keyframeIntervalSec = config.keyframeIntervalSec;

    std::shared_ptr<oxrsys::encoder::IEncoderTransport> transport =
        injectedTransport_ != nullptr
            ? injectedTransport_
            : std::make_shared<oxrsys::encoder::InProcessEncoderTransport>();
    if (!transport->Configure(engineConfig))
    {
        Shutdown();
        return false;
    }
    transport_ = std::move(transport);

    spdlog::info("VideoEncoder: Initialized {} encoder {}x{} @ {}fps, {}Mbps (slots={}, keyframe={}s, preset={})",
                  VideoCodecName(codec_), width, height, fps, bitrateMbps, SlotCount,
                  config.keyframeIntervalSec, config.encoderPreset);
    return true;
}

void VideoEncoder::Shutdown()
{
    if (!IsInitialized())
    {
        shuttingDown_.store(true);
        return;
    }

    shuttingDown_.store(true);

    if (transport_ != nullptr)
    {
        // Flush + bounded (<=200ms) drain of in-flight frames BEFORE the
        // session teardown: late Metal completed handlers hold their own
        // session retains (via the pending-frame tokens), but destroying the
        // session early would yank it out from under an encode in flight.
        transport_->Drain();
        transport_->Shutdown();
        transport_.reset();
    }

    DestroySlots();

    if (videoToolbox_.scaler != nullptr)
    {
        [(MPSImageBilinearScale*)videoToolbox_.scaler release];
        videoToolbox_.scaler = nullptr;
    }
    if (videoToolbox_.foveationPipeline != nullptr)
    {
        [(id<MTLComputePipelineState>)videoToolbox_.foveationPipeline release];
        videoToolbox_.foveationPipeline = nullptr;
    }
    if (videoToolbox_.foveationSampler != nullptr)
    {
        [(id<MTLSamplerState>)videoToolbox_.foveationSampler release];
        videoToolbox_.foveationSampler = nullptr;
    }
    if (videoToolbox_.commandQueue != nullptr)
    {
        [(id<MTLCommandQueue>)videoToolbox_.commandQueue release];
        videoToolbox_.commandQueue = nullptr;
    }
    if (videoToolbox_.pixelBufferPool != nullptr)
    {
        CFRelease(videoToolbox_.pixelBufferPool);
        videoToolbox_.pixelBufferPool = nullptr;
    }
    if (videoToolbox_.textureCache != nullptr)
    {
        CFRelease(videoToolbox_.textureCache);
        videoToolbox_.textureCache = nullptr;
    }

    spdlog::info("VideoEncoder: Shut down (submitted={} dropped={})",
                  frameCount_, droppedFrameCount_.load());
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                           OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                 OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool stereo,
                                   int64_t timestampNs, OnNalUnitCallback callback,
                                   OnFrameEncodedCallback frameCallback)
{
    if (transport_ == nullptr ||
        !frameSource.left.IsValid() ||
        (stereo && !frameSource.right.IsValid()))
    {
        return false;
    }
    if (shuttingDown_.load())
    {
        return false;
    }

    size_t slotIndex = 0;
    if (!AcquireSlot(slotIndex))
    {
        FrameMetrics metrics = {};
        metrics.frameNumber = frameNumberCounter_.fetch_add(1);
        metrics.timestampNs = timestampNs;
        metrics.frameDropped = true;
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    }

    BufferSlot& slot = slots_[slotIndex];
    CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)slot.pixelBuffer;
    // Compose in BGRA directly into the slot's composite texture — a view of
    // the pixel buffer's IOSurface — which then goes to VideoToolbox as-is.
    id<MTLTexture> dstTexture = slot.compositeTexture != nullptr
        ? CVMetalTextureGetTexture((CVMetalTextureRef)slot.compositeTexture) : nil;
    if (pixelBuffer == nullptr || dstTexture == nil)
    {
        ReleaseSlot(slotIndex);
        return false;
    }

    id<MTLTexture> leftTex = (__bridge id<MTLTexture>)frameSource.left.GetImage();
    id<MTLTexture> rightTex = stereo ? (__bridge id<MTLTexture>)frameSource.right.GetImage() : nil;
    // Assigned (consumed from forceKeyframe_) further below, after the cheap
    // early-out paths; declared here so dropAcquiredSlot can re-arm it.
    bool forceKeyframe = false;
    auto dropAcquiredSlot = [&](const char* reason) {
        if (reason != nullptr && !foveationValidationWarningLogged_.exchange(true))
        {
            spdlog::warn("VideoEncoder: dropping frame before encode: {}", reason);
        }
        droppedFrameCount_.fetch_add(1);
        if (forceKeyframe && !shuttingDown_.load())
        {
            // The frame never reached VT; put the swallowed keyframe request
            // back so a following frame honors it (the 500ms limiter in
            // ForceKeyframe still gates re-acceptance, so no IDR storm).
            forceKeyframe_.store(true);
        }
        ReleaseSlot(slotIndex);
        if (frameCallback)
        {
            FrameMetrics metrics = {};
            metrics.frameNumber = frameNumberCounter_.fetch_add(1);
            metrics.timestampNs = timestampNs;
            metrics.frameDropped = true;
            frameCallback(metrics);
        }
        return false;
    };
    if (leftTex == nil || (stereo && rightTex == nil))
    {
        return dropAcquiredSlot("missing source texture");
    }

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)videoToolbox_.commandQueue;
    if (queue == nil)
    {
        ReleaseSlot(slotIndex);
        return false;
    }
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    if (cmdBuf == nil)
    {
        ReleaseSlot(slotIndex);
        return false;
    }

    EncodeWaitForFrameImage(cmdBuf, frameSource.left);
    if (stereo)
    {
        EncodeWaitForFrameImage(cmdBuf, frameSource.right);
    }

    // Crop each eye out of its swapchain sub-rectangle (subImage.imageRect). UE packs both eyes
    // side-by-side in one swapchain, so without this both eyes would receive the full [L|R] frame.
    // The crop target is cached per-eye and only reallocated when size/format actually changes
    // (session start, or a resolution/foveation reconfigure) -- not on every single frame.
    {
        id<MTLDevice> cropDev = queue.device;
        auto cropEye = [&](id<MTLTexture> tex, const FrameImageSource& src, void** cachedTexture) -> id<MTLTexture> {
            if (tex == nil || !src.HasSourceRect()) return tex;
            if (src.sourceX == 0 && src.sourceY == 0 &&
                src.sourceWidth == (uint32_t)tex.width &&
                src.sourceHeight == (uint32_t)tex.height) return tex;

            id<MTLTexture> eye = (__bridge id<MTLTexture>)*cachedTexture;
            if (eye == nil || eye.pixelFormat != tex.pixelFormat ||
                eye.width != (NSUInteger)src.sourceWidth ||
                eye.height != (NSUInteger)src.sourceHeight)
            {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat
                                                                                              width:src.sourceWidth
                                                                                             height:src.sourceHeight
                                                                                          mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                d.storageMode = MTLStorageModePrivate;
                id<MTLTexture> newEye = [cropDev newTextureWithDescriptor:d];
                if (newEye == nil)
                {
                    return nil;
                }
                if (eye != nil)
                {
                    [eye release];
                }
                eye = newEye;
                *cachedTexture = (void*)eye;
            }

            id<MTLBlitCommandEncoder> cb = [cmdBuf blitCommandEncoder];
            if (cb == nil)
            {
                return nil;
            }
            [cb copyFromTexture:tex sourceSlice:0 sourceLevel:0
                   sourceOrigin:MTLOriginMake(src.sourceX, src.sourceY, 0)
                     sourceSize:MTLSizeMake(src.sourceWidth, src.sourceHeight, 1)
                      toTexture:eye destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
            [cb endEncoding];
            return eye;
        };
        leftTex = cropEye(leftTex, frameSource.left, &slot.leftCropTexture);
        if (leftTex == nil)
        {
            return dropAcquiredSlot("failed to crop left eye texture");
        }
        if (stereo)
        {
            rightTex = cropEye(rightTex, frameSource.right, &slot.rightCropTexture);
            if (rightTex == nil)
            {
                return dropAcquiredSlot("failed to crop right eye texture");
            }
        }
    }

    forceKeyframe = forceKeyframe_.exchange(false);
    const bool useFoveatedEncoding = stereo &&
        foveationSettings_.enabled &&
        videoToolbox_.foveationPipeline != nullptr &&
        videoToolbox_.foveationSampler != nullptr &&
        slot.foveatedScratchTexture != nullptr;
    bool needsDownscale = stereo
        ? (leftTex.width != (NSUInteger)eyeWidth_ || leftTex.height != (NSUInteger)height_ ||
           rightTex.width != (NSUInteger)eyeWidth_ || rightTex.height != (NSUInteger)height_)
        : (leftTex.width != (NSUInteger)width_ || leftTex.height != (NSUInteger)height_);

    if (frameCount_ == 0)
    {
        spdlog::info("VideoEncoder: submit {} frame srcL={}x{} srcR={}x{} dst={}x{} downscale={}",
                      stereo ? "stereo" : "mono",
                      (uint32_t)leftTex.width, (uint32_t)leftTex.height,
                      stereo ? (uint32_t)rightTex.width : 0,
                      stereo ? (uint32_t)rightTex.height : 0,
                      (uint32_t)dstTexture.width, (uint32_t)dstTexture.height,
                      useFoveatedEncoding ? true : needsDownscale);
        if (useFoveatedEncoding)
        {
            spdlog::info("VideoEncoder: foveated path targetEye={}x{} encoded={}x{} ratio={:.4f}x{:.4f} via compute scratch texture",
                          foveationSettings_.targetEyeWidth,
                          foveationSettings_.targetEyeHeight,
                          width_,
                          height_,
                          foveationSettings_.eyeWidthRatio,
                          foveationSettings_.eyeHeightRatio);
        }
    }

    if (useFoveatedEncoding)
    {
        id<MTLTexture> foveatedDstTexture = (id<MTLTexture>)slot.foveatedScratchTexture;
        if (foveatedDstTexture == nil ||
            foveatedDstTexture.width != (NSUInteger)width_ ||
            foveatedDstTexture.height != (NSUInteger)height_ ||
            rightTex.width != leftTex.width ||
            rightTex.height != leftTex.height ||
            !TextureAllowsUsage(leftTex, MTLTextureUsageShaderRead) ||
            !TextureAllowsUsage(rightTex, MTLTextureUsageShaderRead) ||
            !TextureAllowsUsage(foveatedDstTexture, MTLTextureUsageShaderWrite) ||
            (width_ % 2u) != 0 ||
            eyeWidth_ == 0 ||
            height_ == 0 ||
            !IsFoveationSettingsValid(foveationSettings_,
                                      (uint32_t)leftTex.width,
                                      (uint32_t)leftTex.height))
        {
            return dropAcquiredSlot("invalid foveated texture, dimensions, usage, or settings");
        }

        MetalFoveationUniforms uniforms = {};
        uniforms.centerSize = {foveationSettings_.centerSizeX, foveationSettings_.centerSizeY};
        uniforms.centerShift = {foveationSettings_.centerShiftX, foveationSettings_.centerShiftY};
        uniforms.edgeRatio = {foveationSettings_.edgeRatioX, foveationSettings_.edgeRatioY};
        uniforms.eyeSizeRatio = {foveationSettings_.eyeWidthRatio, foveationSettings_.eyeHeightRatio};

        id<MTLComputeCommandEncoder> computeEncoder = [cmdBuf computeCommandEncoder];
        if (computeEncoder == nil)
        {
            return dropAcquiredSlot("failed to create foveated compute encoder");
        }
        id<MTLComputePipelineState> pipeline =
            (id<MTLComputePipelineState>)videoToolbox_.foveationPipeline;
        [computeEncoder setComputePipelineState:pipeline];
        [computeEncoder setTexture:leftTex atIndex:0];
        [computeEncoder setTexture:rightTex atIndex:1];
        [computeEncoder setTexture:foveatedDstTexture atIndex:2];
        [computeEncoder setSamplerState:(id<MTLSamplerState>)videoToolbox_.foveationSampler
                                atIndex:0];
        [computeEncoder setBytes:&uniforms length:sizeof(uniforms) atIndex:0];

        const NSUInteger threadsX = std::max<NSUInteger>(1, std::min<NSUInteger>(pipeline.threadExecutionWidth, 16));
        const NSUInteger threadsY = std::max<NSUInteger>(
            1,
            std::min<NSUInteger>(pipeline.maxTotalThreadsPerThreadgroup / threadsX, 16));
        const MTLSize threadsPerGroup = MTLSizeMake(threadsX, threadsY, 1);
        const MTLSize threadgroups = MTLSizeMake(
            ((NSUInteger)width_ + threadsX - 1) / threadsX,
            ((NSUInteger)height_ + threadsY - 1) / threadsY,
            1);
        [computeEncoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerGroup];
        [computeEncoder endEncoding];

        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
        if (blit == nil)
        {
            return dropAcquiredSlot("failed to create foveated blit encoder");
        }
        [blit copyFromTexture:foveatedDstTexture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width_, height_, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }
    else if (stereo && needsDownscale)
    {
        id<MTLTexture> tmpLeft = (id<MTLTexture>)slot.tmpLeftTexture;
        id<MTLTexture> tmpRight = (id<MTLTexture>)slot.tmpRightTexture;
        MPSImageBilinearScale* scaler = (MPSImageBilinearScale*)videoToolbox_.scaler;
        [scaler encodeToCommandBuffer:cmdBuf sourceTexture:leftTex destinationTexture:tmpLeft];
        [scaler encodeToCommandBuffer:cmdBuf sourceTexture:rightTex destinationTexture:tmpRight];

        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
        [blit copyFromTexture:tmpLeft
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(eyeWidth_, height_, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit copyFromTexture:tmpRight
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(eyeWidth_, height_, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(eyeWidth_, 0, 0)];
        [blit endEncoding];
    }
    else if (stereo)
    {
        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
        NSUInteger leftCopyW = MIN(leftTex.width, (NSUInteger)eyeWidth_);
        NSUInteger leftCopyH = MIN(leftTex.height, (NSUInteger)height_);
        [blit copyFromTexture:leftTex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(leftCopyW, leftCopyH, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];

        NSUInteger rightCopyW = MIN(rightTex.width, (NSUInteger)eyeWidth_);
        NSUInteger rightCopyH = MIN(rightTex.height, (NSUInteger)height_);
        [blit copyFromTexture:rightTex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(rightCopyW, rightCopyH, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(eyeWidth_, 0, 0)];
        [blit endEncoding];
    }
    else if (needsDownscale)
    {
        MPSImageBilinearScale* scaler = (MPSImageBilinearScale*)videoToolbox_.scaler;
        [scaler encodeToCommandBuffer:cmdBuf sourceTexture:leftTex destinationTexture:dstTexture];
    }
    else
    {
        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
        NSUInteger copyW = MIN(leftTex.width, dstTexture.width);
        NSUInteger copyH = MIN(leftTex.height, dstTexture.height);
        [blit copyFromTexture:leftTex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copyW, copyH, 1)
                    toTexture:dstTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }

    oxrsys::encoder::FrameCallbacks frameCallbacks;
    if (callback)
    {
        // Fan the frame-level result back out as the legacy per-NAL callback:
        // one invocation per Annex-B NAL unit, byte-identical to the old
        // direct emission (start code included in each descriptor).
        frameCallbacks.onEncodedFrame =
            [nalCallback = std::move(callback)](const oxrsys::encoder::EncodedFrameResult& result) {
                for (const oxrsys::encoder::NalUnitDescriptor& nal : result.nalUnits)
                {
                    nalCallback(result.data + nal.offset, nal.size, result.isIdr, result.timestampNs);
                }
            };
    }
    frameCallbacks.onFrameComplete = std::move(frameCallback);
    // Hold a strong reference to the encoder for as long as the frame is in
    // flight. The engine finalizes the frame (invoking this lambda last) from
    // either the Metal completed handler below or, on the success path, the
    // asynchronous VT output callback long after both owners have dropped
    // their shared_ptr. Capturing self keeps ReleaseSlot()'s `this` valid
    // through that whole window instead of dereferencing a freed encoder.
    // frameSource rides along so the source images/sync objects stay alive
    // until the frame context is destroyed.
    frameCallbacks.releaseResources =
        [self = shared_from_this(), slotIndex, frameSource = std::move(frameSource)]() {
            (void)frameSource;
            self->ReleaseSlot(slotIndex);
        };

    oxrsys::encoder::EncodedFrameMetrics seedMetrics;
    seedMetrics.frameNumber = frameNumberCounter_.fetch_add(1);
    seedMetrics.timestampNs = timestampNs;
    seedMetrics.keyframe = forceKeyframe;

    // The pending-frame token retains the compression session across the
    // async handler (blocks do not retain CF types), so a handler firing
    // during/after Shutdown() must go through the token, never a raw session.
    std::shared_ptr<oxrsys::encoder::IPendingEncodeFrame> pendingFrame =
        transport_->BeginFrame(std::move(frameCallbacks), seedMetrics);
    if (pendingFrame == nullptr)
    {
        // Session vanished under us (shutdown race). The callbacks were
        // consumed by BeginFrame, so finalize through the drop path without
        // them; the old code would have crashed retaining a null session here.
        return dropAcquiredSlot(nullptr);
    }

    CVPixelBufferRetain(pixelBuffer);
    // Capture self so the handler's direct member accesses (shuttingDown_,
    // forceKeyframe_) stay valid even if this fires after both owners have
    // dropped their shared_ptr and the 200ms Shutdown() drain gave up.
    auto self = shared_from_this();
    [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer)
    {
        if (commandBuffer.status != MTLCommandBufferStatusCompleted || self->shuttingDown_.load())
        {
            if (forceKeyframe && !self->shuttingDown_.load())
            {
                // Frame never reached VT; re-arm the swallowed keyframe request.
                self->forceKeyframe_.store(true);
            }
            pendingFrame->Cancel();
            CVPixelBufferRelease(pixelBuffer);
            return;
        }

        // Submit() performs ALL frame-context writes before the VT encode
        // call (the low-latency output callback can finalize the frame before
        // Submit even returns) and finalizes the frame as dropped itself when
        // the submission fails.
        if (!pendingFrame->Submit(pixelBuffer, timestampNs, forceKeyframe))
        {
            if (forceKeyframe && !self->shuttingDown_.load())
            {
                // Frame never reached VT; re-arm the swallowed keyframe request.
                self->forceKeyframe_.store(true);
            }
        }
        CVPixelBufferRelease(pixelBuffer);
    }];

    [cmdBuf commit];

    frameCount_++;
    return true;
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    std::lock_guard<std::mutex> lock(slotMutex_);
    for (size_t i = 0; i < SlotCount; i++)
    {
        if (!slots_[i].inUse)
        {
            slots_[i].inUse = true;
            inFlightFrameCount_.fetch_add(1);
            outSlotIndex = i;
            return true;
        }
    }

    droppedFrameCount_.fetch_add(1);
    return false;
}

void VideoEncoder::ReleaseSlot(size_t slotIndex)
{
    std::lock_guard<std::mutex> lock(slotMutex_);
    if (slotIndex >= SlotCount || !slots_[slotIndex].inUse)
    {
        return;
    }

    slots_[slotIndex].inUse = false;
    uint32_t current = inFlightFrameCount_.load();
    if (current > 0)
    {
        inFlightFrameCount_.fetch_sub(1);
    }
}

void VideoEncoder::DestroySlots()
{
    std::lock_guard<std::mutex> lock(slotMutex_);
    for (BufferSlot& slot : slots_)
    {
        slot.inUse = false;

        if (slot.tmpLeftTexture != nullptr)
        {
            [(id<MTLTexture>)slot.tmpLeftTexture release];
            slot.tmpLeftTexture = nullptr;
        }
        if (slot.tmpRightTexture != nullptr)
        {
            [(id<MTLTexture>)slot.tmpRightTexture release];
            slot.tmpRightTexture = nullptr;
        }
        if (slot.foveatedScratchTexture != nullptr)
        {
            [(id<MTLTexture>)slot.foveatedScratchTexture release];
            slot.foveatedScratchTexture = nullptr;
        }
        if (slot.leftCropTexture != nullptr)
        {
            [(id<MTLTexture>)slot.leftCropTexture release];
            slot.leftCropTexture = nullptr;
        }
        if (slot.rightCropTexture != nullptr)
        {
            [(id<MTLTexture>)slot.rightCropTexture release];
            slot.rightCropTexture = nullptr;
        }
        if (slot.compositeTexture != nullptr)
        {
            CFRelease(slot.compositeTexture);
            slot.compositeTexture = nullptr;
        }
        if (slot.pixelBuffer != nullptr)
        {
            CVPixelBufferRelease((CVPixelBufferRef)slot.pixelBuffer);
            slot.pixelBuffer = nullptr;
        }
    }

    inFlightFrameCount_.store(0);
}

void VideoEncoder::ForceKeyframe()
{
    // Unconditional: rate-limiting client keyframe requests is the caller's
    // job (KeyframeRequestLimiter at the request-ingress points); internal
    // forces (connect warmup, GOP cadence, reconfigure) are deliberate.
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    if (transport_ == nullptr || bitrateMbps == bitrateMbps_)
    {
        return;
    }

    // The engine updates the AverageBitRate/DataRateLimits pair (no CBR path;
    // see the rate-control comment in VideoToolboxEncodeEngine::CreateSession)
    // and owns the success/failure logging.
    if (transport_->SetBitrate(bitrateMbps))
    {
        bitrateMbps_ = bitrateMbps;
    }
}
