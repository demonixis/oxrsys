// SPDX-License-Identifier: MPL-2.0

#import "VideoEncoder.h"
#import "Config.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <VideoToolbox/VideoToolbox.h>
#import <simd/simd.h>

#import <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace
{

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
    uint32_t eyeIndex;
    uint32_t _padding[3];
};

// Axis-aligned foveated encoding shader logic adapted from ALVR's AADT
// compression shader (MIT licensed).
constexpr const char* kFoveationMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

struct FoveationUniforms
{
    float2 centerSize;
    float2 centerShift;
    float2 edgeRatio;
    float2 eyeSizeRatio;
    uint eyeIndex;
    uint3 _padding;
};

vertex VertexOut foveation_vertex(uint vertexId [[vertex_id]])
{
    constexpr float2 positions[6] = {
        {-1.0, -1.0}, { 1.0, -1.0}, { 1.0,  1.0},
        {-1.0, -1.0}, { 1.0,  1.0}, {-1.0,  1.0},
    };
    constexpr float2 uvs[6] = {
        {0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0},
        {0.0, 1.0}, {1.0, 0.0}, {0.0, 0.0},
    };

    VertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = uvs[vertexId];
    return out;
}

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

fragment float4 foveation_fragment(VertexOut in [[stage_in]],
                                   texture2d<float, access::sample> leftTexture [[texture(0)]],
                                   texture2d<float, access::sample> rightTexture [[texture(1)]],
                                   sampler linearSampler [[sampler(0)]],
                                   constant FoveationUniforms& params [[buffer(0)]])
{
    float2 eyeUv = in.uv / max(params.eyeSizeRatio, float2(0.0001));
    float2 compressedUv;
    compressedUv.x = compress_axis(eyeUv.x, params.centerSize.x, params.centerShift.x, params.edgeRatio.x);
    compressedUv.y = compress_axis(eyeUv.y, params.centerSize.y, params.centerShift.y, params.edgeRatio.y);
    compressedUv = clamp(compressedUv, float2(0.0), float2(1.0));

    return params.eyeIndex == 0
        ? leftTexture.sample(linearSampler, compressedUv)
        : rightTexture.sample(linearSampler, compressedUv);
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
        context->frameCallback(context->metrics);
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
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                formatDesc, 0, nullptr, nullptr, &paramSetCount, nullptr);

            for (size_t i = 0; i < paramSetCount; i++)
            {
                const uint8_t* paramSet = nullptr;
                size_t paramSetSize = 0;
                OSStatus status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
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

id<MTLRenderPipelineState> CreateFoveationPipeline(id<MTLDevice> device)
{
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kFoveationMetalSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil)
    {
        spdlog::error("VideoEncoder: Failed to compile foveation shader: {}",
                      error != nil ? error.localizedDescription.UTF8String : "unknown error");
        return nil;
    }

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"foveation_vertex"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"foveation_fragment"];
    if (vertexFunction == nil || fragmentFunction == nil)
    {
        spdlog::error("VideoEncoder: Failed to load foveation shader entry points");
        [vertexFunction release];
        [fragmentFunction release];
        [library release];
        return nil;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertexFunction;
    descriptor.fragmentFunction = fragmentFunction;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil)
    {
        spdlog::error("VideoEncoder: Failed to create foveation pipeline: {}",
                      error != nil ? error.localizedDescription.UTF8String : "unknown error");
    }

    [descriptor release];
    [vertexFunction release];
    [fragmentFunction release];
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
    EmitSampleNalUnits(sampleBuffer, isKeyframe, context->nalCallback);
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

    id<MTLRenderPipelineState> pipeline = CreateFoveationPipeline(device);
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
    fps_ = fps;
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    videoToolbox_.metalDevice = graphicsContext.metalDevice;
    shuttingDown_.store(false);
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
        videoToolbox_.foveationPipeline = (void*)CreateFoveationPipeline(device);
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

        CVMetalTextureRef cvMetalTexture = nullptr;
        cvResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            (CVMetalTextureCacheRef)videoToolbox_.textureCache,
            pixelBuffer,
            nullptr,
            MTLPixelFormatBGRA8Unorm,
            width_,
            height_,
            0,
            &cvMetalTexture);
        if (cvResult != kCVReturnSuccess || cvMetalTexture == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to create Metal texture for slot {}", i);
            CVPixelBufferRelease(pixelBuffer);
            Shutdown();
            return false;
        }

        slots_[i].pixelBuffer = pixelBuffer;
        slots_[i].metalTexture = cvMetalTexture;
        slots_[i].tmpLeftTexture = (void*)[device newTextureWithDescriptor:tmpDesc];
        slots_[i].tmpRightTexture = (void*)[device newTextureWithDescriptor:tmpDesc];
        slots_[i].inUse = false;
    }

    NSDictionary* encoderSpec = @{
        (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @NO,
    };

    VTCompressionSessionRef compressionSession = nullptr;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCMVideoCodecType_HEVC,
        (__bridge CFDictionaryRef)encoderSpec,
        nullptr,
        kCFAllocatorDefault,
        CompressionOutputCallback,
        nullptr,
        &compressionSession);
    if (status != noErr)
    {
        spdlog::error("VideoEncoder: Failed to create compression session: {}", status);
        Shutdown();
        return false;
    }

    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    const ConfigValues config = Config::Get().GetValues();
    const std::string& preset = config.encoderPreset;
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_HEVC_Main_AutoLevel);
    if (preset == "speed")
    {
        VTSessionSetProperty(compressionSession,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
        spdlog::info("VideoEncoder: Using 'speed' preset (prioritize speed)");
    }
    else if (preset == "quality")
    {
        VTSessionSetProperty(compressionSession,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanFalse);
        spdlog::info("VideoEncoder: Using 'quality' preset");
    }
    else
    {
        spdlog::info("VideoEncoder: Using 'balanced' preset");
    }

    int avgBitrate = bitrateMbps * 1000000;
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &avgBitrate);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    double peakBytesPerSecond = (double)(bitrateMbps * 1000000) * 1.5 / 8.0;
    NSArray* dataRateLimits = @[@(peakBytesPerSecond), @(1.0)];
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)dataRateLimits);

    uint32_t keyframeIntervalSec = config.keyframeIntervalSec;
    int keyframeInterval = keyframeIntervalSec * std::max(fps, 1u);
    CFNumberRef intervalRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyframeInterval);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, intervalRef);
    CFRelease(intervalRef);

    double keyframeDuration = (double)keyframeIntervalSec;
    CFNumberRef durationRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &keyframeDuration);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, durationRef);
    CFRelease(durationRef);

    int expectedFps = std::max(fps, 1u);
    CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &expectedFps);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);

    int maxFrameDelay = 0;
    CFNumberRef delayRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxFrameDelay);
    VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_MaxFrameDelayCount, delayRef);
    CFRelease(delayRef);

    VTCompressionSessionPrepareToEncodeFrames(compressionSession);
    videoToolbox_.session = compressionSession;

    spdlog::info("VideoEncoder: Initialized H.265 encoder {}x{} @ {}fps, {}Mbps (slots={}, keyframe={}s, preset={})",
                  width, height, fps, bitrateMbps, SlotCount, keyframeIntervalSec, preset);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    if (videoToolbox_.session != nullptr)
    {
        VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)videoToolbox_.session;
        VTCompressionSessionCompleteFrames(compressionSession, kCMTimeInvalid);
        VTCompressionSessionInvalidate(compressionSession);
        CFRelease(compressionSession);
        videoToolbox_.session = nullptr;
    }

    for (int i = 0; i < 200 && inFlightFrameCount_.load() > 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    DestroySlots();

    if (videoToolbox_.scaler != nullptr)
    {
        [(MPSImageBilinearScale*)videoToolbox_.scaler release];
        videoToolbox_.scaler = nullptr;
    }
    if (videoToolbox_.foveationPipeline != nullptr)
    {
        [(id<MTLRenderPipelineState>)videoToolbox_.foveationPipeline release];
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
    CVMetalTextureRef cvMetalTexture = (CVMetalTextureRef)slot.metalTexture;
    id<MTLTexture> dstTexture = CVMetalTextureGetTexture(cvMetalTexture);
    if (pixelBuffer == nullptr || cvMetalTexture == nullptr || dstTexture == nil)
    {
        ReleaseSlot(slotIndex);
        return false;
    }

    id<MTLTexture> leftTex = (__bridge id<MTLTexture>)frameSource.left.GetImage();
    id<MTLTexture> rightTex = stereo ? (__bridge id<MTLTexture>)frameSource.right.GetImage() : nil;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)videoToolbox_.commandQueue;
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

    bool forceKeyframe = forceKeyframe_.exchange(false);
    const bool useFoveatedEncoding = stereo &&
        foveationSettings_.enabled &&
        videoToolbox_.foveationPipeline != nullptr &&
        videoToolbox_.foveationSampler != nullptr;
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
    }

    if (useFoveatedEncoding)
    {
        MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        passDescriptor.colorAttachments[0].texture = dstTexture;
        passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLRenderCommandEncoder> renderEncoder =
            [cmdBuf renderCommandEncoderWithDescriptor:passDescriptor];
        [renderEncoder setRenderPipelineState:(id<MTLRenderPipelineState>)videoToolbox_.foveationPipeline];
        [renderEncoder setFragmentTexture:leftTex atIndex:0];
        [renderEncoder setFragmentTexture:rightTex atIndex:1];
        [renderEncoder setFragmentSamplerState:(id<MTLSamplerState>)videoToolbox_.foveationSampler
                                       atIndex:0];

        MetalFoveationUniforms uniforms = {};
        uniforms.centerSize = {foveationSettings_.centerSizeX, foveationSettings_.centerSizeY};
        uniforms.centerShift = {foveationSettings_.centerShiftX, foveationSettings_.centerShiftY};
        uniforms.edgeRatio = {foveationSettings_.edgeRatioX, foveationSettings_.edgeRatioY};
        uniforms.eyeSizeRatio = {foveationSettings_.eyeWidthRatio, foveationSettings_.eyeHeightRatio};

        for (uint32_t eye = 0; eye < 2; ++eye)
        {
            uniforms.eyeIndex = eye;
            MTLViewport viewport = {
                static_cast<double>(eye * eyeWidth_),
                0.0,
                static_cast<double>(eyeWidth_),
                static_cast<double>(height_),
                0.0,
                1.0,
            };
            [renderEncoder setViewport:viewport];
            [renderEncoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        }
        [renderEncoder endEncoding];
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

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)videoToolbox_.session;
    [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer)
    {
        if (commandBuffer.status != MTLCommandBufferStatusCompleted || this->shuttingDown_.load())
        {
            FinalizeEncodeFrame(context, true);
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
        auto submitStart = Clock::now();
        OSStatus status = VTCompressionSessionEncodeFrame(
            compressionSession,
            pixelBuffer,
            presentationTime,
            kCMTimeInvalid,
            frameProps,
            context,
            nullptr);
        context->metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
        context->encodeSubmitFinished = Clock::now();

        if (frameProps != nullptr)
        {
            CFRelease(frameProps);
        }

        if (status != noErr)
        {
            spdlog::warn("VideoEncoder: VTCompressionSessionEncodeFrame failed: {}", status);
            FinalizeEncodeFrame(context, true);
        }
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
        if (slot.metalTexture != nullptr)
        {
            CFRelease(slot.metalTexture);
            slot.metalTexture = nullptr;
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
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    if (videoToolbox_.session == nullptr || bitrateMbps == bitrateMbps_)
    {
        return;
    }

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)videoToolbox_.session;

    int avgBitrate = bitrateMbps * 1000000;
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &avgBitrate);
    OSStatus status = VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    if (status == noErr)
    {
        double peakBytesPerSecond = (double)(bitrateMbps * 1000000) * 1.5 / 8.0;
        NSArray* dataRateLimits = @[@(peakBytesPerSecond), @(1.0)];
        VTSessionSetProperty(compressionSession,
            kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)dataRateLimits);

        spdlog::info("VideoEncoder: Bitrate changed {} -> {} Mbps", bitrateMbps_, bitrateMbps);
        bitrateMbps_ = bitrateMbps;
    }
    else
    {
        spdlog::warn("VideoEncoder: Failed to set bitrate to {} Mbps: {}", bitrateMbps, status);
    }
}
