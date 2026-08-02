// SPDX-License-Identifier: MPL-2.0

#import "VideoToolboxEncodeEngine.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#import <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace oxrsys::encoder
{

// Pending-frame bookkeeping shared between the engine and every in-flight
// frame context: the count drives Drain(), the flag backs ForceKeyframe().
// shared_ptr-held so a context finalizing after the engine was torn down
// (late VT/Metal callback past the drain timeout) never dangles.
struct VideoToolboxEncodeEngine::SharedState
{
    std::atomic<uint32_t> pendingFrames{0};
    std::atomic<bool> forceKeyframe{false};
};

namespace
{

using Clock = std::chrono::steady_clock;
using SharedState = VideoToolboxEncodeEngine::SharedState;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

struct EncodeFrameContext
{
    FrameCallbacks callbacks;
    EncodedFrameMetrics metrics;
    oxr::protocol::VideoCodec codec = oxr::protocol::VideoCodec::H265;
    std::shared_ptr<SharedState> shared;
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

    if (context->callbacks.onFrameComplete)
    {
        try
        {
            context->callbacks.onFrameComplete(context->metrics);
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

    if (context->callbacks.releaseResources)
    {
        context->callbacks.releaseResources();
    }

    if (context->shared)
    {
        context->shared->pendingFrames.fetch_sub(1);
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

CMVideoCodecType VideoToolboxCodecType(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return kCMVideoCodecType_H264;
        case oxr::protocol::VideoCodec::H265:
        default:
            return kCMVideoCodecType_HEVC;
    }
}

CFStringRef VideoToolboxProfileLevel(oxr::protocol::VideoCodec codec, bool tenBit)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return kVTProfileLevel_H264_High_AutoLevel;
        case oxr::protocol::VideoCodec::H265:
        default:
            return tenBit ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
    }
}

// Query the parameter-set count and the AVCC/HVCC NAL-unit length-prefix size
// from the format description. The prefix size is used to walk the sample's
// NAL units instead of assuming the usual 4 bytes; fall back to 4 when the
// query fails or reports something outside the spec'd 1..4 range.
void QueryParameterSetLayout(CMFormatDescriptionRef formatDesc,
                             oxr::protocol::VideoCodec codec,
                             size_t& outParamSetCount,
                             int& outNalUnitHeaderLength)
{
    outParamSetCount = 0;
    outNalUnitHeaderLength = 4;

    size_t paramSetCount = 0;
    int nalUnitHeaderLength = 0;
    OSStatus status = noErr;
    if (codec == oxr::protocol::VideoCodec::H264)
    {
        status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            formatDesc, 0, nullptr, nullptr, &paramSetCount, &nalUnitHeaderLength);
    }
    else
    {
        status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            formatDesc, 0, nullptr, nullptr, &paramSetCount, &nalUnitHeaderLength);
    }
    if (status != noErr)
    {
        return;
    }

    outParamSetCount = paramSetCount;
    if (nalUnitHeaderLength >= 1 && nalUnitHeaderLength <= 4)
    {
        outNalUnitHeaderLength = nalUnitHeaderLength;
    }
}

// Emission rank for a parameter set NAL: VPS(0) < SPS(1) < PPS(2) < other(3).
// Classify by the NAL type byte instead of trusting the index order VT
// happens to use (HEVC type = (byte >> 1) & 0x3F: 32=VPS 33=SPS 34=PPS;
// H.264 type = byte & 0x1F: 7=SPS 8=PPS).
int ParameterSetEmissionRank(oxr::protocol::VideoCodec codec, const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return 3;
    }
    if (codec == oxr::protocol::VideoCodec::H264)
    {
        const uint8_t nalType = data[0] & 0x1F;
        if (nalType == 7)
        {
            return 1; // SPS
        }
        if (nalType == 8)
        {
            return 2; // PPS
        }
        return 3;
    }
    const uint8_t nalType = (data[0] >> 1) & 0x3F;
    if (nalType == 32)
    {
        return 0; // VPS
    }
    if (nalType == 33)
    {
        return 1; // SPS
    }
    if (nalType == 34)
    {
        return 2; // PPS
    }
    return 3;
}

void AppendAnnexBNalUnit(std::vector<uint8_t>& payload,
                         std::vector<NalUnitDescriptor>& nalUnits,
                         const uint8_t* data, size_t size)
{
    NalUnitDescriptor descriptor;
    descriptor.offset = payload.size();
    descriptor.size = 4 + size;
    static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    payload.insert(payload.end(), kStartCode, kStartCode + 4);
    payload.insert(payload.end(), data, data + size);
    nalUnits.push_back(descriptor);
}

