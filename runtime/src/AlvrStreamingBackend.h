// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef OXRSYS_HAS_ALVR

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AlvrEncoderSelection.h"
#include "AlvrNalFraming.h"
#include "KeyframeRequestLimiter.h"
#include "IStreamingBackend.h"
#include "StreamingFrameQueue.h"
#include "VideoEncoder.h"

class TrackingReceiver;

namespace oxrsys::encoder
{
class NativeHelperEncoderTransport;
}

/**
 * Streaming backend that embeds ALVR's server_core (libalvr_server_core.dylib,
 * pinned tag documented in alvr/alvr_server_core.h). The stock ALVR Quest
 * client connects to it; ALVR owns discovery, transport, trust, statistics and
 * phase sync. oxrsys keeps its own VideoToolbox encoder and OpenXR side.
 *
 * Video: frames from Session::EndFrame are queued latest-only, encoded by
 * oxrsys's VideoToolbox encoder at the ALVR-negotiated stream resolution, and
 * handed to alvr_send_video_nal (one aggregated Annex-B buffer per frame).
 * SPS/PPS parameter sets are detected in the NAL stream and routed to
 * alvr_set_video_config_nals whenever they change.
 *
 * Note: server_core keeps process-global state (SERVER_CORE_CONTEXT); this
 * class must be a singleton per process in practice. Session recreation calls
 * Stop()/Start() which maps to alvr_shutdown()/alvr_initialize().
 */
class AlvrStreamingBackend : public IStreamingBackend
{
public:
    AlvrStreamingBackend();
    ~AlvrStreamingBackend() override;

    AlvrStreamingBackend(const AlvrStreamingBackend&) = delete;
    AlvrStreamingBackend& operator=(const AlvrStreamingBackend&) = delete;

    bool Start(uint32_t renderWidth, uint32_t renderHeight, uint32_t refreshRateHz) override;
    void Stop() override;
    void StopForProcessExit() override;
    // The render-pose arguments are unused: ALVR pairs frames to poses by
    // trackingSampleTimestampNs, not by an explicit pose tag.
    void SendFrame(FrameSource frameSource,
                   const float* renderHeadOrientation,
                   const float* renderHeadPosition) override;
    void SetGraphicsContext(const GraphicsContext& graphicsContext) override
    {
        graphicsContext_ = graphicsContext;
    }
    bool IsClientConnected() const override { return connected_.load(); }
    uint32_t GetTargetRefreshRateHz() const override { return targetRefreshRateHz_.load(); }
    std::string GetClientName() const override;
    TrackingReceiver* GetTrackingReceiver() override { return trackingReceiver_.get(); }
    bool GetFramePacing(int64_t& outSleepNs) override;
    void ApplyHaptics(int hand, float amplitude, float durationSeconds,
                      float frequencyHz) override;

private:
    // Config/payload NAL aggregation lives in AlvrNalFraming.h (unit-tested).
    using PendingEncodedFrame = oxrsys::alvr::PendingEncodedFrame;

    // Distinguishes which oxrsys input a given ALVR button path id feeds.
    enum class ButtonKind
    {
        LeftX,
        LeftY,
        LeftMenu,
        LeftThumbClick,
        LeftThumbX,
        LeftThumbY,
        LeftTriggerClick,
        LeftTriggerValue,
        LeftSqueezeClick,
        LeftSqueezeValue,
        RightA,
        RightB,
        RightSystem,
        RightThumbClick,
        RightThumbX,
        RightThumbY,
        RightTriggerClick,
        RightTriggerValue,
        RightSqueezeClick,
        RightSqueezeValue,
    };

    // Persistent input state, updated from ALVR button events and injected
    // with every tracking sample. Touched only by EventThread.
    struct InputState
    {
        uint32_t buttons = 0;
        float leftTrigger = 0.0f;
        float rightTrigger = 0.0f;
        float leftGrip = 0.0f;
        float rightGrip = 0.0f;
        float leftThumb[2] = {};
        float rightThumb[2] = {};
        float ipd = 0.0f;
        float eyeFov[4] = {};
    };

