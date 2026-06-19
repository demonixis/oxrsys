// SPDX-License-Identifier: MPL-2.0

#include "VideoDecoder.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <media/NdkMediaFormat.h>
#include <oxrsys/protocol/Protocol.h>

#define LOG_TAG "OXRSys-Decoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VideoDecoder::~VideoDecoder()
{
    Shutdown();
}

bool VideoDecoder::Initialize(uint32_t width, uint32_t height)
{
    width_ = width;
    height_ = height;

    // Create AImageReader as the output surface for MediaCodec.
    // We use AHardwareBuffer from the decoded AImage for zero-copy GPU rendering
    // via EGLImage + GL_TEXTURE_EXTERNAL_OES. This avoids CPU YUV access which
    // doesn't work on Quest (Qualcomm UBWC format hides UV plane data).
    media_status_t imgStatus = AImageReader_new(
        width, height,
        AIMAGE_FORMAT_YUV_420_888,
        3,  // maxImages — allows a decoder output thread plus one held render image
        &imageReader_);

    if (imgStatus != AMEDIA_OK || imageReader_ == nullptr)
    {
        LOGE("Failed to create AImageReader: %d", imgStatus);
        return false;
    }

    // Get the ANativeWindow from the image reader (used as MediaCodec output surface)
    imgStatus = AImageReader_getWindow(imageReader_, &outputWindow_);
    if (imgStatus != AMEDIA_OK || outputWindow_ == nullptr)
    {
        LOGE("Failed to get ANativeWindow from AImageReader: %d", imgStatus);
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        return false;
    }

    LOGI("AImageReader created: %ux%u, format=YUV_420_888", width, height);

    // Create H.265 decoder
    codec_ = AMediaCodec_createDecoderByType("video/hevc");
    if (codec_ == nullptr)
    {
        LOGE("Failed to create H.265 decoder");
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        outputWindow_ = nullptr;
        return false;
    }

    // Configure decoder
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxInputSize = std::clamp<uint64_t>(
        pixelCount * 4u,
        2u * 1024u * 1024u,
        protocol::TCP_MAX_RECORD_PAYLOAD);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                          static_cast<int32_t>(maxInputSize));

    // Low latency mode (Android 11+)
    AMediaFormat_setInt32(format, "low-latency", 1);

    // Configure with the AImageReader's surface as output
    media_status_t status = AMediaCodec_configure(
        codec_, format, outputWindow_, nullptr, 0);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK)
    {
        LOGE("Failed to configure decoder with surface: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        outputWindow_ = nullptr;
        return false;
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK)
    {
        LOGE("Failed to start decoder: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        outputWindow_ = nullptr;
        return false;
    }

    outputThreadRunning_.store(true);
    outputThread_ = std::thread(&VideoDecoder::OutputThreadMain, this);

    LOGI("H.265 decoder initialized with AImageReader surface: %ux%u maxInput=%llu",
         width, height, (unsigned long long)maxInputSize);
    return true;
}

void VideoDecoder::Shutdown()
{
    outputThreadRunning_.store(false);
    if (outputThread_.joinable())
    {
        outputThread_.join();
    }

    if (currentImage_ != nullptr)
    {
        AImage_delete(currentImage_);
        currentImage_ = nullptr;
    }

    if (codec_ != nullptr)
    {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }

    // outputWindow_ is owned by imageReader_, do not release separately
    outputWindow_ = nullptr;

    if (imageReader_ != nullptr)
    {
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
    }
}

bool VideoDecoder::SubmitNalUnit(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                                 int64_t receiveTimeNs)
{
    if (codec_ == nullptr)
    {
        return false;
    }

    // Get an input buffer (non-blocking)
    ssize_t bufferIndex = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (bufferIndex < 0)
    {
        return false;  // No buffer available, try again next frame
    }

    size_t bufferSize = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, bufferIndex, &bufferSize);
    if (buffer == nullptr || bufferSize < size)
    {
        LOGE("Input buffer too small: %zu < %zu", bufferSize, size);
        AMediaCodec_queueInputBuffer(codec_, bufferIndex, 0, 0, presentationTimeUs, 0);
        return false;
    }

    memcpy(buffer, data, size);
    int64_t submitTimeNs = SteadyClockNowNs();
    AMediaCodec_queueInputBuffer(codec_, bufferIndex, 0, size, presentationTimeUs, 0);
    RememberSubmittedFrame(presentationTimeUs, receiveTimeNs, submitTimeNs);
    return true;
}

uint32_t VideoDecoder::FlushOutputToSurface(int64_t timeoutUs)
{
    // Dequeue all available output buffers and render them to the AImageReader surface.
    // With surface output, we must release buffers with render=true for AImageReader to receive them.
    AMediaCodecBufferInfo info;
    uint32_t releasedCount = 0;
    for (;;)
    {
        ssize_t bufferIndex = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeoutUs);
        timeoutUs = 0;

        if (bufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
            if (fmt)
            {
                const char* fmtStr = AMediaFormat_toString(fmt);
                LOGI("Output format changed: %s", fmtStr ? fmtStr : "(null)");
                AMediaFormat_delete(fmt);
            }
            continue;
        }

        if (bufferIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        {
            continue;
        }

        if (bufferIndex < 0)
        {
            break;  // No more output buffers available
        }

        // Release to surface (render = true) so AImageReader receives the frame
        AMediaCodec_releaseOutputBuffer(codec_, bufferIndex, true);
        releasedCount++;
        outputFramesReleasedSinceAcquire_.fetch_add(1);
    }

    return releasedCount;
}

void VideoDecoder::OutputThreadMain()
{
    LOGI("Decoder output thread started");
    while (outputThreadRunning_.load())
    {
        if (codec_ == nullptr)
        {
            break;
        }
        FlushOutputToSurface(1000);
    }
    LOGI("Decoder output thread ended");
}

