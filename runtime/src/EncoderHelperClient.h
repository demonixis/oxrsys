// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * Runtime-side client for the native-arm64 video encoder helper.
 *
 * The runtime dylib is x86_64 (Rosetta) and cannot reach VideoToolbox's
 * hardware HEVC encoder. This client spawns a native-arm64 helper process that
 * can, hands it the compose IOSurfaces once (zero-copy, via mach send rights),
 * and thereafter submits "encode slot N" requests over a Unix socket, receiving
 * Annex-B NAL units back. If the helper fails to start, fails to obtain the
 * hardware encoder, or dies mid-session, this reports not-alive so the caller
 * falls back to the existing in-process software path — never a black screen.
 *
 * Header stays framework-free (IOSurfaces are passed as opaque void*); the .mm
 * resolves IOSurface / mach types.
 */
class EncoderHelperClient
{
public:
    // Mirrors enc_ipc::CodecCode / ProfileCode. Declared here so the runtime
    // does not have to include the wire header; EncoderHelperClient.mm
    // static_asserts the two against each other.
    enum class Codec : uint32_t
    {
        H265 = 0,
        H264 = 1,
    };
    enum class Profile : uint32_t
    {
        Main = 0,   // HEVC Main / H.264 Main, 8-bit
        Main10 = 1, // HEVC Main10 (10-bit bitstream from the 8-bit compose surface)
    };

    struct Config
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t bitrateMbps = 0;
        uint32_t keyframeIntervalSec = 2;
        uint32_t preset = 0; // 0 balanced, 1 speed, 2 quality
        // Negotiated codec/profile, as enc_ipc::CodecCode / ProfileCode values.
        // The helper honours them exactly or refuses to start; it never
        // substitutes a codec the client did not agree to.
        Codec codec = Codec::H265;
        Profile profile = Profile::Main;
        std::string helperPath;
    };

    // cookie is an opaque token the caller uses to correlate results with the
    // original frame; the helper echoes it back verbatim.
    using OnNal = std::function<void(uint64_t cookie, const uint8_t* data, size_t size,
                                     bool isKeyframe, int64_t ptsNs)>;
    using OnFrameDone = std::function<void(uint64_t cookie, bool dropped, double encodeMs,
                                           bool keyframe)>;
    // Invoked at most once, from the reader/submit thread, when the helper is
    // first found dead after having been alive. Lets the owner reclaim any
    // frames still in flight (whose completions will never arrive) so their
    // slots are released and the software fallback is not starved.
    using OnDied = std::function<void()>;

    EncoderHelperClient() = default;
    ~EncoderHelperClient();

    // Set before Start(). Called once when the helper dies mid-session.
    void SetDiedCallback(OnDied cb) { onDied_ = std::move(cb); }

    EncoderHelperClient(const EncoderHelperClient&) = delete;
    EncoderHelperClient& operator=(const EncoderHelperClient&) = delete;

    // Spawns the helper, transfers the `count` IOSurfaces (index == slot), and
    // waits for its init acknowledgement. `iosurfaces[i]` is an IOSurfaceRef.
    // Returns true only if the helper came up on the hardware encoder.
    bool Start(const Config& config, void* const* iosurfaces, size_t count, OnNal onNal,
               OnFrameDone onFrameDone);

    // True once Start() succeeded and the helper is still responsive.
    bool IsAlive() const { return alive_.load(); }
    bool IsUsingHardware() const { return usingHardware_.load(); }

    // Submit slot for encoding. cookie is echoed back on the NAL/done callbacks.
    // Safe to call from any thread. Returns true once the request is written to
    // the helper, which then owns its completion: FrameDone, or the died
    // callback if it never answers. False means it was never sent (helper not
    // alive, or the write failed - which also marks the helper dead), so no
    // callback will ever mention this cookie and the caller still owns the frame.
    bool SubmitFrame(uint64_t cookie, uint32_t slot, int64_t ptsNs, bool forceKeyframe);

    void SetBitrate(uint32_t bitrateMbps);

    // Orderly shutdown: asks the helper to exit, joins the reader, reaps child.
    void Stop();

    // Diagnostics (and tests): the helper's pid while it runs, -1 otherwise; and
    // the raw waitpid() status Stop() reaped it with, -1 until then (or if it
    // was reaped elsewhere).
    int HelperPid() const { return childPid_; }
    int HelperExitStatus() const { return childExitStatus_; }

private:
    bool SendFramed(uint16_t type, const std::vector<uint8_t>& payload);
    void ReaderLoop();
    void MarkDead(const char* reason);
    // Fires OnDied at most once, from the reader thread as it exits.
    void NotifyDiedIfNeeded();

    int sockFd_ = -1;        // parent end of the control socket
    int stderrReadFd_ = -1;  // parent end of the helper's captured stderr
    int childPid_ = -1;
    int childExitStatus_ = -1;

    std::mutex writeMutex_;      // guards sockFd_ / socketShutdown_ and serializes writes
    bool socketShutdown_ = false; // shutdown(2) done; the fd is closed only in Stop()
    std::atomic<bool> alive_{false};
    std::atomic<bool> everAlive_{false};
    std::atomic<bool> usingHardware_{false};
    std::atomic<bool> stopping_{false};

    std::thread readerThread_;
    std::thread stderrThread_;

    OnNal onNal_;
    OnFrameDone onFrameDone_;
    OnDied onDied_;
    std::atomic<bool> diedNotified_{false};

    uint32_t slotCount_ = 0;
};
