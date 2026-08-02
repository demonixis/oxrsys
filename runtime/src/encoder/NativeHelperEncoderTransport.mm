// SPDX-License-Identifier: MPL-2.0

#import "NativeHelperEncoderTransport.h"

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#import <spdlog/spdlog.h>

#include <mach/mach_time.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "EncoderIpcProtocol.h"
#include "EncoderIpcSocket.h"
#include "EncoderMachSurface.h"
#include "RuntimePlatform.h"

extern char** environ;

namespace oxrsys::encoder
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr size_t kSubmitQueueDepth = 3; ///< matches the 3-slot compose ring
constexpr uint32_t kHandshakeTimeoutMs = 5000;
constexpr uint32_t kConfigureTimeoutMs = 5000;
constexpr uint32_t kDrainBudgetMs = 200;
constexpr uint32_t kShutdownAckTimeoutMs = 500;
constexpr uint32_t kTermWaitMs = 500;

uint64_t NowNs()
{
    static mach_timebase_info_data_t timebase = {};
    if (timebase.denom == 0)
    {
        mach_timebase_info(&timebase);
    }
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

double ToMs(Clock::duration d)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

std::string LocateHelperBinary()
{
    if (const char* override = getenv("OXRSYS_ENCODER_HELPER");
        override != nullptr && override[0] != '\0')
    {
        return override;
    }
    const std::string dir =
        runtime_platform::ModuleDirectory((const void*)&LocateHelperBinary);
    return dir + "/oxrsys-encoder-helper";
}

// One frame the parent has submitted and not yet resolved.
struct OutstandingFrame
{
    FrameCallbacks callbacks;
    EncodedFrameMetrics metrics;
    Clock::time_point begin;
    Clock::time_point submitted;
};

// Per-generation state, retained until every submitted frame of the
// generation reached exactly one terminal transition. Slot mapping lives here
// so stale-generation results can never touch a newer generation's slots.
struct GenerationState
{
    uint32_t generation = 0;
    EncoderConfig config = {};
    std::map<uint32_t, uint32_t> surfaceSlots; ///< IOSurfaceID -> slot
    std::map<uint64_t, OutstandingFrame> outstanding; ///< frameId -> frame
};

struct QueuedMessage
{
    ipc::MessageType type;
    std::vector<uint8_t> payload;
    bool isFrame = false;
};

} // namespace

struct NativeHelperEncoderTransport::Impl
{
    // --- process/link state ---
    ipc::EncoderIpcSocket socket;
    mach_surface::ParentSurfaceRendezvous rendezvous;
    pid_t childPid = -1;
    std::atomic<bool> alive{false};
    std::atomic<bool> stopping{false};
    ipc::HelloReply childCaps = {};
    bool spawned = false;

    std::thread readerThread;
    std::thread writerThread;

    // --- writer queues (control unbounded/tiny, frames bounded) ---
    std::mutex writeMutex;
    std::condition_variable writeCondition;
    std::deque<QueuedMessage> controlQueue;
    std::deque<QueuedMessage> frameQueue;

    // --- control-plane responses (reader -> waiting caller) ---
    std::mutex controlMutex;
    std::condition_variable controlCondition;
    std::optional<ipc::ConfigureAck> configureAck;
    std::optional<uint32_t> drainCompleteGeneration;
    bool shutdownAckReceived = false;

    // --- frame/generation state ---
    std::mutex stateMutex;
    std::condition_variable outstandingCondition;
    std::map<uint32_t, std::shared_ptr<GenerationState>> generations;
    std::shared_ptr<GenerationState> current;
    uint32_t generationCounter = 0;
    std::atomic<uint64_t> frameIdCounter{0};
    std::atomic<uint64_t> bitrateBps{0};
    std::atomic<bool> forceKeyframe{false};

    ~Impl() { TearDownProcess(true); }

    // ---------------------------------------------------------------- spawn