// Convert one encoded sample from AVCC/HVCC length-prefixed NAL units to a
// contiguous Annex-B payload with per-NAL descriptors. On keyframes the
// parameter sets are prepended in VPS/SPS/PPS order.
void EmitSampleNalUnits(CMSampleBufferRef sampleBuffer, bool isKeyframe,
                        oxr::protocol::VideoCodec codec,
                        const OnEncodedFrameCallback& callback)
{
    if (!callback)
    {
        return;
    }

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t timestampNs = (int64_t)(CMTimeGetSeconds(pts) * 1e9);

    std::vector<uint8_t> payload;
    std::vector<NalUnitDescriptor> nalUnits;

    size_t paramSetCount = 0;
    int nalUnitHeaderLength = 4;
    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (formatDesc != nullptr)
    {
        QueryParameterSetLayout(formatDesc, codec, paramSetCount, nalUnitHeaderLength);
    }

    if (isKeyframe && formatDesc != nullptr)
    {
        struct ParameterSet
        {
            const uint8_t* data = nullptr;
            size_t size = 0;
            int rank = 3;
        };
        std::vector<ParameterSet> parameterSets;
        parameterSets.reserve(paramSetCount);
        for (size_t i = 0; i < paramSetCount; i++)
        {
            const uint8_t* paramSet = nullptr;
            size_t paramSetSize = 0;
            OSStatus status = noErr;
            if (codec == oxr::protocol::VideoCodec::H264)
            {
                status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDesc, i, &paramSet, &paramSetSize, nullptr, nullptr);
            }
            else
            {
                status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    formatDesc, i, &paramSet, &paramSetSize, nullptr, nullptr);
            }
            if (status != noErr || paramSet == nullptr || paramSetSize == 0)
            {
                continue;
            }
            parameterSets.push_back({paramSet, paramSetSize,
                                     ParameterSetEmissionRank(codec, paramSet, paramSetSize)});
        }
        // Stable: parameter sets of the same type keep their original order.
        std::stable_sort(parameterSets.begin(), parameterSets.end(),
                         [](const ParameterSet& a, const ParameterSet& b) { return a.rank < b.rank; });
        for (const ParameterSet& parameterSet : parameterSets)
        {
            AppendAnnexBNalUnit(payload, nalUnits, parameterSet.data, parameterSet.size);
        }
    }

    CMBlockBufferRef dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (dataBuffer != nullptr)
    {
        size_t totalLength = 0;
        char* dataPointer = nullptr;
        if (CMBlockBufferGetDataPointer(dataBuffer, 0, nullptr, &totalLength, &dataPointer) == noErr &&
            dataPointer != nullptr && totalLength != 0)
        {
            size_t offset = 0;
            while (offset + (size_t)nalUnitHeaderLength <= totalLength)
            {
                uint32_t naluLength = 0;
                for (int i = 0; i < nalUnitHeaderLength; i++)
                {
                    naluLength = (naluLength << 8) | (uint8_t)dataPointer[offset + i];
                }
                offset += (size_t)nalUnitHeaderLength;

                if (naluLength == 0 || offset + naluLength > totalLength)
                {
                    break;
                }

                AppendAnnexBNalUnit(payload, nalUnits,
                                    (const uint8_t*)dataPointer + offset, naluLength);
                offset += naluLength;
            }
        }
    }

    if (nalUnits.empty())
    {
        return;
    }

    EncodedFrameResult result;
    result.data = payload.data();
    result.size = payload.size();
    result.nalUnits = std::move(nalUnits);
    result.isIdr = isKeyframe;
    result.timestampNs = timestampNs;
    callback(result);
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

