// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GraphicsTypes.h"
#include "BoundedDrain.h"
#include "EncoderPathPolicy.h"
#include <oxrsys/protocol/Protocol.h>

// Runtime-side client for the out-of-process native-arm64 hardware HEVC
// encoder helper (see runtime/encoder_helper/README.md).
class EncoderHelperClient;

/**
 * Low-latency video encoder facade.
 *
 * macOS builds use VideoToolbox with Metal textures. Backend-specific graphics
 * snapshot state stays behind GraphicsContext.
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

    struct BackendCapabilities
    {
        std::string backendName;
        bool hardwareEncoder = false;
        bool supportsH264 = false;
        bool supportsH265 = false;
        bool supportsTenBitH265 = false;
        bool supportsFoveatedEncoding = false;
        std::string unsupportedReason;

        bool SupportsCodec(oxr::protocol::VideoCodec codec) const
        {
            switch (codec)
            {
                case oxr::protocol::VideoCodec::H264:
                    return supportsH264;
                case oxr::protocol::VideoCodec::H265:
                    return supportsH265;
                case oxr::protocol::VideoCodec::AV1:
                default:
                    return false;
            }
        }
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
    // Stops new submissions and waits only up to timeout for Metal and
    // VideoToolbox callbacks. A timeout keeps all backing resources alive and
    // is safe to retry; true means every callback released its FrameSource.
    bool Shutdown(std::chrono::nanoseconds timeout = std::chrono::milliseconds(500));
    void SetFoveationSettings(const FoveationSettings& settings) { foveationSettings_ = settings; }
    // Applies before Initialize(); only the H.265 VideoToolbox path supports Main10.
    void SetTenBitEncoding(bool enabled) { tenBit_ = enabled; }
    // Diagnostics (and tests): pid of the live out-of-process encoder helper
    // frames are currently sent to, or -1 when they are encoded in-process.
    int EncoderHelperPid() const;
    // Diagnostics (and tests): whether the in-process VideoToolbox session
    // Initialize() created is hardware-accelerated. A software session is fed a
    // video-range 4:2:0 conversion of each frame (EncoderSessionColor.h).
    bool InProcessSessionUsesHardware() const { return inProcessSessionHardware_; }
    // Tests only, before Initialize(): create the in-process session on the
    // software encoder and never start the helper, so the software path the
    // helper-death fallback lands on under Rosetta can be exercised anywhere.
    void SetForceSoftwareEncoderForTesting(bool enabled) { forceSoftwareForTesting_ = enabled; }
    static bool SupportsFoveatedEncoding(const GraphicsContext& graphicsContext);
    static BackendCapabilities QueryBackendCapabilities(const GraphicsContext* graphicsContext = nullptr);
    static bool SupportsCodec(oxr::protocol::VideoCodec codec);

    // Drain frames VideoToolbox still holds, through the normal callbacks. Frames reach it from
    // a Metal completion handler, so this retries until nothing is in flight. Blocks.
    bool FlushPendingFrames(std::chrono::milliseconds timeout = std::chrono::seconds(5));

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

    bool IsInitialized() const { return initialized_.load(std::memory_order_acquire); }

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

    // Software in-process sessions only (EncoderSessionColor.h): sets up, and
    // per frame performs, the BGRA -> video-range 4:2:0 conversion. Convert
    // returns a +1 CVPixelBufferRef, or nullptr to encode the BGRA slot as is.
    bool CreateVideoRangeConversion(uint32_t width, uint32_t height);
    void* ConvertToVideoRange(void* bgraPixelBuffer);

    // Native-arm64 hardware encoder helper integration. When the helper starts and
    // reports the hardware encoder, the per-frame VideoToolbox encode is
    // delegated to it (out-of-process, native arm64); the in-process session
    // stays live as the fallback for helper death.
    bool TryStartHelper();
    // Stops the helper and finalizes every frame still in flight to it, so their
    // slots and callback-drain leases are released. The client object itself is
    // released only once the callback drain is empty (ReleaseHelperClient), so a
    // Metal completion handler racing teardown cannot use a freed client.
    void StopHelper();
    void ReleaseHelperClient();
    std::shared_ptr<EncoderHelperClient> AcquireHelperClient() const;
    void ReclaimHelperFrames(const char* reason);
    // Invoked from the helper client's reader thread. `cookie` is the sequence
    // number the frame was submitted with, the key of its EncodeFrameContext in
    // helperContexts_ (opaque across the IPC).
    void OnHelperNal(uint64_t cookie, const uint8_t* data, size_t size, bool keyframe,
                     int64_t ptsNs);
    void OnHelperFrameDone(uint64_t cookie, bool dropped, double encodeMs, bool keyframe);
    // Reclaims every frame in flight to a helper that just died, so the
    // in-process fallback is not starved of slots.
    void OnHelperDied();

    struct VideoToolboxState
    {
        void* session = nullptr;          // VTCompressionSessionRef
        void* pixelBufferPool = nullptr;  // CVPixelBufferPoolRef
        void* textureCache = nullptr;     // CVMetalTextureCacheRef
        void* metalDevice = nullptr;      // id<MTLDevice>
        void* commandQueue = nullptr;     // id<MTLCommandQueue>
        void* scaler = nullptr;           // MPSImageBilinearScale*
        void* copyPipeline = nullptr;      // id<MTLComputePipelineState>
        void* copySampler = nullptr;       // id<MTLSamplerState>
        void* foveationPipeline = nullptr; // id<MTLComputePipelineState>
        void* foveationSampler = nullptr;  // id<MTLSamplerState>
        // Software sessions only: BGRA -> video-range 4:2:0 before encode.
        void* videoRangeTransfer = nullptr; // VTPixelTransferSessionRef
        void* videoRangePool = nullptr;     // CVPixelBufferPoolRef
    };

    GraphicsContext graphicsContext_ = {};
    VideoToolboxState videoToolbox_ = {};
    uint32_t width_ = 0;       // Total encoded width (may be 2x eye width for stereo)
    uint32_t height_ = 0;
    uint32_t eyeWidth_ = 0;   // Single eye width (width_/2 for stereo)
    uint32_t fps_ = 90;
    uint32_t bitrateMbps_ = 50;
    oxr::protocol::VideoCodec codec_ = oxr::protocol::VideoCodec::H265;
    FoveationSettings foveationSettings_ = {};
    bool tenBit_ = false;
    bool forceSoftwareForTesting_ = false;
    bool inProcessSessionHardware_ = false;

    // Which process encodes, and why — decided once in Initialize() from an
    // actual VideoToolbox hardware-encoder query plus the encoder_helper
    // override, and used both to pick RequireHardware for the in-process
    // session and to decide whether to spawn the helper at all.
    oxrsys::encoder::EncodePathDecision encodePath_ = {};

    // Out-of-process native-arm64 hardware encoder helper. useHelper_ is set only
    // once the helper is up AND reports the hardware encoder; if it dies
    // mid-session the client reports not-alive and EncodeInternal reverts to the
    // in-process session (never a black screen).
    mutable std::mutex helperClientMutex_;
    std::shared_ptr<EncoderHelperClient> helperClient_;
    std::atomic<bool> useHelper_{false};
    std::mutex helperContextMutex_;
    std::unordered_map<uint64_t, void*> helperContexts_; // cookie -> EncodeFrameContext*
    // Source of helper cookies: unique for the encoder's lifetime, unlike a
    // context address, which the allocator may hand to the next frame.
    std::atomic<uint64_t> nextHelperCookie_{0};
    std::atomic_bool initialized_{false};
    uint32_t frameCount_ = 0;
    std::atomic<bool> forceKeyframe_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> foveationValidationWarningLogged_{false};
    std::atomic<bool> videoRangeConversionWarningLogged_{false};
    std::atomic<uint32_t> droppedFrameCount_{0};
    std::atomic<uint32_t> inFlightFrameCount_{0};
    std::atomic<uint64_t> frameNumberCounter_{0};
    std::mutex slotMutex_;
    BoundedDrain callbackDrain_;
    // Serializes VTCompressionSessionEncodeFrame against session invalidation.
    // The OpenXR teardown thread never performs the potentially blocking VT
    // drain itself; the shutdown worker owns that operation.
    std::mutex videoToolboxSessionMutex_;
    std::mutex shutdownMutex_;
    std::condition_variable shutdownCondition_;
    std::thread shutdownThread_;
    bool sessionShutdownStarted_ = false;
    bool sessionShutdownComplete_ = true;
    bool resourcesDestroyed_ = true;
    static constexpr size_t SlotCount = 3;
    std::array<BufferSlot, SlotCount> slots_{};
};