bool VideoDecoder::AcquireFrame(DecodedFrame* outFrame)
{
    if (codec_ == nullptr || imageReader_ == nullptr || outFrame == nullptr)
    {
        return false;
    }

    const uint32_t outputFramesReleased = outputFramesReleasedSinceAcquire_.exchange(0);

    // Release previous image if still held
    if (currentImage_ != nullptr)
    {
        AImage_delete(currentImage_);
        currentImage_ = nullptr;
    }

    // Acquire the latest decoded image (drops older images automatically)
    media_status_t status = AImageReader_acquireLatestImage(imageReader_, &currentImage_);
    if (status != AMEDIA_OK || currentImage_ == nullptr)
    {
        return false;  // No image available yet
    }

    // Get AHardwareBuffer for zero-copy GPU rendering
    AHardwareBuffer* hwBuffer = nullptr;
    status = AImage_getHardwareBuffer(currentImage_, &hwBuffer);
    if (status != AMEDIA_OK || hwBuffer == nullptr)
    {
        LOGE("Failed to get AHardwareBuffer from AImage: %d", status);
        AImage_delete(currentImage_);
        currentImage_ = nullptr;
        return false;
    }

    // Get timestamp
    int64_t timestampNs = 0;
    AImage_getTimestamp(currentImage_, &timestampNs);

    int32_t imageWidth = 0;
    int32_t imageHeight = 0;
    if (AImage_getWidth(currentImage_, &imageWidth) != AMEDIA_OK || imageWidth <= 0)
    {
        imageWidth = static_cast<int32_t>(width_);
    }
    if (AImage_getHeight(currentImage_, &imageHeight) != AMEDIA_OK || imageHeight <= 0)
    {
        imageHeight = static_cast<int32_t>(height_);
    }

    AImageCropRect cropRect = {};
    if (AImage_getCropRect(currentImage_, &cropRect) != AMEDIA_OK ||
        cropRect.right <= cropRect.left || cropRect.bottom <= cropRect.top)
    {
        cropRect.left = 0;
        cropRect.top = 0;
        cropRect.right = imageWidth;
        cropRect.bottom = imageHeight;
    }

    // Log first few frames for diagnostics
    static uint32_t frameCount = 0;
    frameCount++;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hwBuffer, &desc);
    if (frameCount <= 5 || frameCount % 300 == 0)
    {
        LOGI("AHardwareBuffer frame #%u: %ux%u format=0x%x stride=%u layers=%u "
             "usage=0x%llx crop=[%d,%d - %d,%d] ts=%lld",
             frameCount, desc.width, desc.height, desc.format, desc.stride,
             desc.layers, (unsigned long long)desc.usage,
             cropRect.left, cropRect.top, cropRect.right, cropRect.bottom,
             (long long)(timestampNs / 1000));
    }

    outFrame->hardwareBuffer = hwBuffer;
    outFrame->presentationTimeUs = timestampNs / 1000;  // ns → us
    outFrame->bufferWidth = desc.width;
    outFrame->bufferHeight = desc.height;
    outFrame->bufferStride = desc.stride;
    outFrame->cropLeft = cropRect.left;
    outFrame->cropTop = cropRect.top;
    outFrame->cropRight = cropRect.right;
    outFrame->cropBottom = cropRect.bottom;
    outFrame->localAcquireTimeNs = SteadyClockNowNs();
    outFrame->skippedFramesBeforeAcquire = outputFramesReleased > 0 ? (outputFramesReleased - 1) : 0;
    skippedFramesBeforeAcquire_.fetch_add(outFrame->skippedFramesBeforeAcquire);

    PendingFrameMetadata metadata = {};
    if (ConsumeSubmittedFrameMetadata(outFrame->presentationTimeUs, &metadata))
    {
        outFrame->localReceiveTimeNs = metadata.receiveTimeNs;
        outFrame->localSubmitTimeNs = metadata.submitTimeNs;
    }

    return true;
}

void VideoDecoder::ReleaseFrame()
{
    if (currentImage_ != nullptr)
    {
        AImage_delete(currentImage_);
        currentImage_ = nullptr;
    }
}

void VideoDecoder::RememberSubmittedFrame(int64_t presentationTimeUs, int64_t receiveTimeNs, int64_t submitTimeNs)
{
    std::lock_guard<std::mutex> lock(metadataMutex_);

    for (PendingFrameMetadata& metadata : pendingFrames_)
    {
        if (metadata.presentationTimeUs == presentationTimeUs)
        {
            metadata.receiveTimeNs = metadata.receiveTimeNs == 0
                ? receiveTimeNs
                : std::min(metadata.receiveTimeNs, receiveTimeNs);
            metadata.submitTimeNs = std::max(metadata.submitTimeNs, submitTimeNs);
            return;
        }
    }

    pendingFrames_.push_back({presentationTimeUs, receiveTimeNs, submitTimeNs});
    while (pendingFrames_.size() > 64)
    {
        pendingFrames_.pop_front();
    }
}

bool VideoDecoder::ConsumeSubmittedFrameMetadata(int64_t presentationTimeUs,
                                                 PendingFrameMetadata* outMetadata)
{
    std::lock_guard<std::mutex> lock(metadataMutex_);
    while (!pendingFrames_.empty())
    {
        PendingFrameMetadata metadata = pendingFrames_.front();
        pendingFrames_.pop_front();

        if (metadata.presentationTimeUs == presentationTimeUs)
        {
            if (outMetadata != nullptr)
            {
                *outMetadata = metadata;
            }
            return true;
        }
    }

    return false;
}

} // namespace oxr