void CompressionOutputCallback(void* /*outputCallbackRefCon*/,
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
        EmitSampleNalUnits(sampleBuffer, isKeyframe, context->codec, context->callbacks.onEncodedFrame);
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

// One in-flight frame. Holds its own retain on the compression session so a
// late Submit()/Cancel() (a Metal completed handler firing after Shutdown
// gave up its bounded drain) never touches a freed session — the same
// discipline as the old per-handler CFRetain.
class PendingEncodeFrame final : public IPendingEncodeFrame
{
public:
    PendingEncodeFrame(VTCompressionSessionRef retainedSession, EncodeFrameContext* context)
        : session_(retainedSession), context_(context)
    {
    }

    ~PendingEncodeFrame() override
    {
        // Exactly-once safety net: a token abandoned without Submit()/Cancel()
        // still finalizes its frame as dropped so the slot is not leaked.
        EncodeFrameContext* context = std::exchange(context_, nullptr);
        if (context != nullptr)
        {
            FinalizeEncodeFrame(context, true);
        }
        if (session_ != nullptr)
        {
            CFRelease(session_);
            session_ = nullptr;
        }
    }

    bool Submit(void* composedSlotBuffer, int64_t timestampNs, bool forceKeyframe) override
    {
        EncodeFrameContext* context = std::exchange(context_, nullptr);
        if (context == nullptr || composedSlotBuffer == nullptr)
        {
            FinalizeEncodeFrame(context, true);
            return false;
        }

        CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)composedSlotBuffer;
        if (context->shared && context->shared->forceKeyframe.exchange(false))
        {
            forceKeyframe = true;
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
            session_,
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
            FinalizeEncodeFrame(context, true);
            return false;
        }
        return true;
    }

    void Cancel() override
    {
        EncodeFrameContext* context = std::exchange(context_, nullptr);
        if (context != nullptr)
        {
            FinalizeEncodeFrame(context, true);
        }
    }

private:
    VTCompressionSessionRef session_ = nullptr;
    EncodeFrameContext* context_ = nullptr;
};

} // namespace

VideoToolboxEncodeEngine::VideoToolboxEncodeEngine()
    : shared_(std::make_shared<SharedState>())
{
}

VideoToolboxEncodeEngine::~VideoToolboxEncodeEngine()
{
    DestroySession();
}

bool VideoToolboxEncodeEngine::CreateSession(const EncoderConfig& config)
{
    DestroySession();
    config_ = config;

    const CMVideoCodecType codecType = VideoToolboxCodecType(config_.codec);

    // Low-latency rate control halves encode latency (33 -> 10.6ms measured)
    // and fixes the ~30% bitrate overshoot of the default RC. Historically its
    // Rosetta all-zero-chroma bug (green image, VT's internal RGB->YCbCr of
    // BGRA) forced an rgb_to_nv12 pre-convert; Apple fixed that in macOS 27
    // and BGRA is fed directly (BT.709 match verified by vt-llrc-probe
    // --matrix; the macOS<27 warning is logged by VideoEncoder::Initialize).
    NSDictionary* encoderSpec = @{
        (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @NO,
        (NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl: @YES,
    };

    VTCompressionSessionRef compressionSession = nullptr;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        config_.width,
        config_.height,
        codecType,
        (__bridge CFDictionaryRef)encoderSpec,
        nullptr,
        kCFAllocatorDefault,
        CompressionOutputCallback,
        nullptr,
        &compressionSession);
    if (status != noErr)
    {
        // No fallback: a non-LL session has different latency and property
        // behavior, and the LL create has never failed on supported hardware
        // (evidence/vt-llrc-probe-rerun-*). Fail loudly instead of degrading.
        spdlog::error("VideoEncoder: Failed to create low-latency compression session ({}); "
                      "VideoToolbox low-latency rate control (macOS 13+) is required",
                      (int)status);
        return false;
    }

    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    // Define one deterministic SDR color contract for every encoded stream. VideoToolbox embeds
    // these values in H.264/H.265 metadata and uses the matching matrix for RGB-to-YCbCr conversion.
    const OSStatus primariesStatus = VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_ColorPrimaries,
        kCVImageBufferColorPrimaries_ITU_R_709_2);
    const OSStatus transferStatus = VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_TransferFunction,
        kCVImageBufferTransferFunction_ITU_R_709_2);
    const OSStatus matrixStatus = VTSessionSetProperty(compressionSession,
        kVTCompressionPropertyKey_YCbCrMatrix,
        kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    if (primariesStatus != noErr || transferStatus != noErr || matrixStatus != noErr)
    {
        spdlog::warn("VideoEncoder: failed to apply complete BT.709 color metadata (primaries={} transfer={} matrix={})",
                     primariesStatus, transferStatus, matrixStatus);
    }

    const std::string& preset = config_.encoderPreset;
    const bool tenBitHevc = config_.tenBit && config_.codec == oxr::protocol::VideoCodec::H265;
    const OSStatus profileStatus = SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_ProfileLevel,
        VideoToolboxProfileLevel(config_.codec, tenBitHevc));
    if (config_.codec == oxr::protocol::VideoCodec::H264)
    {
        // CABAC buys ~10% quality over the CAVLC default at the same bitrate;
        // High profile already implies the decoder supports it.
        SetSessionProperty(compressionSession,
            kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC);
    }
    if (tenBitHevc)
    {
        if (profileStatus == noErr)
        {
            spdlog::info("VideoEncoder: Using HEVC Main10 (10-bit) profile");
        }
        else
        {
            spdlog::warn("VideoEncoder: HEVC Main10 profile unavailable ({}); falling back to encoder default",
                         profileStatus);
        }
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
    int targetBitrate = config_.bitrateMbps * 1000000;
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &targetBitrate);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    // Exact per-second byte budget; headroom above target lets VT overshoot.
    double peakBytesPerSecond = (double)targetBitrate / 8.0;
    NSArray* dataRateLimits = @[@(peakBytesPerSecond), @(1.0)];
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)dataRateLimits);

    uint32_t keyframeIntervalSec = config_.keyframeIntervalSec;
    int keyframeInterval = keyframeIntervalSec * std::max(config_.fps, 1u);
    CFNumberRef intervalRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyframeInterval);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, intervalRef);
    CFRelease(intervalRef);

    double keyframeDuration = (double)keyframeIntervalSec;
    CFNumberRef durationRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &keyframeDuration);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, durationRef);
    CFRelease(durationRef);

    int expectedFps = std::max(config_.fps, 1u);
    CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &expectedFps);
    SetSessionProperty(compressionSession,
        kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);

    VTCompressionSessionPrepareToEncodeFrames(compressionSession);
    session_ = compressionSession;
    shared_->forceKeyframe.store(false);

    {
        CFBooleanRef usingHw = nullptr;
        const OSStatus hwStatus = VTSessionCopyProperty(compressionSession,
            kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &usingHw);
        spdlog::info("VideoEncoder: hardware-accelerated encoder = {}",
            hwStatus != noErr ? "unknown (query unsupported)" : (usingHw && CFBooleanGetValue(usingHw)) ? "yes" : "no");
        if (usingHw) CFRelease(usingHw);
    }

    return true;
}

