// SPDX-License-Identifier: MPL-2.0

#import "VideoEncoder.h"
#import "Config.h"
#import "CodecSelect.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <VideoToolbox/VideoToolbox.h>
#import <simd/simd.h>

#import <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>
#include <utility>

namespace
{

// Set once at session init; read by the output callback for parameter-set extraction.
// H.264 when the runtime runs under Rosetta (HEVC HW encode unavailable there).
bool g_useH264 = false;

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

struct EncodeFrameContext
{
    VideoEncoder::OnNalUnitCallback nalCallback;
    VideoEncoder::OnFrameEncodedCallback frameCallback;
    std::function<void(size_t)> releaseSlot;
    FrameSource frameSource;
    VideoEncoder::FrameMetrics metrics;
    size_t slotIndex = 0;
    Clock::time_point encodeStart;
    Clock::time_point encodeSubmitFinished;
};

struct MetalFoveationUniforms
{
    vector_float2 centerSize;
    vector_float2 centerShift;
    vector_float2 edgeRatio;
    vector_float2 eyeSizeRatio;
};

// Encoder compute library: axis-aligned foveated encoding shader logic adapted
// from ALVR's AADT compression shader (MIT licensed), plus the BGRA->NV12
// conversion kernel that feeds VideoToolbox pre-converted YCbCr planes.
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

constant float3 kBt709Luma = float3(0.2126, 0.7152, 0.0722);

// BT.709 video-range BGRA -> NV12. One thread per CHROMA texel: writes the
// 2x2 luma quad and one CbCr sample averaged over the 2x2 RGB block.
kernel void rgb_to_nv12(texture2d<float, access::read> rgbTexture [[texture(0)]],
                        texture2d<float, access::write> yTexture [[texture(1)]],
                        texture2d<float, access::write> cbcrTexture [[texture(2)]],
                        uint2 gid [[thread_position_in_grid]])
{
    uint chromaWidth = cbcrTexture.get_width();
    uint chromaHeight = cbcrTexture.get_height();
    if (gid.x >= chromaWidth || gid.y >= chromaHeight)
    {
        return;
    }

    uint2 lumaBase = gid * 2;
    float3 rgbSum = float3(0.0);
    for (uint dy = 0; dy < 2; dy++)
    {
        for (uint dx = 0; dx < 2; dx++)
        {
            uint2 coord = lumaBase + uint2(dx, dy);
            float3 rgb = rgbTexture.read(coord).rgb;
            float luma = dot(rgb, kBt709Luma);
            yTexture.write(float4((16.0 + 219.0 * luma) / 255.0), coord);
            rgbSum += rgb;
        }
    }

    float3 avgRgb = rgbSum * 0.25;
    float avgLuma = dot(avgRgb, kBt709Luma);
    float cb = (128.0 + 224.0 * (avgRgb.b - avgLuma) / 1.8556) / 255.0;
    float cr = (128.0 + 224.0 * (avgRgb.r - avgLuma) / 1.5748) / 255.0;
    cbcrTexture.write(float4(cb, cr, 0.0, 0.0), gid);
}
)METAL";

void FinalizeEncodeFrame(EncodeFrameContext* context, bool frameDropped)
{
    if (context == nullptr)
    {
        return;
    }

    auto now = Clock::now();
    context->metrics.frameDropped = frameDropped;
    context->metrics.callbackLatencyMs = ToMilliseconds(now - context->encodeSubmitFinished);
    context->metrics.totalLatencyMs = ToMilliseconds(now - context->encodeStart);

    if (context->frameCallback)
    {
        try
        {
            context->frameCallback(context->metrics);
        }
        catch (const std::exception& error)
        {
            spdlog::warn("VideoEncoder: frame callback threw: {}", error.what());
        }
        catch (...)
        {
            spdlog::warn("VideoEncoder: frame callback threw an unknown exception");
        }
    }

    if (context->releaseSlot)
    {
        context->releaseSlot(context->slotIndex);
    }

    delete context;
}

