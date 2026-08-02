// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <memory>

#include "EncoderTransport.h"

/**
 * Out-of-process IEncoderTransport: spawns the native-arm64
 * `oxrsys-encoder-helper` next to the runtime dylib (override:
 * OXRSYS_ENCODER_HELPER env, used by tests), hands it the compose ring's
 * IOSurfaces by Mach send right (EncoderMachSurface.h), and drives frames
 * over the framed socket protocol (EncoderIpcProtocol.h).
 *
 * Contract highlights:
 *  - Configure() spawns + handshakes on first use. A translated (Rosetta)
 *    child or a child without hardware low-latency support for the requested
 *    codec is rejected — the whole point of the helper is native HW encode.
 *  - Submission never blocks a time-critical thread: FrameSubmit goes through
 *    a bounded (3-deep) writer queue; enqueue failure finalizes the frame as
 *    dropped immediately.
 *  - Every submitted frame reaches exactly ONE terminal transition: a
 *    FrameResult consumed, a FrameDropped consumed, or reclaimed-as-dropped
 *    on helper EOF/crash. Per-generation state is retained until its last
 *    outstanding frame resolves; results for a stale generation can never
 *    release the current generation's slots.
 *  - Crash detection: socket EOF is primary, waitpid(WNOHANG) reaps.
 *  - Shutdown(): Drain -> Shutdown/Ack -> close socket -> SIGTERM (process
 *    group) -> bounded wait -> SIGKILL -> reap. StopForProcessExit() is the
 *    bounded fast path (close + SIGTERM, no waits beyond a WNOHANG reap) for
 *    process-exit paths that must never hang.
 */
namespace oxrsys::encoder
{

class NativeHelperEncoderTransport final : public IEncoderTransport
{
public:
    NativeHelperEncoderTransport();
    ~NativeHelperEncoderTransport() override;

    bool Configure(const EncoderConfig& config) override;
    std::shared_ptr<IPendingEncodeFrame> BeginFrame(
        FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics) override;
    void ForceKeyframe() override;
    bool SetBitrate(uint32_t bitrateMbps) override;
    void Drain() override;
    void Shutdown() override;

    /// Bounded process-exit fast path (see class comment).
    void StopForProcessExit();

    /// False once the helper died (EOF) or was never successfully configured.
    bool IsHealthy() const;

    /// True when the helper executable exists (and is executable) at its
    /// resolved location: the OXRSYS_ENCODER_HELPER override or next to the
    /// runtime dylib. Cheap enough to poll per encoder (re)build.
    static bool HelperBinaryAvailable();

    /// Spawn + handshake the helper if it is not already up. False when the
    /// spawn, the handshake, or the native-arm64 requirement fails; a
    /// transport whose helper died is single-use and stays failed.
    bool StartHelper();

    /// The helper advertised hardware low-latency support for `codec` in its
    /// handshake caps. Only meaningful after StartHelper() succeeded.
    bool SupportsCodec(oxr::protocol::VideoCodec codec) const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace oxrsys::encoder
