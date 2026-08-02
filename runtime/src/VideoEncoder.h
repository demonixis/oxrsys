// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "GraphicsTypes.h"
#include "encoder/EncoderTransport.h"
#include <oxrsys/protocol/Protocol.h>

/**
 * Low-latency video encoder facade.
 *
 * Apple builds compose with Metal and encode through the transport seam in
 * encoder/EncoderTransport.h (today: InProcessEncoderTransport wrapping the
 * VideoToolbox engine). Linux builds use FFmpeg and keep backend-specific
 * graphics readback state behind GraphicsContext. macOS can opt into FFmpeg
 * for codec/pipeline testing.
 */
class VideoEncoder : public std::enable_shared_from_this<VideoEncoder>
{
public:
    // Field-for-field the transport-side metrics struct; kept as a nested
    // name for existing callers.
    using FrameMetrics = oxrsys::encoder::EncodedFrameMetrics;

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
                    uint32_t bitrateMbps, const GraphicsContext& graphicsContext,
                    oxr::protocol::VideoCodec codec);
    void Shutdown();
    void SetFoveationSettings(const FoveationSettings& settings) { foveationSettings_ = settings; }
    // Applies before Initialize(); only the H.265 VideoToolbox path supports Main10.
    void SetTenBitEncoding(bool enabled) { tenBit_ = enabled; }
    // Inject the encode transport to drive (must be un-configured; Initialize()
    // calls Configure() on it). Applies before Initialize(); when unset, Apple
    // builds default to InProcessEncoderTransport. The injected transport is
    // single-use — Shutdown() retires it with the encoder.
    void SetEncoderTransport(std::shared_ptr<oxrsys::encoder::IEncoderTransport> transport)
    {
        injectedTransport_ = std::move(transport);
    }
    static bool SupportsFoveatedEncoding(const GraphicsContext& graphicsContext);

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
    oxr::protocol::VideoCodec GetCodec() const { return codec_; }

    bool IsInitialized() const
    {
        return transport_ != nullptr || ffmpeg_.codecContext != nullptr;
    }

    // Stats
    uint32_t GetEncodedFrameCount() const { return frameCount_; }
    uint32_t GetDroppedFrameCount() const { return droppedFrameCount_.load(); }
    uint32_t GetInFlightFrameCount() const { return inFlightFrameCount_.load(); }

private:
    struct BufferSlot
    {
        void* pixelBuffer = nullptr;      // CVPixelBufferRef (32BGRA, IOSurface-backed)
        void* compositeTexture = nullptr; // CVMetalTextureRef (BGRA view of pixelBuffer; compose target = encoder input)
        void* tmpLeftTexture = nullptr;   // id<MTLTexture>
        void* tmpRightTexture = nullptr;  // id<MTLTexture>
        void* foveatedScratchTexture = nullptr; // id<MTLTexture>
        void* leftCropTexture = nullptr;   // id<MTLTexture>, lazily (re)sized to sourceWidth/sourceHeight
        void* rightCropTexture = nullptr;  // id<MTLTexture>, lazily (re)sized to sourceWidth/sourceHeight
        bool inUse = false;
    };

    bool EncodeInternal(FrameSource frameSource, bool stereo,
                        int64_t timestampNs, OnNalUnitCallback callback,
                        OnFrameEncodedCallback frameCallback);
    bool AcquireSlot(size_t& outSlotIndex);
    void ReleaseSlot(size_t slotIndex);
    void DestroySlots();

    // Metal compose infrastructure only; the VTCompressionSession lives in
    // the encode engine behind transport_.
    struct VideoToolboxState
    {
        void* pixelBufferPool = nullptr;  // CVPixelBufferPoolRef
        void* textureCache = nullptr;     // CVMetalTextureCacheRef
        void* metalDevice = nullptr;      // id<MTLDevice>
        void* commandQueue = nullptr;     // id<MTLCommandQueue>
        void* scaler = nullptr;           // MPSImageBilinearScale*
        void* foveationPipeline = nullptr; // id<MTLComputePipelineState>
        void* foveationSampler = nullptr;  // id<MTLSamplerState>
    };

    struct FfmpegState
    {
        void* codecContext = nullptr; // AVCodecContext*
        void* frame = nullptr;        // AVFrame*
        void* packet = nullptr;       // AVPacket*
        void* readbackState = nullptr;
    };

    GraphicsContext graphicsContext_ = {};
    VideoToolboxState videoToolbox_ = {};
    FfmpegState ffmpeg_ = {};
    // Encode seam (Apple/VideoToolbox builds); null until Initialize succeeds.
    std::shared_ptr<oxrsys::encoder::IEncoderTransport> transport_;
    // Optional caller-selected transport (SetEncoderTransport), consumed by
    // Initialize(); null selects the in-process default.
    std::shared_ptr<oxrsys::encoder::IEncoderTransport> injectedTransport_;

    uint32_t width_ = 0;       // Total encoded width (may be 2x eye width for stereo)
    uint32_t height_ = 0;
    uint32_t eyeWidth_ = 0;   // Single eye width (width_/2 for stereo)
    uint32_t fps_ = 90;
    uint32_t bitrateMbps_ = 50;
    oxr::protocol::VideoCodec codec_ = oxr::protocol::VideoCodec::H265;
    FoveationSettings foveationSettings_ = {};
    bool tenBit_ = false;
    uint32_t frameCount_ = 0;
    std::atomic<bool> forceKeyframe_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> foveationValidationWarningLogged_{false};
    std::atomic<uint32_t> droppedFrameCount_{0};
    std::atomic<uint32_t> inFlightFrameCount_{0};
    std::atomic<uint64_t> frameNumberCounter_{0};
    std::mutex slotMutex_;
    static constexpr size_t SlotCount = 3;
    std::array<BufferSlot, SlotCount> slots_{};
};