    void EventThread();
    void EncodeThread();
    bool EnsureEncoder();
    // Shuts down + drops the current encoder generation (and its helper
    // transport) and clears the config-NAL/identity state. EncodeThread only.
    void RetireEncoder();
    void InitInputIds();
    void DrainButtons();
    void InjectTrackingSample(uint64_t sampleTimestampNs);
    // Re-reads the post-negotiation stream resolution and refresh rate from
    // session.json (openvr_config, updated by server_core during handshake).
    // Flags an encoder reset when they changed. Called on ClientConnected.
    void RefreshNegotiatedConfig();
    // Submits codec config (when changed) + the frame to server_core. Runs on
    // the VideoToolbox callback thread (serialized per session). Returns how
    // long alvr_send_video_nal blocked, in milliseconds.
    double SubmitEncodedFrame(PendingEncodedFrame& frame);
    // Aggregates encode/send timings and logs a 1-second summary line used to
    // attribute stutter to a pipeline stage. VideoToolbox callback thread.
    void RecordFrameMetrics(const VideoEncoder::FrameMetrics& metrics, size_t nalBytes,
                            double sendMs);
    // Shared Stop() body; skipAlvrShutdown keeps oxrsys's own threads joining
    // but leaves ServerCoreContext alive (process-exit path).
    void StopInternal(bool skipAlvrShutdown);
    // Writes a minimal session.json (client discovery + auto-trust + wired
    // client entry) when none exists. ALVR extrapolates the rest and owns the
    // file afterwards.
    static void EnsureSessionJson(const std::string& configDir);
    // Slurps session.json into `out`; false if it cannot be opened. Shared by
    // SyncSessionSettings and RefreshNegotiatedConfig.
    bool ReadSessionJson(std::string& out) const;
    // Merge-updates the existing session.json before alvr_initialize so
    // oxrsys-runtime.toml stays the single source of truth for bitrate, and
    // client-side buffering is capped (max_buffering_frames).
    void SyncSessionSettings();

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> targetRefreshRateHz_{72};

    // Latest tracking sample timestamp from the client (ALVR time domain).
    // Video frames must be tagged with a value from this domain.
    std::atomic<uint64_t> latestTrackingTimestampNs_{0};
    std::atomic<bool> keyframeRequested_{false};
    oxrsys::KeyframeRequestLimiter keyframeRequestLimiter_;

    std::thread eventThread_;
    std::thread encodeThread_;
    std::unique_ptr<TrackingReceiver> trackingReceiver_;
    GraphicsContext graphicsContext_ = {};
    std::string sessionJsonPath_;
    std::atomic<uint32_t> streamWidth_{0};
    std::atomic<uint32_t> streamHeight_{0};
    // Set by EventThread when negotiated dims/fps changed; EncodeThread owns
    // the encoder and recreates it on the next frame.
    std::atomic<bool> encoderResetPending_{false};

    StreamingFrameQueue frameQueue_;
    std::shared_ptr<VideoEncoder> encoder_;
    bool encoderUsesH264_ = true;

    // Native-helper transport of the CURRENT encoder generation (null when the
    // encoder is in-process or absent). Restart-per-generation: every encoder
    // rebuild retires it and spawns a fresh one when the decision is Native.
    // Created/destroyed on EncodeThread only.
    std::shared_ptr<oxrsys::encoder::NativeHelperEncoderTransport> nativeTransport_;
    // Identity the current encoder was built with (EncodeThread only). Any
    // drift — dims/fps, codec, bit depth, pixel format, transport mode —
    // forces a rebuild instead of inheriting the encoder.
    oxrsys::alvr::EncoderIdentity encoderIdentity_ = {};
    bool encoderIdentityValid_ = false; // EncodeThread only
    // Cached helper handshake result for HEVC (EncodeThread only): keeps the
    // caps-driven H.264 downgrade stable across identity checks so it cannot
    // oscillate into a rebuild loop. Machine property, so caching is safe.
    bool helperHevcCapKnown_ = false;
    bool helperSupportsHevc_ = true;
    // Helper spawn/crash failures within the current connected session;
    // written by EncodeThread, cleared by EventThread on client disconnect.
    std::atomic<uint32_t> helperFailures_{0};
    std::chrono::steady_clock::time_point lastHelperSpawnAttempt_{}; // EncodeThread only
    bool helperRetryLaterLogged_ = false;         // EncodeThread only
    std::atomic<bool> helperPinLogged_{false};    // set EncodeThread, cleared EventThread

    // ALVR device/input ids (pure path hashes, filled at Start).
    uint64_t headId_ = 0;
    uint64_t handLeftId_ = 0;
    uint64_t handRightId_ = 0;
    std::unordered_map<uint64_t, ButtonKind> buttonIds_;
    InputState inputState_;

    // Last codec config submitted to server_core (compare-before-send).
    // Touched only on the VideoToolbox callback thread.
    std::vector<uint8_t> submittedConfigNals_;

    // 1-second window of encode/send statistics. Written from VideoToolbox
    // callback threads; the mutex keeps the window swap atomic.
    struct EncodeStats
    {
        std::mutex mutex;
        std::chrono::steady_clock::time_point windowStart{};
        std::vector<double> totalMs;
        std::vector<double> callbackMs;
        std::vector<double> gpuCopyMs;
        std::vector<double> sendMs;
        std::vector<double> nalKb;
        uint32_t frames = 0;
        uint32_t drops = 0;
        uint32_t keyframes = 0;
    };
    EncodeStats encodeStats_;
};

#endif // OXRSYS_HAS_ALVR