    bool SpawnAndHandshake()
    {
        const std::string helperPath = LocateHelperBinary();
        if (access(helperPath.c_str(), X_OK) != 0)
        {
            spdlog::error("NativeHelperEncoderTransport: helper binary not found at '{}'",
                          helperPath);
            return false;
        }

        // Parent checks the rendezvous name in BEFORE spawning (production
        // inversion of the probe's bootstrap_register; namespace stays intact
        // — the special-port route is XPC-poisoned, see EncoderMachSurface.h).
        const std::string rendezvousName = mach_surface::MakeRendezvousName();
        if (!rendezvous.CheckIn(rendezvousName))
        {
            spdlog::error("NativeHelperEncoderTransport: bootstrap_check_in('{}') failed",
                          rendezvousName);
            return false;
        }

        int fds[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        {
            spdlog::error("NativeHelperEncoderTransport: socketpair failed: {}", strerror(errno));
            return false;
        }
        ipc::EncoderIpcSocket::SetNoSigPipe(fds[0]);
        ipc::EncoderIpcSocket::SetNoSigPipe(fds[1]);
        // Only the parent end is CLOEXEC; the child end is inherited by number.
        ipc::EncoderIpcSocket::SetCloseOnExec(fds[0]);

        char fdString[16];
        snprintf(fdString, sizeof(fdString), "%d", fds[1]);
        const char* argv[] = {
            helperPath.c_str(), "--socket-fd", fdString,
            "--bootstrap-name", rendezvousName.c_str(), nullptr,
        };

        // Spawn hygiene: default signal dispositions, empty signal mask, own
        // process group (so Shutdown can SIGTERM the group), sanitized env.
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        sigset_t allSignals;
        sigfillset(&allSignals);
        posix_spawnattr_setsigdefault(&attr, &allSignals);
        sigset_t emptyMask;
        sigemptyset(&emptyMask);
        posix_spawnattr_setsigmask(&attr, &emptyMask);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
                                            POSIX_SPAWN_SETPGROUP);

        // DYLD_* injected into the Wine/game process must not leak into the
        // helper: it would change what the native child loads.
        std::vector<std::string> environmentStorage;
        std::vector<char*> environment;
        for (char** e = environ; e != nullptr && *e != nullptr; e++)
        {
            if (strncmp(*e, "DYLD_", 5) == 0)
            {
                continue;
            }
            environmentStorage.emplace_back(*e);
        }
        environment.reserve(environmentStorage.size() + 1);
        for (std::string& entry : environmentStorage)
        {
            environment.push_back(entry.data());
        }
        environment.push_back(nullptr);

        pid_t pid = -1;
        const int spawnResult = posix_spawn(&pid, helperPath.c_str(), nullptr, &attr,
                                            (char* const*)argv, environment.data());
        posix_spawnattr_destroy(&attr);
        close(fds[1]);
        if (spawnResult != 0)
        {
            spdlog::error("NativeHelperEncoderTransport: posix_spawn('{}') failed: {}", helperPath,
                          strerror(spawnResult));
            close(fds[0]);
            rendezvous.Close();
            return false;
        }
        childPid = pid;
        socket.Adopt(fds[0]);

        if (!rendezvous.WaitForChild(kHandshakeTimeoutMs))
        {
            spdlog::error("NativeHelperEncoderTransport: helper never checked in over Mach");
            TearDownProcess(true);
            return false;
        }

        // Synchronous handshake (reader thread not started yet).
        ipc::Hello hello;
        hello.parentPid = (uint32_t)getpid();
        std::vector<uint8_t> helloPayload;
        hello.Serialize(helloPayload);
        if (socket.WriteMessage(ipc::MessageType::Hello, 0, 0, helloPayload) != ipc::IoResult::Ok)
        {
            TearDownProcess(true);
            return false;
        }
        ipc::MessageHeader header;
        std::vector<uint8_t> payload;
        if (socket.ReadMessage(header, payload) != ipc::IoResult::Ok ||
            header.type != (uint16_t)ipc::MessageType::HelloReply ||
            !ipc::HelloReply::Deserialize(payload.data(), payload.size(), childCaps))
        {
            spdlog::error("NativeHelperEncoderTransport: handshake failed");
            TearDownProcess(true);
            return false;
        }

        if (childCaps.arch != ipc::kArchArm64 || childCaps.translated != 0)
        {
            spdlog::error("NativeHelperEncoderTransport: rejecting helper (arch={} translated={}) "
                          "— a translated child defeats the native-encode purpose",
                          childCaps.arch, childCaps.translated);
            TearDownProcess(true);
            return false;
        }
        // Per-codec HW/low-latency caps are NOT checked here: the caller reads
        // them via SupportsCodec() to pick the codec, and Configure() enforces
        // them for the codec it was finally given.
        spdlog::info("NativeHelperEncoderTransport: helper pid {} up (arm64 native, macOS {}, "
                     "capsH264=0x{:x} capsH265=0x{:x})",
                     childCaps.helperPid, childCaps.macosMajor, childCaps.capsH264,
                     childCaps.capsH265);

        alive.store(true);
        readerThread = std::thread([this] { ReaderLoop(); });
        writerThread = std::thread([this] { WriterLoop(); });
        spawned = true;
        return true;
    }