bool IsKeyframeSample(CMSampleBufferRef sampleBuffer)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments == nullptr || CFArrayGetCount(attachments) == 0)
    {
        return true;
    }

    CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef notSync = nullptr;
    if (!CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, (const void**)&notSync))
    {
        return true;
    }

    return !CFBooleanGetValue(notSync);
}

void EmitSampleNalUnits(CMSampleBufferRef sampleBuffer, bool isKeyframe,
                        const VideoEncoder::OnNalUnitCallback& callback)
{
    if (!callback)
    {
        return;
    }

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t timestampNs = (int64_t)(CMTimeGetSeconds(pts) * 1e9);

    if (isKeyframe)
    {
        CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (formatDesc != nullptr)
        {
            size_t paramSetCount = 0;
            if (g_useH264)
            {
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDesc, 0, nullptr, nullptr, &paramSetCount, nullptr);
            }
            else
            {
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    formatDesc, 0, nullptr, nullptr, &paramSetCount, nullptr);
            }

            for (size_t i = 0; i < paramSetCount; i++)
            {
                const uint8_t* paramSet = nullptr;
                size_t paramSetSize = 0;
                OSStatus status = g_useH264
                    ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                          formatDesc, i, &paramSet, &paramSetSize, nullptr, nullptr)
                    : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                          formatDesc, i, &paramSet, &paramSetSize, nullptr, nullptr);
                if (status != noErr || paramSet == nullptr || paramSetSize == 0)
                {
                    continue;
                }

                std::vector<uint8_t> nalUnit(4 + paramSetSize);
                nalUnit[0] = 0x00;
                nalUnit[1] = 0x00;
                nalUnit[2] = 0x00;
                nalUnit[3] = 0x01;
                memcpy(nalUnit.data() + 4, paramSet, paramSetSize);
                callback(nalUnit.data(), nalUnit.size(), true, timestampNs);
            }
        }
    }

    CMBlockBufferRef dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (dataBuffer == nullptr)
    {
        return;
    }

    size_t totalLength = 0;
    char* dataPointer = nullptr;
    if (CMBlockBufferGetDataPointer(dataBuffer, 0, nullptr, &totalLength, &dataPointer) != noErr ||
        dataPointer == nullptr || totalLength == 0)
    {
        return;
    }

    size_t offset = 0;
    while (offset + 4 <= totalLength)
    {
        uint32_t naluLength = 0;
        memcpy(&naluLength, dataPointer + offset, 4);
        naluLength = CFSwapInt32BigToHost(naluLength);
        offset += 4;

        if (naluLength == 0 || offset + naluLength > totalLength)
        {
            break;
        }

        std::vector<uint8_t> nalUnit(4 + naluLength);
        nalUnit[0] = 0x00;
        nalUnit[1] = 0x00;
        nalUnit[2] = 0x00;
        nalUnit[3] = 0x01;
        memcpy(nalUnit.data() + 4, dataPointer + offset, naluLength);

        callback(nalUnit.data(), nalUnit.size(), isKeyframe, timestampNs);
        offset += naluLength;
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

// VTSessionSetProperty failures are silent otherwise; a rejected property means
// the encoder is running with defaults (e.g. no rate cap), so always log them.
OSStatus SetSessionProperty(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value)
{
    OSStatus status = VTSessionSetProperty(session, key, value);
    if (status != noErr)
    {
        const char* keyName = [(__bridge NSString*)key UTF8String];
        spdlog::warn("VideoEncoder: VTSessionSetProperty({}) failed: {}",
                     keyName != nullptr ? keyName : "?", (int)status);
    }
    return status;
}

} // namespace

