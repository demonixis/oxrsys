// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <memory>

#include "EncoderTransport.h"

/**
 * VideoToolbox low-latency encode engine.
 *
 * Owns the VTCompressionSession end of the pipeline: session
 * creation/configuration (LL rate control, BT.709 color contract, profile
 * selection, exact 1.0x rate budget), frame submission, the compression
 * output callback, parameter-set extraction, and AVCC->Annex-B conversion.
 *
 * The engine has no ALVR and no Metal-compose dependencies: composed frames
 * arrive as already-filled CVPixelBufferRefs (passed as void* so this header
 * stays framework-free). Compose (slots, shared-event waits, crop/MPS/blit)
 * lives in VideoEncoder, which drives this engine through
 * InProcessEncoderTransport.
 */
namespace oxrsys::encoder
{

class VideoToolboxEncodeEngine
{
public:
    VideoToolboxEncodeEngine();
    ~VideoToolboxEncodeEngine();

    VideoToolboxEncodeEngine(const VideoToolboxEncodeEngine&) = delete;
    VideoToolboxEncodeEngine& operator=(const VideoToolboxEncodeEngine&) = delete;

    /** Create the LL-RC compression session and apply the full property set. */
    bool CreateSession(const EncoderConfig& config);

    /**
     * Register a frame whose compose is still in flight. The returned token
     * retains the compression session so a late Submit()/Cancel() (e.g. a
     * Metal completed handler firing after shutdown gave up draining) stays
     * memory-safe, exactly like the old per-frame CFRetain. Returns nullptr
     * when no session is active.
     */
    std::shared_ptr<IPendingEncodeFrame> BeginFrame(FrameCallbacks callbacks,
                                                    const EncodedFrameMetrics& seedMetrics);

    /** Arm an IDR request consumed by the next Submit(). */
    void ForceKeyframe();

    /** Live AverageBitRate + DataRateLimits update; false if VT rejected it. */
    bool SetBitrate(uint32_t bitrateMbps);

    /**
     * Flush pending encodes and wait (<=200ms, 1ms steps) for every begun
     * frame to finalize. Must run before DestroySession(): invalidating with
     * an encode in flight would yank the session out from under it.
     */
    void Drain();

    /** Invalidate and release the compression session. */
    void DestroySession();

    bool HasSession() const { return session_ != nullptr; }

    // Pending-frame count + armed keyframe flag; opaque outside the engine
    // translation unit but named here so frame contexts can share ownership.
    struct SharedState;

private:
    void* session_ = nullptr; // VTCompressionSessionRef
    EncoderConfig config_ = {};
    std::shared_ptr<SharedState> shared_;
};

} // namespace oxrsys::encoder