    // ---------------------------------------------------------------- writer

    void EnqueueControl(ipc::MessageType type, std::vector<uint8_t> payload)
    {
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            controlQueue.push_back({type, std::move(payload), false});
        }
        writeCondition.notify_one();
    }

    bool EnqueueFrame(std::vector<uint8_t> payload)
    {
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            if (frameQueue.size() >= kSubmitQueueDepth)
            {
                return false;
            }
            frameQueue.push_back({ipc::MessageType::FrameSubmit, std::move(payload), true});
        }
        writeCondition.notify_one();
        return true;
    }

    void WriterLoop()
    {
        uint64_t sequence = 0;
        for (;;)
        {
            QueuedMessage message;
            {
                std::unique_lock<std::mutex> lock(writeMutex);
                writeCondition.wait(lock, [this] {
                    return stopping.load() || !controlQueue.empty() || !frameQueue.empty();
                });
                if (!controlQueue.empty())
                {
                    message = std::move(controlQueue.front());
                    controlQueue.pop_front();
                }
                else if (!frameQueue.empty())
                {
                    message = std::move(frameQueue.front());
                    frameQueue.pop_front();
                }
                else
                {
                    return; // stopping and empty
                }
            }
            const ipc::IoResult result =
                socket.WriteMessage(message.type, 0, sequence++, message.payload);
            if (result == ipc::IoResult::Eof || result == ipc::IoResult::Error)
            {
                HandleLinkDown();
                // Keep draining queues so producers never block; writes are
                // no-ops once the fd is shut down.
            }
        }
    }

    // ---------------------------------------------------------------- reader

    void ReaderLoop()
    {
        for (;;)
        {
            ipc::MessageHeader header;
            std::vector<uint8_t> payload;
            const ipc::IoResult result = socket.ReadMessage(header, payload);
            if (result != ipc::IoResult::Ok)
            {
                if (result == ipc::IoResult::Invalid)
                {
                    spdlog::error("NativeHelperEncoderTransport: protocol violation from helper");
                }
                HandleLinkDown();
                return;
            }
            switch ((ipc::MessageType)header.type)
            {
                case ipc::MessageType::FrameResult:
                    OnFrameResult(payload);
                    break;
                case ipc::MessageType::FrameDropped:
                    OnFrameDropped(payload);
                    break;
                case ipc::MessageType::ConfigureAck:
                {
                    ipc::ConfigureAck ack;
                    if (ipc::ConfigureAck::Deserialize(payload.data(), payload.size(), ack))
                    {
                        std::lock_guard<std::mutex> lock(controlMutex);
                        configureAck = ack;
                        controlCondition.notify_all();
                    }
                    break;
                }
                case ipc::MessageType::DrainComplete:
                {
                    ipc::DrainComplete complete;
                    if (ipc::DrainComplete::Deserialize(payload.data(), payload.size(), complete))
                    {
                        std::lock_guard<std::mutex> lock(controlMutex);
                        drainCompleteGeneration = complete.generation;
                        controlCondition.notify_all();
                    }
                    break;
                }
                case ipc::MessageType::ShutdownAck:
                {
                    std::lock_guard<std::mutex> lock(controlMutex);
                    shutdownAckReceived = true;
                    controlCondition.notify_all();
                    break;
                }
                case ipc::MessageType::FatalError:
                {
                    ipc::FatalError fatal;
                    if (ipc::FatalError::Deserialize(payload.data(), payload.size(), fatal))
                    {
                        spdlog::error("NativeHelperEncoderTransport: helper fatal error {}: {}",
                                      fatal.code, fatal.message);
                    }
                    HandleLinkDown();
                    return;
                }
                default:
                    break; // additive minor-version tolerance
            }
        }
    }

    // Take a frame's terminal transition. Returns the entry when this call
    // owns the transition; the generation is dropped once idle and stale.
    std::optional<OutstandingFrame> TakeOutstanding(uint32_t generation, uint64_t frameId)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto generationIt = generations.find(generation);
        if (generationIt == generations.end())
        {
            return std::nullopt;
        }
        auto& state = *generationIt->second;
        auto frameIt = state.outstanding.find(frameId);
        if (frameIt == state.outstanding.end())
        {
            return std::nullopt;
        }
        OutstandingFrame frame = std::move(frameIt->second);
        state.outstanding.erase(frameIt);
        if (state.outstanding.empty() && generationIt->second != current)
        {
            generations.erase(generationIt);
        }
        outstandingCondition.notify_all();
        return frame;
    }

    static void FinalizeFrame(OutstandingFrame& frame, bool dropped)
    {
        frame.metrics.frameDropped = dropped;
        frame.metrics.totalLatencyMs = ToMs(Clock::now() - frame.begin);
        if (frame.callbacks.onFrameComplete)
        {
            frame.callbacks.onFrameComplete(frame.metrics);
        }
        if (frame.callbacks.releaseResources)
        {
            frame.callbacks.releaseResources();
        }
    }

    void OnFrameResult(const std::vector<uint8_t>& payload)
    {
        ipc::FrameResult wire;
        if (!ipc::FrameResult::Deserialize(payload.data(), payload.size(), wire))
        {
            spdlog::warn("NativeHelperEncoderTransport: invalid FrameResult dropped");
            return;
        }
        // Stale-generation results resolve THEIR generation's entry (or
        // nothing); they can never touch the current generation's slots.
        auto frame = TakeOutstanding(wire.generation, wire.frameId);
        if (!frame.has_value())
        {
            return;
        }
        frame->metrics.keyframe = (wire.flags & ipc::kFrameResultFlagIsIdr) != 0;
        frame->metrics.callbackLatencyMs =
            wire.callbackAtNs > wire.encodeStartNs
                ? (double)(wire.callbackAtNs - wire.encodeStartNs) / 1e6
                : 0.0;
        if (frame->callbacks.onEncodedFrame)
        {
            EncodedFrameResult result;
            result.data = wire.data.data();
            result.size = wire.data.size();
            result.isIdr = frame->metrics.keyframe;
            result.timestampNs = frame->metrics.timestampNs;
            result.nalUnits.reserve(wire.nalUnits.size());
            for (const ipc::NalDescriptor& nal : wire.nalUnits)
            {
                result.nalUnits.push_back({nal.offset, nal.length});
            }
            frame->callbacks.onEncodedFrame(result);
        }
        FinalizeFrame(*frame, false);
    }

    void OnFrameDropped(const std::vector<uint8_t>& payload)
    {
        ipc::FrameDropped wire;
        if (!ipc::FrameDropped::Deserialize(payload.data(), payload.size(), wire))
        {
            return;
        }
        auto frame = TakeOutstanding(wire.generation, wire.frameId);
        if (!frame.has_value())
        {
            return;
        }
        if (wire.reason != ipc::kDropEncoderReported)
        {
            spdlog::warn("NativeHelperEncoderTransport: helper dropped frame {} (reason={} "
                         "status={})",
                         wire.frameId, wire.reason, wire.status);
        }
        FinalizeFrame(*frame, true);
    }

    // ------------------------------------------------------------ link death

    void HandleLinkDown()
    {
        if (!alive.exchange(false))
        {
            return;
        }
        spdlog::error("NativeHelperEncoderTransport: helper link down (EOF/crash) — reclaiming "
                      "in-flight frames");
        ReclaimAllOutstanding();
        ReapChild(false);
        std::lock_guard<std::mutex> lock(controlMutex);
        controlCondition.notify_all();
    }

    void ReclaimAllOutstanding()
    {
        std::vector<OutstandingFrame> reclaimed;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            for (auto& [generation, state] : generations)
            {
                for (auto& [frameId, frame] : state->outstanding)
                {
                    reclaimed.push_back(std::move(frame));
                }
                state->outstanding.clear();
            }
            // Keep only the current generation object (idle); stale ones go.
            for (auto it = generations.begin(); it != generations.end();)
            {
                it = it->second == current ? std::next(it) : generations.erase(it);
            }
            outstandingCondition.notify_all();
        }
        for (OutstandingFrame& frame : reclaimed)
        {
            FinalizeFrame(frame, true);
        }
    }

    void ReapChild(bool force)
    {
        if (childPid <= 0)
        {
            return;
        }
        int status = 0;
        const pid_t reaped = waitpid(childPid, &status, WNOHANG);
        if (reaped == childPid)
        {
            childPid = -1;
            return;
        }
        if (!force)
        {
            return;
        }
        kill(-childPid, SIGKILL); // process group (POSIX_SPAWN_SETPGROUP pgid==pid)
        for (int i = 0; i < 100; i++)
        {
            if (waitpid(childPid, &status, WNOHANG) == childPid)
            {
                childPid = -1;
                return;
            }
            usleep(5000);
        }
        spdlog::warn("NativeHelperEncoderTransport: helper pid {} did not reap after SIGKILL",
                     childPid);
        childPid = -1; // give up; init will reap
    }

    // ------------------------------------------------------------- teardown

    void TearDownProcess(bool waitBounded)
    {
        stopping.store(true);
        alive.store(false);
        writeCondition.notify_all();
        socket.ShutdownBoth(); // unblocks the reader
        if (childPid > 0)
        {
            kill(-childPid, SIGTERM);
        }
        if (readerThread.joinable())
        {
            readerThread.join();
        }
        if (writerThread.joinable())
        {
            writerThread.join();
        }
        socket.Close();
        rendezvous.Close();
        ReclaimAllOutstanding();
        if (childPid > 0)
        {
            if (waitBounded)
            {
                int status = 0;
                bool reaped = false;
                for (uint32_t waited = 0; waited < kTermWaitMs; waited += 10)
                {
                    if (waitpid(childPid, &status, WNOHANG) == childPid)
                    {
                        reaped = true;
                        break;
                    }
                    usleep(10000);
                }
                if (!reaped)
                {
                    ReapChild(true); // escalate to SIGKILL
                }
                else
                {
                    childPid = -1;
                }
            }
            else
            {
                ReapChild(false);
            }
        }
        stopping.store(false);
        spawned = false;
    }
};

