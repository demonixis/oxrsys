// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <oxrsys/protocol/Protocol.h>

// Pure encoder-selection logic for the ALVR backend, factored out of
// AlvrStreamingBackend so the transport decision (crash fallback / respawn
// budget / pinning) and the encoder generation identity are unit-testable
// without the alvr dylib.
namespace oxrsys::alvr
{

enum class EncoderTransportMode
{
    InProcess,    ///< InProcessEncoderTransport (legacy in-process VideoToolbox)
    NativeHelper, ///< NativeHelperEncoderTransport (out-of-process native arm64)
};

// Which transport EnsureEncoder should build (or keep) for the encoder.
enum class TransportDecision
{
    Native,          ///< use the native helper transport
    InProcess,       ///< use the in-process transport (configured or clean fallback)
    InProcessPinned, ///< helper failed twice this session: in-process H.264 until reconnect
    RetryLater,      ///< encoder_process = "native" but the helper cannot run right now
};

// At most one automatic helper respawn per this interval.
constexpr auto kHelperRespawnBudget = std::chrono::seconds(30);

// Mirrors encoder::ipc::kPixelFormatBGRA (EncoderIpcProtocol.h); kept local
// because this header must stay ipc-free. AlvrStreamingBackend.cpp, which
// includes both, static_asserts the two stay equal.
constexpr uint32_t kPixelFormatBGRA = 0x42475241;

/**
 * Transport decision for one EnsureEncoder pass.
 *
 * @param encoderProcess       validated "auto" | "native" | "inproc" (Config whitelist).
 * @param helperBinaryPresent  helper executable exists at its resolved location.
 * @param helperFailures       helper spawn/crash failures within the current connected
 *                             session (cleared on client disconnect).
 * @param respawnBudgetElapsed >=kHelperRespawnBudget since the last helper spawn attempt.
 * @param helperHealthy        a helper transport is currently up and driving a live
 *                             encoder; the respawn budget never applies to it.
 */
inline TransportDecision DecideEncoderTransport(const std::string& encoderProcess,
                                                bool helperBinaryPresent,
                                                uint32_t helperFailures,
                                                bool respawnBudgetElapsed,
                                                bool helperHealthy)
{
    if (encoderProcess == "inproc")
    {
        return TransportDecision::InProcess;
    }
    if (encoderProcess == "native")
    {
        // Never silently pin: no in-process fallback, keep retrying the helper
        // on the caller's lazy-recreate cadence, throttled by the respawn budget.
        if (helperHealthy)
        {
            return TransportDecision::Native;
        }
        if (!helperBinaryPresent || (helperFailures > 0 && !respawnBudgetElapsed))
        {
            return TransportDecision::RetryLater;
        }
        return TransportDecision::Native;
    }
    // "auto"
    if (helperFailures >= 2)
    {
        return TransportDecision::InProcessPinned;
    }
    if (helperHealthy)
    {
        return TransportDecision::Native;
    }
    if (!helperBinaryPresent)
    {
        return TransportDecision::InProcess;
    }
    if (helperFailures > 0 && !respawnBudgetElapsed)
    {
        return TransportDecision::InProcess;
    }
    return TransportDecision::Native;
}

/**
 * Identity of one encoder generation. Any field change means the running
 * encoder must not be reused: EnsureEncoder rebuilds it (for the helper
 * transport that is a fresh generation/helper restart — restart-per-generation
 * by design) and the config NALs are re-sent for the new generation.
 */
struct EncoderIdentity
{
    uint32_t totalWidth = 0; ///< 2x eye width (side-by-side stereo)
    uint32_t height = 0;
    uint32_t fps = 0;
    oxr::protocol::VideoCodec codec = oxr::protocol::VideoCodec::H265;
    uint32_t bitDepth = 8;             ///< 8, or 10 for HEVC Main10
    uint32_t pixelFormat = kPixelFormatBGRA; ///< fourcc of the compose target ('BGRA')
    EncoderTransportMode transport = EncoderTransportMode::InProcess;

    // Defaulted: a field added above is compared automatically. The hand-written
    // form this replaced would have silently ignored it, reusing an encoder that
    // should have been rebuilt.
    bool operator==(const EncoderIdentity&) const = default;
};

} // namespace oxrsys::alvr
