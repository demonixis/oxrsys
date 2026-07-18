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

const char* CodecMimeType(protocol::VideoCodec codec)
{
    switch (codec)
    {
        case protocol::VideoCodec::H264:
            return "video/avc";
        case protocol::VideoCodec::H265:
            return "video/hevc";
        case protocol::VideoCodec::AV1:
        default:
            return nullptr;
    }
}

const char* CodecDisplayName(protocol::VideoCodec codec)
{
    switch (codec)
    {
        case protocol::VideoCodec::H264:
            return "H.264";
        case protocol::VideoCodec::AV1:
            return "AV1";
        case protocol::VideoCodec::H265:
        default:
            return "H.265";
    }
}

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

bool VideoDecoder::Initialize(uint32_t width, uint32_t height, protocol::VideoCodec videoCodec)
{
    const char* mimeType = CodecMimeType(videoCodec);
    if (mimeType == nullptr)
    {
        LOGE("Unsupported video codec: %u", static_cast<uint32_t>(videoCodec));
        return false;
    }

    Shutdown();

    width_ = width;
    height_ = height;
    activeCodec_ = videoCodec;

    // Create AImageReader as the output surface for MediaCodec.
    // We use AHardwareBuffer from the decoded AImage for zero-copy GPU rendering
    // via EGLImage + GL_TEXTURE_EXTERNAL_OES. This avoids CPU YUV access which
    // doesn't work on Quest (Qualcomm UBWC format hides UV plane data).

    // The reader holds the latch queue, the displayed image and two decodes in flight at once
    const int32_t maxImages = static_cast<int32_t>(MaxLatchQueueDepth) + 3;

    media_status_t imageReaderStatus = AImageReader_new(
        width, height,
        AIMAGE_FORMAT_YUV_420_888,
        maxImages,
        &imageReader_);

    if (imageReaderStatus != AMEDIA_OK || imageReader_ == nullptr)
    {
        LOGE("Failed to create AImageReader: %d", imageReaderStatus);
        return false;
    }

    // Get the ANativeWindow from the image reader (used as MediaCodec output surface)
    imageReaderStatus = AImageReader_getWindow(imageReader_, &outputWindow_);

    if (imageReaderStatus != AMEDIA_OK || outputWindow_ == nullptr)
    {
        LOGE("Failed to get ANativeWindow from AImageReader: %d", imageReaderStatus);
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        return false;
    }

    LOGI("AImageReader created: %ux%u, format=YUV_420_888", width, height);

    // Create decoder for the selected stream codec.
    codec_ = AMediaCodec_createDecoderByType(mimeType);
    if (codec_ == nullptr)
    {
        LOGE("Failed to create %s decoder (%s)", CodecDisplayName(activeCodec_), mimeType);
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
        outputWindow_ = nullptr;
        return false;
    }

    // Configure decoder
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mimeType);
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

    LOGI("%s decoder initialized with AImageReader surface: %ux%u mime=%s maxInput=%llu",
         CodecDisplayName(activeCodec_), width, height, mimeType,
         (unsigned long long)maxInputSize);
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

    for (QueuedImage& queued : latchQueue_)
    {
        AImage_delete(queued.image);
    }

    latchQueue_.clear();

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
    pendingFrames_.clear();
}

bool VideoDecoder::SubmitNalUnit(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                                 int64_t targetDisplayClientNs, int64_t receiveTimeNs,
                                 bool alphaBlend)
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

    RememberSubmittedFrame(presentationTimeUs, targetDisplayClientNs, receiveTimeNs, submitTimeNs, alphaBlend);

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

        RecordDecodeDone(info.presentationTimeUs, SteadyClockNowNs());

        // Release to surface (render = true) so AImageReader receives the frame
        AMediaCodec_releaseOutputBuffer(codec_, bufferIndex, true);
        releasedCount++;
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