// ---------------------------------------------------------------------------
// Pending-frame token.
// ---------------------------------------------------------------------------

namespace
{

class HelperPendingFrame final : public IPendingEncodeFrame
{
public:
    HelperPendingFrame(std::shared_ptr<NativeHelperEncoderTransport::Impl> impl,
                       std::shared_ptr<GenerationState> generation, FrameCallbacks callbacks,
                       const EncodedFrameMetrics& seedMetrics)
        : impl_(std::move(impl)), generation_(std::move(generation))
    {
        frame_.callbacks = std::move(callbacks);
        frame_.metrics = seedMetrics;
        frame_.begin = Clock::now();
    }

    ~HelperPendingFrame() override
    {
        // Exactly-once safety net for abandoned tokens.
        if (!consumed_)
        {
            NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
        }
    }

    bool Submit(void* composedSlotBuffer, int64_t timestampNs, bool forceKeyframe) override
    {
        if (consumed_)
        {
            return false;
        }
        consumed_ = true;
        auto& impl = *impl_;
        if (!impl.alive.load() || composedSlotBuffer == nullptr)
        {
            NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
            return false;
        }

        // Resolve (and lazily register) the compose slot for this IOSurface.
        CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)composedSlotBuffer;
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
        if (surface == nullptr)
        {
            spdlog::warn("NativeHelperEncoderTransport: composed buffer has no IOSurface");
            NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
            return false;
        }
        const uint32_t surfaceId = IOSurfaceGetID(surface);
        uint32_t slot = 0;
        bool needsRegistration = false;
        {
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            auto it = generation_->surfaceSlots.find(surfaceId);
            if (it != generation_->surfaceSlots.end())
            {
                slot = it->second;
            }
            else if (generation_->surfaceSlots.size() < ipc::kSlotCount)
            {
                slot = (uint32_t)generation_->surfaceSlots.size();
                generation_->surfaceSlots[surfaceId] = slot;
                needsRegistration = true;
            }
            else
            {
                slot = UINT32_MAX;
            }
        }
        if (slot == UINT32_MAX)
        {
            spdlog::warn("NativeHelperEncoderTransport: more than {} distinct compose surfaces "
                         "in one generation",
                         ipc::kSlotCount);
            NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
            return false;
        }
        if (needsRegistration)
        {
            mach_surface::SurfaceTag tag;
            tag.generation = generation_->generation;
            tag.slot = slot;
            tag.width = (uint32_t)IOSurfaceGetWidth(surface);
            tag.height = (uint32_t)IOSurfaceGetHeight(surface);
            tag.pixelFormat = (uint32_t)IOSurfaceGetPixelFormat(surface);
            if (!impl.rendezvous.SendSurface(surface, tag))
            {
                spdlog::error("NativeHelperEncoderTransport: surface transfer failed "
                              "(generation {} slot {})",
                              tag.generation, slot);
                {
                    std::lock_guard<std::mutex> lock(impl.stateMutex);
                    generation_->surfaceSlots.erase(surfaceId);
                }
                NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
                return false;
            }
        }

