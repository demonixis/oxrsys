// SPDX-License-Identifier: MPL-2.0

#import "VideoEncoder.h"
#import "Config.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <VideoToolbox/VideoToolbox.h>

#import <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <thread>

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
    VideoEncoder::FrameMetrics metrics;
    size_t slotIndex = 0;
    Clock::time_point encodeStart;
    Clock::time_point encodeSubmitFinished;
};

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

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                               uint32_t bitrateMbps, GraphicsApi graphicsApi,
                               void* metalDevice)
{
    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    fps_ = fps;
    bitrateMbps_ = bitrateMbps;
    graphicsApi_ = graphicsApi;
    metalDevice_ = metalDevice;
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);
    frameCount_ = 0;

    id<MTLDevice> device = (__bridge id<MTLDevice>)metalDevice;
    if (device == nil)
    {
        spdlog::error("VideoEncoder: No Metal device");
        return false;
    }

    commandQueue_ = (void*)[device newCommandQueue];
    scaler_ = (void*)[[MPSImageBilinearScale alloc] initWithDevice:device];

    CVMetalTextureCacheRef cache = nullptr;
    CVReturn cvResult = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, device, nullptr, &cache);
    if (cvResult != kCVReturnSuccess)
    {
        spdlog::error("VideoEncoder: Failed to create Metal texture cache: {}", cvResult);
        return false;
    }
    textureCache_ = cache;

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
    pixelBufferPool_ = pool;

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
            kCFAllocatorDefault, (CVPixelBufferPoolRef)pixelBufferPool_, &pixelBuffer);
        if (cvResult != kCVReturnSuccess || pixelBuffer == nullptr)
        {
            spdlog::error("VideoEncoder: Failed to preallocate pixel buffer slot {}", i);
            Shutdown();
            return false;
        }

        CVMetalTextureRef cvMetalTexture = nullptr;
        cvResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            (CVMetalTextureCacheRef)textureCache_,
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
    session_ = compressionSession;

    spdlog::info("VideoEncoder: Initialized H.265 encoder {}x{} @ {}fps, {}Mbps (slots={}, keyframe={}s, preset={})",
                  width, height, fps, bitrateMbps, SlotCount, keyframeIntervalSec, preset);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    if (session_ != nullptr)
    {
        VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;
        VTCompressionSessionCompleteFrames(compressionSession, kCMTimeInvalid);
        VTCompressionSessionInvalidate(compressionSession);
        CFRelease(compressionSession);
        session_ = nullptr;
    }

    for (int i = 0; i < 200 && inFlightFrameCount_.load() > 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    DestroySlots();

    if (scaler_ != nullptr)
    {
        [(MPSImageBilinearScale*)scaler_ release];
        scaler_ = nullptr;
    }
    if (commandQueue_ != nullptr)
    {
        [(id<MTLCommandQueue>)commandQueue_ release];
        commandQueue_ = nullptr;
    }
    if (pixelBufferPool_ != nullptr)
    {
        CFRelease(pixelBufferPool_);
        pixelBufferPool_ = nullptr;
    }
    if (textureCache_ != nullptr)
    {
        CFRelease(textureCache_);
        textureCache_ = nullptr;
    }

    spdlog::info("VideoEncoder: Shut down (submitted={} dropped={})",
                  frameCount_, droppedFrameCount_.load());
}

bool VideoEncoder::Encode(void* metalTexture, int64_t timestampNs, OnNalUnitCallback callback,
                           OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(metalTexture, nullptr, false, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(void* leftTexture, void* rightTexture,
                                 int64_t timestampNs, OnNalUnitCallback callback,
                                 OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(leftTexture, rightTexture, true, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(void* leftTexture, void* rightTexture, bool stereo,
                                   int64_t timestampNs, OnNalUnitCallback callback,
                                   OnFrameEncodedCallback frameCallback)
{
    if (session_ == nullptr || leftTexture == nullptr || (stereo && rightTexture == nullptr))
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

    id<MTLTexture> leftTex = (__bridge id<MTLTexture>)leftTexture;
    id<MTLTexture> rightTex = stereo ? (__bridge id<MTLTexture>)rightTexture : nil;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)commandQueue_;
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    if (cmdBuf == nil)
    {
        ReleaseSlot(slotIndex);
        return false;
    }

    bool forceKeyframe = forceKeyframe_.exchange(false);
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
                      needsDownscale);
    }

    if (stereo && needsDownscale)
    {
        id<MTLTexture> tmpLeft = (id<MTLTexture>)slot.tmpLeftTexture;
        id<MTLTexture> tmpRight = (id<MTLTexture>)slot.tmpRightTexture;
        MPSImageBilinearScale* scaler = (MPSImageBilinearScale*)scaler_;
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
        MPSImageBilinearScale* scaler = (MPSImageBilinearScale*)scaler_;
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
    context->slotIndex = slotIndex;
    context->metrics.frameNumber = frameNumberCounter_.fetch_add(1);
    context->metrics.timestampNs = timestampNs;
    context->metrics.keyframe = forceKeyframe;
    context->encodeStart = Clock::now();
    context->encodeSubmitFinished = context->encodeStart;

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;
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
    if (session_ == nullptr || bitrateMbps == bitrateMbps_)
    {
        return;
    }

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;

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