bool VideoDecoder::AcquireFrame(DecodedFrame* outFrame, int64_t displayTimeNs, int64_t displayPeriodNs)
{
    if (codec_ == nullptr || imageReader_ == nullptr || outFrame == nullptr)
    {
        return false;
    }

    // Move every newly decoded image into the latch queue in decode order
    for (;;)
    {
        AImage* image = nullptr;

        if (AImageReader_acquireNextImage(imageReader_, &image) != AMEDIA_OK || image == nullptr)
        {
            break;
        }

        QueuedImage queued = {};

        queued.image = image;

        int64_t queuedTimestampNs = 0;

        AImage_getTimestamp(image, &queuedTimestampNs);

        queued.presentationTimeUs = queuedTimestampNs / 1000;
        queued.metadataValid = ConsumeSubmittedFrameMetadata(queued.presentationTimeUs, &queued.metadata);

        latchQueue_.push_back(queued);
    }

    uint32_t discarded = 0;

    while (latchQueue_.size() > MaxLatchQueueDepth)
    {
        AImage_delete(latchQueue_.front().image);

        latchQueue_.pop_front();

        discarded++;
    }

    // Latch the newest frame whose intended display tick is this vsync or
    // earlier. Frames aimed at a later tick wait in the queue for their slot
    int dueIndex = -1;

    for (size_t index = 0; index < latchQueue_.size(); index++)
    {
        const QueuedImage& queued = latchQueue_[index];
        const int64_t targetNs = queued.metadataValid ? queued.metadata.targetDisplayClientNs : 0;

        // An untargeted frame is due only at the queue front, so it can
        // never evict a targeted frame that waits for a later slot
        const bool due = targetNs == 0 || displayPeriodNs <= 0 ? index == 0 : targetNs <= displayTimeNs + displayPeriodNs / 2;

        if (due)
        {
            dueIndex = static_cast<int>(index);
        }
    }

    if (dueIndex < 0)
    {
        skippedFramesBeforeAcquire_.fetch_add(discarded);

        return false;
    }

    for (int index = 0; index < dueIndex; index++)
    {
        AImage_delete(latchQueue_.front().image);

        latchQueue_.pop_front();

        discarded++;
    }
    skippedFramesBeforeAcquire_.fetch_add(discarded);

    const QueuedImage latched = latchQueue_.front();

    latchQueue_.pop_front();

    if (currentImage_ != nullptr)
    {
        AImage_delete(currentImage_);
    }

    currentImage_ = latched.image;

    // Get AHardwareBuffer for zero-copy GPU rendering
    AHardwareBuffer* hwBuffer = nullptr;

    const media_status_t status = AImage_getHardwareBuffer(currentImage_, &hwBuffer);

    if (status != AMEDIA_OK || hwBuffer == nullptr)
    {
        LOGE("Failed to get AHardwareBuffer from AImage: %d", status);
        AImage_delete(currentImage_);
        currentImage_ = nullptr;
        return false;
    }

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
             (long long)latched.presentationTimeUs);
    }

    outFrame->hardwareBuffer = hwBuffer;
    outFrame->presentationTimeUs = latched.presentationTimeUs;
    outFrame->bufferWidth = desc.width;
    outFrame->bufferHeight = desc.height;
    outFrame->bufferStride = desc.stride;
    outFrame->cropLeft = cropRect.left;
    outFrame->cropTop = cropRect.top;
    outFrame->cropRight = cropRect.right;
    outFrame->cropBottom = cropRect.bottom;
    outFrame->localAcquireTimeNs = SteadyClockNowNs();
    outFrame->skippedFramesBeforeAcquire = discarded;

    if (latched.metadataValid)
    {
        outFrame->localReceiveTimeNs = latched.metadata.receiveTimeNs;
        outFrame->localSubmitTimeNs = latched.metadata.submitTimeNs;
        outFrame->localDecodeDoneTimeNs = latched.metadata.decodeDoneTimeNs;
        outFrame->targetDisplayClientNs = latched.metadata.targetDisplayClientNs;
        outFrame->alphaBlend = latched.metadata.alphaBlend;
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

void VideoDecoder::RememberSubmittedFrame(int64_t presentationTimeUs,
                                          int64_t targetDisplayClientNs, int64_t receiveTimeNs,
                                          int64_t submitTimeNs, bool alphaBlend)
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
            metadata.alphaBlend = metadata.alphaBlend || alphaBlend;

            if (metadata.targetDisplayClientNs == 0)
            {
                metadata.targetDisplayClientNs = targetDisplayClientNs;
            }

            return;
        }
    }

    PendingFrameMetadata pending = {};

    pending.presentationTimeUs = presentationTimeUs;
    pending.targetDisplayClientNs = targetDisplayClientNs;
    pending.receiveTimeNs = receiveTimeNs;
    pending.submitTimeNs = submitTimeNs;
    pending.alphaBlend = alphaBlend;

    pendingFrames_.push_back(pending);

    while (pendingFrames_.size() > 64)
    {
        pendingFrames_.pop_front();
    }
}

void VideoDecoder::RecordDecodeDone(int64_t presentationTimeUs, int64_t decodeDoneTimeNs)
{
    std::lock_guard<std::mutex> lock(metadataMutex_);

    for (PendingFrameMetadata& metadata : pendingFrames_)
    {
        if (metadata.presentationTimeUs == presentationTimeUs)
        {
            metadata.decodeDoneTimeNs = decodeDoneTimeNs;

            return;
        }
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