        const uint64_t frameId = impl.frameIdCounter.fetch_add(1);
        frame_.metrics.timestampNs = timestampNs;
        frame_.submitted = Clock::now();

        ipc::FrameSubmit submit;
        submit.generation = generation_->generation;
        submit.slot = slot;
        submit.frameId = frameId;
        submit.ptsNs = timestampNs;
        submit.ptsTimescale = 1000000000;
        submit.flags = (forceKeyframe || impl.forceKeyframe.exchange(false))
                           ? ipc::kFrameFlagForceIdr
                           : 0;
        submit.bitrateBps = impl.bitrateBps.load();
        submit.composedAtNs = NowNs();
        submit.submittedAtNs = submit.composedAtNs;
        std::vector<uint8_t> payload;
        submit.Serialize(payload);

        // Register the outstanding entry BEFORE the enqueue: the reader may
        // deliver the result immediately after the writer sends it.
        {
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            generation_->outstanding.emplace(frameId, std::move(frame_));
        }
        if (!impl.EnqueueFrame(std::move(payload)))
        {
            // Bounded queue full: exactly-once via TakeOutstanding.
            auto reclaimed = impl.TakeOutstanding(generation_->generation, frameId);
            if (reclaimed.has_value())
            {
                NativeHelperEncoderTransport::Impl::FinalizeFrame(*reclaimed, true);
            }
            if ((submit.flags & ipc::kFrameFlagForceIdr) != 0)
            {
                impl.forceKeyframe.store(true); // re-arm the swallowed request
            }
            return false;
        }
        return true;
    }

