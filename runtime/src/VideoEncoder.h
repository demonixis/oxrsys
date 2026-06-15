// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "GraphicsTypes.h"

/**
 * H.265 video encoder facade.
 *
 * Apple builds use VideoToolbox with Metal textures. Linux builds use FFmpeg
 * and keep backend-specific graphics readback state behind GraphicsContext.
 */
class VideoEncoder
{
public:
    struct FrameMetrics
    {
        uint64_t frameNumber = 0;
        int64_t timestampNs = 0;
        double gpuCopyMs = 0.0;
        double encodeSubmitMs = 0.0;
        double callbackLatencyMs = 0.0;
        double totalLatencyMs = 0.0;
        bool frameDropped = false;
        bool keyframe = false;
    };

    struct FoveationSettings
    {
        bool enabled = false;
        uint32_t targetEyeWidth = 0;
        uint32_t targetEyeHeight = 0;
        float eyeWidthRatio = 1.0f;
        float eyeHeightRatio = 1.0f;
        float centerSizeX = 1.0f;
        float centerSizeY = 1.0f;
        float centerShiftX = 0.0f;
        float centerShiftY = 0.0f;
        float edgeRatioX = 1.0f;
        float edgeRatioY = 1.0f;
    };

    // Callback for each encoded NAL unit
    using OnNalUnitCallback = std::function<void(const uint8_t* data, size_t size,
                                                  bool isKeyframe, int64_t timestampNs)>;
    using OnFrameEncodedCallback = std::function<void(const FrameMetrics& metrics)>;

    VideoEncoder();
    ~VideoEncoder();

    // Non-copyable
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool Initialize(uint32_t width, uint32_t height, uint32_t fps,
                    uint32_t bitrateMbps, const GraphicsContext& graphicsContext);
    void Shutdown();
    void SetFoveationSettings(const FoveationSettings& settings) { foveationSettings_ = settings; }

    // Encode one backend-native texture/image source.
    // The callback is invoked for each NAL unit produced
    bool Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                OnFrameEncodedCallback frameCallback = {});

    // Encode two backend-native texture/image sources side-by-side (left eye | right eye)
    // The combined image has double the width of a single eye
    bool EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                      OnFrameEncodedCallback frameCallback = {});

    // Force a keyframe on the next encode
    void ForceKeyframe();

    // Update encoding bitrate mid-stream (VideoToolbox supports this live)
    void SetBitrate(uint32_t bitrateMbps);
    uint32_t GetBitrateMbps() const { return bitrateMbps_; }

    bool IsInitialized() const
    {
        return videoToolbox_.session != nullptr || ffmpeg_.codecContext != nullptr;
    }

    // Stats
    uint32_t GetEncodedFrameCount() const { return frameCount_; }
    uint32_t GetDroppedFrameCount() const { return droppedFrameCount_.load(); }
    uint32_t GetInFlightFrameCount() const { return inFlightFrameCount_.load(); }

private:
    struct BufferSlot
    {
        void* pixelBuffer = nullptr;      // CVPixelBufferRef
        void* metalTexture = nullptr;     // CVMetalTextureRef
        void* tmpLeftTexture = nullptr;   // id<MTLTexture>
        void* tmpRightTexture = nullptr;  // id<MTLTexture>
        bool inUse = false;
    };

    bool EncodeInternal(FrameSource frameSource, bool stereo,
                        int64_t timestampNs, OnNalUnitCallback callback,
                        OnFrameEncodedCallback frameCallback);
    bool AcquireSlot(size_t& outSlotIndex);
    void ReleaseSlot(size_t slotIndex);
    void DestroySlots();

    struct VideoToolboxState
    {
        void* session = nullptr;          // VTCompressionSessionRef
        void* pixelBufferPool = nullptr;  // CVPixelBufferPoolRef
        void* textureCache = nullptr;     // CVMetalTextureCacheRef
        void* metalDevice = nullptr;      // id<MTLDevice>
        void* commandQueue = nullptr;     // id<MTLCommandQueue>
        void* scaler = nullptr;           // MPSImageBilinearScale*
        void* foveationPipeline = nullptr; // id<MTLRenderPipelineState>
        void* foveationSampler = nullptr;  // id<MTLSamplerState>
    };

    struct FfmpegState
    {
        void* codecContext = nullptr; // AVCodecContext*
        void* frame = nullptr;        // AVFrame*
        void* packet = nullptr;       // AVPacket*
    };

    GraphicsContext graphicsContext_ = {};
    VideoToolboxState videoToolbox_ = {};
    FfmpegState ffmpeg_ = {};

    uint32_t width_ = 0;       // Total encoded width (may be 2x eye width for stereo)
    uint32_t height_ = 0;
    uint32_t eyeWidth_ = 0;   // Single eye width (width_/2 for stereo)
    uint32_t fps_ = 90;
    uint32_t bitrateMbps_ = 50;
    FoveationSettings foveationSettings_ = {};
    uint32_t frameCount_ = 0;
    std::atomic<bool> forceKeyframe_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint32_t> droppedFrameCount_{0};
    std::atomic<uint32_t> inFlightFrameCount_{0};
    std::atomic<uint64_t> frameNumberCounter_{0};
    std::mutex slotMutex_;
    static constexpr size_t SlotCount = 3;
    std::array<BufferSlot, SlotCount> slots_{};
};
