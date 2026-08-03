// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <oxrsys/protocol/Protocol.h>

/**
 * Transport seam between the Metal compose stage (VideoEncoder) and the
 * hardware encode engine.
 *
 * The seam exists so the compose and streaming-backend code stays
 * transport-agnostic. Two implementations exist: InProcessEncoderTransport
 * wraps VideoToolboxEncodeEngine in the same process (the Rosetta H.264
 * fallback), and NativeHelperEncoderTransport moves the encode into a
 * separate native-arm64 helper process (IOSurface handoff). Everything in
 * this header is deliberately free of ALVR, Metal, and VideoToolbox
 * dependencies.
 */
namespace oxrsys::encoder
{

/** Per-frame encode metrics (field-for-field the legacy VideoEncoder::FrameMetrics). */
struct EncodedFrameMetrics
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

/** One Annex-B NAL unit inside EncodedFrameResult::data (start code included). */
struct NalUnitDescriptor
{
    size_t offset = 0; ///< Byte offset of the 00 00 00 01 start code within data.
    size_t size = 0;   ///< Total bytes including the 4-byte start code.
};

/**
 * One encoded frame as a contiguous Annex-B payload plus per-NAL descriptors.
 * On IDR frames the parameter sets (VPS/SPS/PPS in that order) precede the
 * slice NAL units. `data` is only valid for the duration of the callback.
 */
struct EncodedFrameResult
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<NalUnitDescriptor> nalUnits;
    bool isIdr = false;
    int64_t timestampNs = 0;
};

/** Static encode configuration handed to IEncoderTransport::Configure(). */
struct EncoderConfig
{
    uint32_t width = 0;  ///< Total encoded width (2x eye width for stereo).
    uint32_t height = 0;
    uint32_t fps = 90;
    uint32_t bitrateMbps = 50;
    oxr::protocol::VideoCodec codec = oxr::protocol::VideoCodec::H265;
    bool tenBit = false;              ///< HEVC Main10 (H.265 only).
    std::string encoderPreset;        ///< "speed", "quality", or balanced default.
    uint32_t keyframeIntervalSec = 0;
};

/// Fires once per frame that produced encoded output, before OnFrameCompleteCallback.
using OnEncodedFrameCallback = std::function<void(const EncodedFrameResult& result)>;
/// Fires exactly once per begun frame (encoded or dropped) with the final metrics.
using OnFrameCompleteCallback = std::function<void(const EncodedFrameMetrics& metrics)>;

/**
 * Per-frame sinks. `releaseResources` always runs exactly once, after
 * `onFrameComplete`, and is the place to release compose-side resources
 * (the buffer slot, frame sources); it must therefore stay valid until the
 * frame finishes even if the owner has shut down (capture shared ownership).
 */
struct FrameCallbacks
{
    OnEncodedFrameCallback onEncodedFrame;
    OnFrameCompleteCallback onFrameComplete;
    std::function<void()> releaseResources;
};

/**
 * A frame slot registered with the transport whose composed pixels are not
 * ready yet (GPU compose is asynchronous). Exactly one of Submit()/Cancel()
 * must be called, after which the token is inert. Both paths guarantee the
 * FrameCallbacks contract above. Frames begun but not yet finished count
 * toward IEncoderTransport::Drain().
 */
class IPendingEncodeFrame
{
public:
    virtual ~IPendingEncodeFrame() = default;

    /**
     * Hand the composed slot buffer to the encoder. `composedSlotBuffer` is a
     * CVPixelBufferRef for the in-process transport; the caller keeps it alive
     * until Submit returns. ALL frame bookkeeping happens before the encoder
     * submission inside — the low-latency output path may complete the frame
     * (and run the callbacks) before Submit returns.
     * Returns false when the frame could not be submitted; the frame has then
     * already been finalized as dropped (callbacks fired) and the caller may
     * re-arm any swallowed keyframe request.
     */
    virtual bool Submit(void* composedSlotBuffer, int64_t timestampNs, bool forceKeyframe) = 0;

    /** Finalize the frame as dropped without submitting (compose failed/shutdown). */
    virtual void Cancel() = 0;
};

/**
 * Minimal encoder transport: configure / submit-composed-slot / force-keyframe /
 * set-bitrate / drain / shutdown, with encoded output delivered through the
 * per-frame FrameCallbacks.
 */
class IEncoderTransport
{
public:
    virtual ~IEncoderTransport() = default;

    /** Create and configure the encode session. */
    virtual bool Configure(const EncoderConfig& config) = 0;

    /**
     * Register a frame whose compose is in flight. Returns nullptr when no
     * session is active. Submission of the composed slot happens through the
     * returned token (see IPendingEncodeFrame).
     */
    virtual std::shared_ptr<IPendingEncodeFrame> BeginFrame(
        FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics) = 0;

    /** Request an IDR on the next submitted frame. */
    virtual void ForceKeyframe() = 0;

    /** Live bitrate change; returns false if the encoder rejected it. */
    virtual bool SetBitrate(uint32_t bitrateMbps) = 0;

    /** Flush the encoder and wait (bounded, <=200ms) for in-flight frames. */
    virtual void Drain() = 0;

    /** Destroy the encode session. Call Drain() first. */
    virtual void Shutdown() = 0;
};

} // namespace oxrsys::encoder