    void Cancel() override
    {
        if (!consumed_)
        {
            consumed_ = true;
            NativeHelperEncoderTransport::Impl::FinalizeFrame(frame_, true);
        }
    }

private:
    std::shared_ptr<NativeHelperEncoderTransport::Impl> impl_;
    std::shared_ptr<GenerationState> generation_;
    OutstandingFrame frame_;
    bool consumed_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Transport facade.
// ---------------------------------------------------------------------------

NativeHelperEncoderTransport::NativeHelperEncoderTransport() : impl_(std::make_shared<Impl>()) {}

NativeHelperEncoderTransport::~NativeHelperEncoderTransport()
{
    Shutdown();
}

bool NativeHelperEncoderTransport::HelperBinaryAvailable()
{
    const std::string helperPath = LocateHelperBinary();
    return access(helperPath.c_str(), X_OK) == 0;
}

bool NativeHelperEncoderTransport::StartHelper()
{
    auto& impl = *impl_;
    if (impl.spawned)
    {
        return impl.alive.load(); // a died helper makes this transport inert
    }
    return impl.SpawnAndHandshake();
}

bool NativeHelperEncoderTransport::SupportsCodec(oxr::protocol::VideoCodec codec) const
{
    auto& impl = *impl_;
    if (!impl.spawned)
    {
        return false;
    }
    const uint32_t requiredCaps = ipc::kCodecCapHardware | ipc::kCodecCapLowLatency;
    const uint32_t codecCaps = codec == oxr::protocol::VideoCodec::H264 ? impl.childCaps.capsH264
                                                                        : impl.childCaps.capsH265;
    return (codecCaps & requiredCaps) == requiredCaps;
}

bool NativeHelperEncoderTransport::Configure(const EncoderConfig& config)
{
    auto& impl = *impl_;
    if (!impl.spawned && !impl.SpawnAndHandshake())
    {
        return false;
    }
    if (!SupportsCodec(config.codec))
    {
        // The whole point of the helper is native hardware low-latency encode;
        // callers gate the codec on SupportsCodec() before configuring.
        spdlog::error("NativeHelperEncoderTransport: helper lacks HW low-latency support for the "
                      "requested codec (capsH264=0x{:x} capsH265=0x{:x})",
                      impl.childCaps.capsH264, impl.childCaps.capsH265);
        return false;
    }

    const uint32_t generation = ++impl.generationCounter;
    ipc::ConfigureGeneration configure;
    configure.generation = generation;
    configure.width = config.width;
    configure.height = config.height;
    configure.codec =
        config.codec == oxr::protocol::VideoCodec::H264 ? ipc::kCodecH264 : ipc::kCodecH265;
    configure.bitDepth = config.tenBit ? 10 : 8;
    configure.pixelFormat = 'BGRA';
    configure.fps = config.fps;
    configure.keyframeIntervalSec = config.keyframeIntervalSec;
    configure.initialBitrateBps = (uint64_t)config.bitrateMbps * 1000000;
    configure.slotCount = ipc::kSlotCount;
    std::vector<uint8_t> payload;
    configure.Serialize(payload);

    {
        std::lock_guard<std::mutex> lock(impl.controlMutex);
        impl.configureAck.reset();
    }
    impl.EnqueueControl(ipc::MessageType::ConfigureGeneration, std::move(payload));

    std::unique_lock<std::mutex> lock(impl.controlMutex);
    const bool acked = impl.controlCondition.wait_for(
        lock, std::chrono::milliseconds(kConfigureTimeoutMs), [&] {
            return (impl.configureAck.has_value() &&
                    impl.configureAck->generation == generation) ||
                   !impl.alive.load();
        });
    if (!acked || !impl.configureAck.has_value() || impl.configureAck->status != 0 ||
        !impl.alive.load())
    {
        spdlog::error("NativeHelperEncoderTransport: generation {} configure failed", generation);
        return false;
    }
    lock.unlock();

    auto state = std::make_shared<GenerationState>();
    state->generation = generation;
    state->config = config;
    {
        std::lock_guard<std::mutex> stateLock(impl.stateMutex);
        impl.generations[generation] = state;
        impl.current = state;
        // Drop retired idle generations eagerly.
        for (auto it = impl.generations.begin(); it != impl.generations.end();)
        {
            const bool retired = it->second != state && it->second->outstanding.empty();
            it = retired ? impl.generations.erase(it) : std::next(it);
        }
    }
    impl.bitrateBps.store((uint64_t)config.bitrateMbps * 1000000);
    return true;
}

std::shared_ptr<IPendingEncodeFrame> NativeHelperEncoderTransport::BeginFrame(
    FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics)
{
    auto& impl = *impl_;
    std::shared_ptr<GenerationState> generation;
    {
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        generation = impl.current;
    }
    if (generation == nullptr || !impl.alive.load())
    {
        return nullptr;
    }
    return std::make_shared<HelperPendingFrame>(impl_, std::move(generation),
                                                std::move(callbacks), seedMetrics);
}

void NativeHelperEncoderTransport::ForceKeyframe()
{
    impl_->forceKeyframe.store(true);
}

bool NativeHelperEncoderTransport::SetBitrate(uint32_t bitrateMbps)
{
    // The value rides on every FrameSubmit (authoritative, delta-applied in
    // the helper), so a dropped message can never lose the update.
    impl_->bitrateBps.store((uint64_t)bitrateMbps * 1000000);
    return impl_->alive.load();
}

void NativeHelperEncoderTransport::Drain()
{
    auto& impl = *impl_;
    if (!impl.alive.load())
    {
        return;
    }
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        generation = impl.current != nullptr ? impl.current->generation : 0;
    }
    ipc::Drain drain;
    drain.generation = generation;
    std::vector<uint8_t> payload;
    drain.Serialize(payload);
    {
        std::lock_guard<std::mutex> lock(impl.controlMutex);
        impl.drainCompleteGeneration.reset();
    }
    impl.EnqueueControl(ipc::MessageType::Drain, std::move(payload));