static void CompressionOutputCallback(void* /*outputCallbackRefCon*/,
                                       void* sourceFrameRefCon,
                                       OSStatus status,
                                       VTEncodeInfoFlags infoFlags,
                                       CMSampleBufferRef sampleBuffer)
{
    auto* context = static_cast<EncodeFrameContext*>(sourceFrameRefCon);
    if (context == nullptr)
    {
        return;
    }

    if (status != noErr || sampleBuffer == nullptr || (infoFlags & kVTEncodeInfo_FrameDropped))
    {
        FinalizeEncodeFrame(context, true);
        return;
    }

    bool isKeyframe = IsKeyframeSample(sampleBuffer);
    context->metrics.keyframe = isKeyframe;
    try
    {
        EmitSampleNalUnits(sampleBuffer, isKeyframe, context->nalCallback);
    }
    catch (const std::exception& error)
    {
        spdlog::warn("VideoEncoder: NAL callback threw: {}", error.what());
        FinalizeEncodeFrame(context, true);
        return;
    }
    catch (...)
    {
        spdlog::warn("VideoEncoder: NAL callback threw an unknown exception");
        FinalizeEncodeFrame(context, true);
        return;
    }
    FinalizeEncodeFrame(context, false);
}

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
                               uint32_t bitrateMbps, const GraphicsContext& graphicsContext)
{
    Shutdown();

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    // NV12 output: the 4:2:0 chroma plane and the 2x2 conversion kernel both
    // require even dimensions (2496x1312 in practice).
    if (width_ == 0 || height_ == 0 || (width_ % 2u) != 0 || (height_ % 2u) != 0)
    {
        spdlog::error("VideoEncoder: NV12 encoding requires even dimensions, got {}x{}",
                      width_, height_);
        return false;
    }
    fps_ = fps;
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    videoToolbox_.metalDevice = graphicsContext.metalDevice;
    shuttingDown_.store(false);
    foveationValidationWarningLogged_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);
    frameCount_ = 0;
    acceptedForcedKeyframes_.store(0, std::memory_order_relaxed);
    lastForcedKeyframeNs_.store(0, std::memory_order_relaxed);

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
    videoToolbox_.nv12ConvertPipeline = (void*)CreateComputePipeline(device, @"rgb_to_nv12");
    if (videoToolbox_.nv12ConvertPipeline == nullptr)
    {
        spdlog::error("VideoEncoder: NV12 conversion pipeline unavailable");
        Shutdown();
        return false;
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
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
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

    // Compose target: all paths write BGRA here, then rgb_to_nv12 reads it
    // (shaderRead) and the MPS mono-downscale path writes it (shaderWrite).
    MTLTextureDescriptor* compositeDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width_
                                    height:height_
                                 mipmapped:NO];
    compositeDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    compositeDesc.storageMode = MTLStorageModePrivate;

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

        // The buffer already holds BT.709 video-range YCbCr (rgb_to_nv12 does the
        // conversion in Metal); with NV12 input VT converts nothing, these tags
        // just describe the content for the bitstream/decoder side.
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
            kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
            kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
            kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);

        CVMetalTextureRef yTexture = nullptr;
        cvResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            (CVMetalTextureCacheRef)videoToolbox_.textureCache,
            pixelBuffer,
            nullptr,
            MTLPixelFormatR8Unorm,
            width_,
            height_,
            0,
            &yTexture);
        if (cvResult != kCVReturnSuccess || yTexture == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to create luma plane texture for slot {}", i);
            CVPixelBufferRelease(pixelBuffer);
            Shutdown();
            return false;
        }

        CVMetalTextureRef cbcrTexture = nullptr;
        cvResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            (CVMetalTextureCacheRef)videoToolbox_.textureCache,
            pixelBuffer,
            nullptr,
            MTLPixelFormatRG8Unorm,
            width_ / 2,
            height_ / 2,
            1,
            &cbcrTexture);
        if (cvResult != kCVReturnSuccess || cbcrTexture == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to create chroma plane texture for slot {}", i);
            CFRelease(yTexture);
            CVPixelBufferRelease(pixelBuffer);
            Shutdown();
            return false;
        }

        slots_[i].pixelBuffer = pixelBuffer;
        slots_[i].yTexture = yTexture;
        slots_[i].cbcrTexture = cbcrTexture;
        slots_[i].compositeTexture = (void*)[device newTextureWithDescriptor:compositeDesc];
        if (slots_[i].compositeTexture == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to create composite texture for slot {}", i);
            Shutdown();
            return false;
        }
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

    g_useH264 = (oxrsys::PreferredVideoCodec() == oxr::protocol::VideoCodec::H264);
    const CMVideoCodecType codecType = g_useH264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;

    // Low-latency rate control halves encode latency (33 -> 10.6ms measured)
    // and fixes the ~30% bitrate overshoot of the default RC. Its Rosetta
    // all-zero-chroma bug (green image) lives in VT's internal RGB->YCbCr
    // conversion of BGRA input, which the NV12 input path above bypasses
    // entirely — proven offline in tools/vt-llrc-probe (LL-RC+BGRA = zero
    // chroma, LL-RC+NV12 = healthy chroma).
    NSDictionary* encoderSpec = @{
        (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @NO,
        (NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl: @YES,
    };

    VTCompressionSessionRef compressionSession = nullptr;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        width,
        height,
        codecType,
        (__bridge CFDictionaryRef)encoderSpec,
        nullptr,
        kCFAllocatorDefault,
        CompressionOutputCallback,
        nullptr,
        &compressionSession);
    const bool lowLatencyRateControl = (status == noErr);
    if (status != noErr)
    {
        spdlog::warn("VideoEncoder: Session create with low-latency RC failed ({}); retrying without it",
                     (int)status);
        NSDictionary* fallbackSpec = @{
            (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
            (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @NO,
        };
        status = VTCompressionSessionCreate(
            kCFAllocatorDefault,
            width,
            height,
            codecType,
            (__bridge CFDictionaryRef)fallbackSpec,
            nullptr,
            kCFAllocatorDefault,
            CompressionOutputCallback,
            nullptr,
            &compressionSession);
    }
    if (status != noErr)
    {
        spdlog::error("VideoEncoder: Failed to create compression session: {}", status);
        Shutdown();
        return false;
    }

    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    const ConfigValues config = Config::Get().GetValues();
    const std::string& preset = config.encoderPreset;
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_ProfileLevel,
        g_useH264 ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel);
    if (g_useH264)
    {
        // CABAC buys ~10% quality over the CAVLC default at the same bitrate;
        // High profile already implies the decoder supports it.
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC);
    }
    if (preset == "speed")
    {
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
        spdlog::info("VideoEncoder: Using 'speed' preset (prioritize speed)");
    }
    else if (preset == "quality")
    {
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanFalse);
        spdlog::info("VideoEncoder: Using 'quality' preset");
    }
    else
    {
        spdlog::info("VideoEncoder: Using 'balanced' preset");
    }

    // Rate control: AverageBitRate + an EXACT per-second DataRateLimits budget.
    // Do NOT use kVTCompressionPropertyKey_ConstantBitRate here: the header
    // documents it as incompatible with AverageBitRate/DataRateLimits, the
    // LL-RC encoder rejects it (-12900), and classic RC silently ignores it
    // (vt-llrc-probe --cbr, 2026-07-04; the "accepted then stalls" observation
    // of 2026-07-03 traced to the frame-context use-after-free fixed
    // alongside the NV12 encoder-input work, not CBR). AverageBitRate alone (with the old 1.5x limits
    // headroom) overshot ~2x; the exact 1.0x budget below holds the measured
    // output at/under target.
    int targetBitrate = bitrateMbps * 1000000;
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &targetBitrate);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    // Exact per-second byte budget; headroom above target lets VT overshoot.
    double peakBytesPerSecond = (double)targetBitrate / 8.0;
    NSArray* dataRateLimits = @[@(peakBytesPerSecond), @(1.0)];
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)dataRateLimits);

    uint32_t keyframeIntervalSec = config.keyframeIntervalSec;
    int keyframeInterval = keyframeIntervalSec * std::max(fps, 1u);
    CFNumberRef intervalRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyframeInterval);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, intervalRef);
    CFRelease(intervalRef);

    double keyframeDuration = (double)keyframeIntervalSec;
    CFNumberRef durationRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &keyframeDuration);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, durationRef);
    CFRelease(durationRef);

    int expectedFps = std::max(fps, 1u);
    CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &expectedFps);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);

    // Sessions created with low-latency RC always reject MaxFrameDelayCount
    // (-12900, and LL already implies no frame delay); only set it on the
    // fallback (non-LL) session.
    if (!lowLatencyRateControl)
    {
        int maxFrameDelay = 0;
        CFNumberRef delayRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxFrameDelay);
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_MaxFrameDelayCount, delayRef);
        CFRelease(delayRef);
    }

    // Explicit output color space (matches the input attachments + client BT.709 shader).
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);

    VTCompressionSessionPrepareToEncodeFrames(compressionSession);
    videoToolbox_.session = compressionSession;

    {
        CFBooleanRef usingHw = nullptr;
        OSStatus hwStatus = VTSessionCopyProperty(compressionSession,
            kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
            kCFAllocatorDefault, &usingHw);
        if (hwStatus == noErr)
        {
            const bool hw = (usingHw != nullptr && CFBooleanGetValue(usingHw));
            spdlog::info("VideoEncoder: hardware-accelerated encoder = {}", hw);
        }
        else
        {
            spdlog::info("VideoEncoder: hardware-acceleration query unsupported by this encoder (status={})",
                         (int)hwStatus);
        }
        if (usingHw) CFRelease(usingHw);
    }

    spdlog::info("VideoEncoder: Initialized {} encoder {}x{} @ {}fps, {}Mbps (slots={}, keyframe={}s, preset={})",
                  g_useH264 ? "H.264" : "H.265",
                  width, height, fps, bitrateMbps, SlotCount, keyframeIntervalSec, preset);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)videoToolbox_.session;
    if (compressionSession != nullptr)
    {
        VTCompressionSessionCompleteFrames(compressionSession, kCMTimeInvalid);
    }

    // Drain in-flight frames BEFORE invalidating/releasing the session: late
    // Metal completed handlers hold their own retains, but invalidating here
    // would yank the session out from under any encode still in flight.
    for (int i = 0; i < 200 && inFlightFrameCount_.load() > 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (compressionSession != nullptr)
    {
        VTCompressionSessionInvalidate(compressionSession);
        CFRelease(compressionSession);
        videoToolbox_.session = nullptr;
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
    if (videoToolbox_.nv12ConvertPipeline != nullptr)
    {
        [(id<MTLComputePipelineState>)videoToolbox_.nv12ConvertPipeline release];
        videoToolbox_.nv12ConvertPipeline = nullptr;
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
    if (videoToolbox_.session == nullptr ||
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
    // Compose in BGRA into the slot's composite texture; a compute pass then
    // converts it into the pixel buffer's NV12 planes for VideoToolbox.
    id<MTLTexture> dstTexture = (id<MTLTexture>)slot.compositeTexture;
    id<MTLTexture> lumaTexture = slot.yTexture != nullptr
        ? CVMetalTextureGetTexture((CVMetalTextureRef)slot.yTexture) : nil;
    id<MTLTexture> chromaTexture = slot.cbcrTexture != nullptr
        ? CVMetalTextureGetTexture((CVMetalTextureRef)slot.cbcrTexture) : nil;
    id<MTLComputePipelineState> nv12Pipeline =
        (id<MTLComputePipelineState>)videoToolbox_.nv12ConvertPipeline;
    if (pixelBuffer == nullptr || dstTexture == nil ||
        lumaTexture == nil || chromaTexture == nil || nv12Pipeline == nil)
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

    // Convert the composed BGRA frame into the pixel buffer's NV12 planes.
    // Doing this conversion ourselves (instead of handing VT BGRA) is what
    // makes low-latency RC safe under Rosetta; see the encoder-spec comment.
    {
        id<MTLComputeCommandEncoder> convertEncoder = [cmdBuf computeCommandEncoder];
        if (convertEncoder == nil)
        {
            return dropAcquiredSlot("failed to create NV12 convert encoder");
        }
        [convertEncoder setComputePipelineState:nv12Pipeline];
        [convertEncoder setTexture:dstTexture atIndex:0];
        [convertEncoder setTexture:lumaTexture atIndex:1];
        [convertEncoder setTexture:chromaTexture atIndex:2];

        // One thread per chroma texel; each writes a 2x2 luma quad.
        const NSUInteger chromaWidth = (NSUInteger)(width_ / 2);
        const NSUInteger chromaHeight = (NSUInteger)(height_ / 2);
        const NSUInteger threadsX = std::max<NSUInteger>(
            1, std::min<NSUInteger>(nv12Pipeline.threadExecutionWidth, 16));
        const NSUInteger threadsY = std::max<NSUInteger>(
            1,
            std::min<NSUInteger>(nv12Pipeline.maxTotalThreadsPerThreadgroup / threadsX, 16));
        const MTLSize threadsPerGroup = MTLSizeMake(threadsX, threadsY, 1);
        const MTLSize threadgroups = MTLSizeMake(
            (chromaWidth + threadsX - 1) / threadsX,
            (chromaHeight + threadsY - 1) / threadsY,
            1);
        [convertEncoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerGroup];
        [convertEncoder endEncoding];
    }

    auto* context = new EncodeFrameContext();
    context->nalCallback = std::move(callback);
    context->frameCallback = std::move(frameCallback);
    context->releaseSlot = [this](size_t releasedSlotIndex) {
        ReleaseSlot(releasedSlotIndex);
    };
    context->frameSource = std::move(frameSource);
    context->slotIndex = slotIndex;
    context->metrics.frameNumber = frameNumberCounter_.fetch_add(1);
    context->metrics.timestampNs = timestampNs;
    context->metrics.keyframe = forceKeyframe;
    context->encodeStart = Clock::now();
    context->encodeSubmitFinished = context->encodeStart;

    // Retain the CF objects across the async handler: blocks do not retain CF
    // types, and a handler firing during/after Shutdown() must not touch a
    // freed session or pixel buffer.
    VTCompressionSessionRef compressionSession =
        (VTCompressionSessionRef)CFRetain(videoToolbox_.session);
    CVPixelBufferRetain(pixelBuffer);
    [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer)
    {
        if (commandBuffer.status != MTLCommandBufferStatusCompleted || this->shuttingDown_.load())
        {
            if (forceKeyframe && !this->shuttingDown_.load())
            {
                // Frame never reached VT; re-arm the swallowed keyframe request.
                this->forceKeyframe_.store(true);
            }
            FinalizeEncodeFrame(context, true);
            CVPixelBufferRelease(pixelBuffer);
            CFRelease(compressionSession);
            return;
        }

        context->metrics.gpuCopyMs = ToMilliseconds(Clock::now() - context->encodeStart);

        CFMutableDictionaryRef frameProps = nullptr;
        if (forceKeyframe)
        {
            frameProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(frameProps,
                kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        }

        CMTime presentationTime = CMTimeMake(timestampNs, 1000000000);
        // ALL context writes must happen BEFORE EncodeFrame: VT owns the
        // refcon from that call on, and the low-latency encoder can run the
        // output callback (which deletes the context) before EncodeFrame even
        // returns. Writing afterwards is a use-after-free that corrupts the
        // heap. encodeSubmitMs is therefore no longer measured (~0.05ms).
        context->metrics.encodeSubmitMs = 0.0;
        context->encodeSubmitFinished = Clock::now();
        OSStatus status = VTCompressionSessionEncodeFrame(
            compressionSession,
            pixelBuffer,
            presentationTime,
            kCMTimeInvalid,
            frameProps,
            context,
            nullptr);

        if (frameProps != nullptr)
        {
            CFRelease(frameProps);
        }

        if (status != noErr)
        {
            // VT does not invoke the output callback when EncodeFrame itself
            // fails, so the context is still ours to reclaim here.
            spdlog::warn("VideoEncoder: VTCompressionSessionEncodeFrame failed: {}", status);
            if (forceKeyframe && !this->shuttingDown_.load())
            {
                // Frame never reached VT; re-arm the swallowed keyframe request.
                this->forceKeyframe_.store(true);
            }
            FinalizeEncodeFrame(context, true);
        }
        CVPixelBufferRelease(pixelBuffer);
        CFRelease(compressionSession);
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
        if (slot.compositeTexture != nullptr)
        {
            [(id<MTLTexture>)slot.compositeTexture release];
            slot.compositeTexture = nullptr;
        }
        if (slot.yTexture != nullptr)
        {
            CFRelease(slot.yTexture);
            slot.yTexture = nullptr;
        }
        if (slot.cbcrTexture != nullptr)
        {
            CFRelease(slot.cbcrTexture);
            slot.cbcrTexture = nullptr;
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
    // Clients spam IDR requests for several seconds on connect; cap explicit
    // forces at 2/sec. Natural GOP keyframes (MaxKeyFrameInterval) are unaffected.
    // The first ForcedKeyframeWarmupCount forces after Initialize() bypass the
    // cap: StreamingServer intentionally sends a redundant IDR burst at connect
    // (post-Initialize force + frames 0-4) so a single lost packet does not
    // leave the client black until a keyframe request round-trips.
    // Called from multiple threads: CAS keeps the warmup counter from
    // double-counting past the cap; the 500ms-window check is a benign race
    // (worst case one extra accepted keyframe).
    bool warmup = false;
    uint32_t accepted = acceptedForcedKeyframes_.load(std::memory_order_relaxed);
    while (accepted < ForcedKeyframeWarmupCount)
    {
        if (acceptedForcedKeyframes_.compare_exchange_weak(
                accepted, accepted + 1, std::memory_order_relaxed))
        {
            warmup = true;
            break;
        }
    }

    const int64_t nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    if (!warmup)
    {
        constexpr int64_t minIntervalTicks =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(500)).count();
        if (nowNs - lastForcedKeyframeNs_.load(std::memory_order_relaxed) < minIntervalTicks)
        {
            spdlog::debug("VideoEncoder: Suppressing forced keyframe (<500ms since last)");
            return;
        }
    }
    lastForcedKeyframeNs_.store(nowNs, std::memory_order_relaxed);
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    if (videoToolbox_.session == nullptr || bitrateMbps == bitrateMbps_)
    {
        return;
    }

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)videoToolbox_.session;

    // No CBR path here: kVTCompressionPropertyKey_ConstantBitRate is banned —
    // documented incompatible with the AverageBitRate/DataRateLimits pair we
    // rely on (see the rate-control comment in Initialize()), so only that
    // pair is ever updated.
    int targetBitrate = bitrateMbps * 1000000;
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &targetBitrate);
    OSStatus status = SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    if (status == noErr)
    {
        // Keep the byte budget in lockstep with the average target.
        double peakBytesPerSecond = (double)targetBitrate / 8.0;
        NSArray* dataRateLimits = @[@(peakBytesPerSecond), @(1.0)];
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)dataRateLimits);
    }
    CFRelease(bitrateRef);

    if (status == noErr)
    {
        spdlog::info("VideoEncoder: Bitrate changed {} -> {} Mbps", bitrateMbps_, bitrateMbps);
        bitrateMbps_ = bitrateMbps;
    }
    else
    {
        spdlog::warn("VideoEncoder: Failed to set bitrate to {} Mbps: {}", bitrateMbps, status);
    }
}