std::shared_ptr<IPendingEncodeFrame> VideoToolboxEncodeEngine::BeginFrame(
    FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics)
{
    if (session_ == nullptr)
    {
        return nullptr;
    }

    auto* context = new EncodeFrameContext();
    context->callbacks = std::move(callbacks);
    context->metrics = seedMetrics;
    context->codec = config_.codec;
    context->shared = shared_;
    context->encodeStart = Clock::now();
    context->encodeSubmitFinished = context->encodeStart;
    shared_->pendingFrames.fetch_add(1);

    // Retain the session across the frame's async lifetime: the caller's GPU
    // compose completes on another thread, possibly after DestroySession().
    VTCompressionSessionRef retainedSession =
        (VTCompressionSessionRef)CFRetain(session_);
    return std::make_shared<PendingEncodeFrame>(retainedSession, context);
}

void VideoToolboxEncodeEngine::ForceKeyframe()
{
    shared_->forceKeyframe.store(true);
}

bool VideoToolboxEncodeEngine::SetBitrate(uint32_t bitrateMbps)
{
    if (session_ == nullptr)
    {
        return false;
    }

    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;

    // No CBR path here: kVTCompressionPropertyKey_ConstantBitRate is banned —
    // documented incompatible with the AverageBitRate/DataRateLimits pair we
    // rely on (see the rate-control comment in CreateSession()), so only that
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
        spdlog::info("VideoEncoder: Bitrate changed {} -> {} Mbps", config_.bitrateMbps, bitrateMbps);
        config_.bitrateMbps = bitrateMbps;
        return true;
    }
    spdlog::warn("VideoEncoder: Failed to set bitrate to {} Mbps: {}", bitrateMbps, status);
    return false;
}

void VideoToolboxEncodeEngine::Drain()
{
    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;
    if (compressionSession != nullptr)
    {
        VTCompressionSessionCompleteFrames(compressionSession, kCMTimeInvalid);
    }

    // Drain in-flight frames BEFORE invalidating/releasing the session: late
    // Metal completed handlers hold their own retains (via the frame tokens),
    // but invalidating here would yank the session out from under any encode
    // still in flight.
    for (int i = 0; i < 200 && shared_->pendingFrames.load() > 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void VideoToolboxEncodeEngine::DestroySession()
{
    VTCompressionSessionRef compressionSession = (VTCompressionSessionRef)session_;
    if (compressionSession != nullptr)
    {
        VTCompressionSessionInvalidate(compressionSession);
        CFRelease(compressionSession);
        session_ = nullptr;
    }
}

} // namespace oxrsys::encoder