    const auto deadline = Clock::now() + std::chrono::milliseconds(kDrainBudgetMs);
    {
        std::unique_lock<std::mutex> lock(impl.controlMutex);
        impl.controlCondition.wait_until(lock, deadline, [&] {
            return (impl.drainCompleteGeneration.has_value() &&
                    *impl.drainCompleteGeneration == generation) ||
                   !impl.alive.load();
        });
    }
    // Consume any results that were flushed by the drain, within the budget.
    std::unique_lock<std::mutex> lock(impl.stateMutex);
    impl.outstandingCondition.wait_until(lock, deadline, [&] {
        return impl.current == nullptr || impl.current->outstanding.empty() ||
               !impl.alive.load();
    });
}

void NativeHelperEncoderTransport::Shutdown()
{
    auto& impl = *impl_;
    if (impl.spawned && impl.alive.load())
    {
        Drain();
        {
            std::lock_guard<std::mutex> lock(impl.controlMutex);
            impl.shutdownAckReceived = false;
        }
        impl.EnqueueControl(ipc::MessageType::Shutdown, {});
        std::unique_lock<std::mutex> lock(impl.controlMutex);
        impl.controlCondition.wait_for(lock, std::chrono::milliseconds(kShutdownAckTimeoutMs),
                                       [&] {
                                           return impl.shutdownAckReceived || !impl.alive.load();
                                       });
    }
    if (impl.spawned || impl.childPid > 0)
    {
        impl.TearDownProcess(true);
    }
    std::lock_guard<std::mutex> lock(impl.stateMutex);
    impl.current.reset();
    impl.generations.clear();
}

void NativeHelperEncoderTransport::StopForProcessExit()
{
    auto& impl = *impl_;
    impl.alive.store(false);
    impl.stopping.store(true);
    impl.writeCondition.notify_all();
    impl.socket.ShutdownBoth();
    impl.socket.Close();
    if (impl.childPid > 0)
    {
        kill(-impl.childPid, SIGTERM);
        int status = 0;
        waitpid(impl.childPid, &status, WNOHANG); // best-effort reap, no waits
    }
}

bool NativeHelperEncoderTransport::IsHealthy() const
{
    return impl_->alive.load();
}

} // namespace oxrsys::encoder
