// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <media/NdkMediaCodec.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <mutex>
#include <oxrsys/protocol/Protocol.h>
#include <thread>

struct ANativeWindow;
struct AHardwareBuffer;

namespace oxr
{

/**
 * Hardware video decoder using Android MediaCodec.
 *
 * Receives NAL units from the network, feeds them to the hardware decoder,
 * and outputs decoded frames via AImageReader → AHardwareBuffer for zero-copy
 * GPU rendering via EGLImage + GL_TEXTURE_EXTERNAL_OES.
 *
 * CPU-based YUV plane access doesn't work on Quest (Qualcomm UBWC format),
 * so we use the GPU path which handles all proprietary YUV layouts natively.
 */
class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    // Non-copyable
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Initialize(uint32_t width, uint32_t height, protocol::VideoCodec codec);
    void Shutdown();

    // Feeds an encoded video NAL unit to the decoder. targetDisplayClientNs
    // names the display slot the server aimed the frame at, or 0 when the
    // server has not settled on a target
    bool SubmitNalUnit(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                       int64_t targetDisplayClientNs, int64_t receiveTimeNs, bool alphaBlend);

    // Decoded frame providing an AHardwareBuffer for GPU rendering
    struct DecodedFrame
    {
        AHardwareBuffer* hardwareBuffer = nullptr;
        int64_t presentationTimeUs = 0;
        uint32_t bufferWidth = 0;
        uint32_t bufferHeight = 0;
        uint32_t bufferStride = 0;
        int32_t cropLeft = 0;
        int32_t cropTop = 0;
        int32_t cropRight = 0;
        int32_t cropBottom = 0;
        int64_t localReceiveTimeNs = 0;
        int64_t localSubmitTimeNs = 0;
        int64_t localDecodeDoneTimeNs = 0;
        int64_t localAcquireTimeNs = 0;
        int64_t targetDisplayClientNs = 0;   // Display slot named by the server, 0 = unknown
        uint32_t skippedFramesBeforeAcquire = 0;
        bool alphaBlend = false;
    };

    // Latches the decoded frame due for the given display tick. Frames aimed
    // at later ticks wait in an internal queue until their slot arrives.
    // Returns false when no frame is due. The AHardwareBuffer stays valid
    // until the next successful AcquireFrame call
    bool AcquireFrame(DecodedFrame* outFrame, int64_t displayTimeNs, int64_t displayPeriodNs);
    void ReleaseFrame();

    bool IsInitialized() const { return codec_ != nullptr; }
    protocol::VideoCodec GetCodec() const { return activeCodec_; }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetSkippedFramesBeforeAcquire() const { return skippedFramesBeforeAcquire_.load(); }

private:
    struct PendingFrameMetadata
    {
        int64_t presentationTimeUs = 0;
        int64_t targetDisplayClientNs = 0;
        int64_t receiveTimeNs = 0;
        int64_t submitTimeNs = 0;
        int64_t decodeDoneTimeNs = 0;
        bool alphaBlend = false;
    };

    // A decoded image waiting in the latch queue, together with the frame
    // metadata consumed when the image was drained from the reader
    struct QueuedImage
    {
        AImage* image = nullptr;
        int64_t presentationTimeUs = 0;
        PendingFrameMetadata metadata = {};
        bool metadataValid = false;
    };

    // Three frames absorb delivery jitter, a deeper queue only adds latency
    static constexpr size_t MaxLatchQueueDepth = 3;

    // Dequeue all available output buffers and render them to the surface
    uint32_t FlushOutputToSurface(int64_t timeoutUs);
    void OutputThreadMain();
    void RememberSubmittedFrame(int64_t presentationTimeUs, int64_t targetDisplayClientNs,
                                int64_t receiveTimeNs, int64_t submitTimeNs, bool alphaBlend);

    // Stamps decode completion time onto the pending frame metadata
    void RecordDecodeDone(int64_t presentationTimeUs, int64_t decodeDoneTimeNs);
    bool ConsumeSubmittedFrameMetadata(int64_t presentationTimeUs, PendingFrameMetadata* outMetadata);

    AMediaCodec* codec_ = nullptr;
    protocol::VideoCodec activeCodec_ = protocol::VideoCodec::H265;
    AImageReader* imageReader_ = nullptr;
    ANativeWindow* outputWindow_ = nullptr;     // Owned by AImageReader, do not release
    AImage* currentImage_ = nullptr;
    std::deque<QueuedImage> latchQueue_;
    std::thread outputThread_;
    std::atomic<bool> outputThreadRunning_{false};

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::mutex metadataMutex_;
    std::deque<PendingFrameMetadata> pendingFrames_;
    std::atomic<uint32_t> skippedFramesBeforeAcquire_{0};
};

} // namespace oxr
