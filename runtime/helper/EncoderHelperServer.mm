// SPDX-License-Identifier: MPL-2.0

#import "EncoderHelperServer.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#import <spdlog/spdlog.h>

#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/encoder/EncoderIpcProtocol.h"
#include "../src/encoder/EncoderIpcSocket.h"
#include "../src/encoder/EncoderMachSurface.h"
#include "../src/encoder/EncoderTransport.h"
#include "../src/encoder/VideoToolboxEncodeEngine.h"

namespace oxrsys::encoder::helper
{

namespace ipc = oxrsys::encoder::ipc;
namespace mach_surface = oxrsys::encoder::mach_surface;

namespace
{

// Wall nanoseconds from the shared host counter. Gate B rule: raw mach ticks
// are NOT comparable across the Rosetta boundary; only normalized ns cross
// the wire.
uint64_t NowNs()
{
    static mach_timebase_info_data_t timebase = {};
    if (timebase.denom == 0)
    {
        mach_timebase_info(&timebase);
    }
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

bool RunningTranslated()
{
    int translated = 0;
    size_t size = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) != 0)
    {
        return false;
    }
    return translated == 1;
}

uint32_t CurrentArch()
{
#if defined(__arm64__)
    return ipc::kArchArm64;
#elif defined(__x86_64__)
    return ipc::kArchX86_64;
#else
    return ipc::kArchUnknown;
#endif
}

// Trial-create capability probe: RequireHardware + LL-RC first, then
// RequireHardware alone. Session create success is the ONLY trustworthy
// hardware signal (the UsingHardware query fails -12900 on arm64 LL-RC
// sessions, and property rejections like PrioritizeEncodingSpeedOverQuality
// -12900 are tolerated by the engine at session-config time).
uint32_t ProbeCodecCaps(CMVideoCodecType codecType)
{
    const auto tryCreate = [codecType](bool lowLatency) -> bool
    {
        NSMutableDictionary* spec = [@{
            (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
            (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES,
        } mutableCopy];
        if (lowLatency)
        {
            spec[(NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
        }
        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault, 1280, 720, codecType, (__bridge CFDictionaryRef)spec, nullptr,
            kCFAllocatorDefault, nullptr, nullptr, &session);
        if (status != noErr || session == nullptr)
        {
            return false;
        }
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return true;
    };

    uint32_t caps = 0;
    if (tryCreate(true))
    {
        caps = ipc::kCodecCapHardware | ipc::kCodecCapLowLatency;
    }
    else if (tryCreate(false))
    {
        caps = ipc::kCodecCapHardware;
    }
    return caps;
}

uint32_t MacOSMajor()
{
    const NSOperatingSystemVersion version =
        NSProcessInfo.processInfo.operatingSystemVersion;
    return (uint32_t)version.majorVersion;
}

uint32_t NalTypeOf(uint32_t codec, const uint8_t* nalStart, size_t size)
{
    // nalStart points at the 4-byte Annex-B start code.
    if (size < 5)
    {
        return 0;
    }
    const uint8_t byte = nalStart[4];
    return codec == ipc::kCodecH264 ? (byte & 0x1F) : ((byte >> 1) & 0x3F);
}

struct OutMessage
{
    ipc::MessageType type;
    std::vector<uint8_t> payload;
    bool bounded = false; ///< subject to the bounded-queue cap (frame payloads)
};

/// Serialized-message writer with a bounded lane for payload-bearing results.
/// Control messages (acks, drops, errors) always enqueue — they are tiny and
/// terminal-transition-critical.
class ResultWriter
{
public:
    static constexpr size_t kBoundedCapacity = 16;

    void Start(ipc::EncoderIpcSocket* socket)
    {
        socket_ = socket;
        thread_ = std::thread(
            [this]
            {
                Loop();
            });
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    /// Returns false only for bounded messages when the lane is full.
    bool Enqueue(OutMessage message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
            {
                return false;
            }
            if (message.bounded && boundedDepth_ >= kBoundedCapacity)
            {
                return false;
            }
            if (message.bounded)
            {
                boundedDepth_++;
            }
            queue_.push_back(std::move(message));
        }
        condition_.notify_one();
        return true;
    }

    /// Block (bounded) until the queue is empty — used before DrainComplete.
    void Flush(uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idleCondition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this] { return queue_.empty() || stopping_; });
    }

private:
    void Loop()
    {
        uint64_t sequence = 0;
        for (;;)
        {
            OutMessage message;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                {
                    idleCondition_.notify_all();
                    if (stopping_)
                    {
                        return;
                    }
                    continue;
                }
                message = std::move(queue_.front());
                queue_.pop_front();
                if (message.bounded)
                {
                    boundedDepth_--;
                }
                if (queue_.empty())
                {
                    idleCondition_.notify_all();
                }
            }
            socket_->WriteMessage(message.type, 0, sequence++, message.payload);
        }
    }

    ipc::EncoderIpcSocket* socket_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::deque<OutMessage> queue_;
    size_t boundedDepth_ = 0;
    bool stopping_ = false;
};

/// One registered, wrapped slot buffer (wrap-once, reuse forever — Gate B2).
struct SlotBuffer
{
    IOSurfaceRef surface = nullptr;
    CVPixelBufferRef pixelBuffer = nullptr;

    ~SlotBuffer()
    {
        if (pixelBuffer != nullptr)
        {
            CVPixelBufferRelease(pixelBuffer);
        }
        if (surface != nullptr)
        {
            CFRelease(surface);
        }
    }
};

class ServerState
{
public:
    ipc::EncoderIpcSocket socket;
    mach_surface::ChildSurfaceRendezvous rendezvous;
    VideoToolboxEncodeEngine engine;
    ResultWriter writer;

    std::atomic<bool> shuttingDown{false};

    // Registered slot buffers keyed by (generation << 32) | slot; entries for
    // retired generations are dropped on ConfigureGeneration. Guarded by
    // slotMutex; slotCondition wakes frame handlers waiting for a surface
    // that is still in flight on the mach channel (the mach and socket
    // channels have no cross-ordering guarantee).
    std::mutex slotMutex;
    std::condition_variable slotCondition;
    std::map<uint64_t, std::shared_ptr<SlotBuffer>> slots;

    uint32_t generation = 0;
    bool configured = false;
    uint32_t wireCodec = ipc::kCodecH265;
    uint64_t appliedBitrateBps = 0;

    std::thread machThread;

    static uint64_t SlotKey(uint32_t generation, uint32_t slot)
    {
        return ((uint64_t)generation << 32) | slot;
    }

    void SendControl(ipc::MessageType type, std::vector<uint8_t> payload)
    {
        OutMessage message;
        message.type = type;
        message.payload = std::move(payload);
        message.bounded = false;
        writer.Enqueue(std::move(message));
    }

    void SendFrameDropped(uint32_t frameGeneration, uint64_t frameId, uint32_t reason,
                          int32_t status)
    {
        ipc::FrameDropped dropped;
        dropped.generation = frameGeneration;
        dropped.frameId = frameId;
        dropped.reason = reason;
        dropped.status = status;
        std::vector<uint8_t> payload;
        dropped.Serialize(payload);
        SendControl(ipc::MessageType::FrameDropped, std::move(payload));
    }
};

// ---------------------------------------------------------------------------
// Mach surface receiver: wraps each surface ONCE with BT.709 ShouldPropagate
// attachments and publishes it under (generation, slot).
// ---------------------------------------------------------------------------

void MachReceiveLoop(ServerState& state)
{
    while (!state.shuttingDown.load())
    {
        void* rawSurface = nullptr;
        mach_surface::SurfaceTag tag;
        if (!state.rendezvous.ReceiveSurface(&rawSurface, tag, 250 /*ms*/))
        {
            continue; // timeout or invalid message; invalid rights are freed inside
        }
        IOSurfaceRef surface = (IOSurfaceRef)rawSurface;

        CVPixelBufferRef pixelBuffer = nullptr;
        const CVReturn cvStatus =
            CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &pixelBuffer);
        if (cvStatus != kCVReturnSuccess || pixelBuffer == nullptr)
        {
            spdlog::error("encoder-helper: CVPixelBufferCreateWithIOSurface failed ({}) for "
                          "generation {} slot {}",
                          (int)cvStatus, tag.generation, tag.slot);
            CFRelease(surface);
            continue;
        }
        // Re-apply the BT.709 color contract on the wrapped buffer: attachments
        // do not travel with the IOSurface across the process boundary.
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferTransferFunction_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);

        auto slotBuffer = std::make_shared<SlotBuffer>();
        slotBuffer->surface = surface;      // ReceiveSurface's +1 CF ref moves here
        slotBuffer->pixelBuffer = pixelBuffer;
        {
            std::lock_guard<std::mutex> lock(state.slotMutex);
            state.slots[ServerState::SlotKey(tag.generation, tag.slot)] = std::move(slotBuffer);
        }
        state.slotCondition.notify_all();
        spdlog::info("encoder-helper: registered surface generation {} slot {} ({}x{})",
                     tag.generation, tag.slot, tag.width, tag.height);
    }
}

// ---------------------------------------------------------------------------
// Message handlers.
// ---------------------------------------------------------------------------

void HandleConfigure(ServerState& state, const std::vector<uint8_t>& payload)
{
    ipc::ConfigureGeneration configure;
    ipc::ConfigureAck ack;
    if (!ipc::ConfigureGeneration::Deserialize(payload.data(), payload.size(), configure))
    {
        ack.status = (uint32_t)-1;
        std::vector<uint8_t> out;
        ack.Serialize(out);
        state.SendControl(ipc::MessageType::ConfigureAck, std::move(out));
        return;
    }

    // Retire every slot of older generations; their wrapped buffers and
    // surface refs drop here (in-flight engine frames were drained by the
    // parent's Drain-before-reconfigure contract).
    state.engine.Drain();
    {
        std::lock_guard<std::mutex> lock(state.slotMutex);
        for (auto it = state.slots.begin(); it != state.slots.end();)
        {
            if ((uint32_t)(it->first >> 32) < configure.generation)
            {
                it = state.slots.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    EncoderConfig config;
    config.width = configure.width;
    config.height = configure.height;
    config.fps = configure.fps;
    config.bitrateMbps =
        (uint32_t)std::max<uint64_t>(1, (configure.initialBitrateBps + 500000) / 1000000);
    config.codec = configure.codec == ipc::kCodecH264 ? oxr::protocol::VideoCodec::H264
                                                      : oxr::protocol::VideoCodec::H265;
    config.tenBit = configure.bitDepth == 10;
    config.keyframeIntervalSec = configure.keyframeIntervalSec;

    const bool created = state.engine.CreateSession(config);
    state.generation = configure.generation;
    state.configured = created;
    state.wireCodec = configure.codec;
    state.appliedBitrateBps = configure.initialBitrateBps;

    ack.generation = configure.generation;
    ack.status = created ? 0 : (uint32_t)-2;
    std::vector<uint8_t> out;
    ack.Serialize(out);
    state.SendControl(ipc::MessageType::ConfigureAck, std::move(out));
    spdlog::info("encoder-helper: generation {} configured {}x{} codec={} -> {}",
                 configure.generation, configure.width, configure.height, configure.codec,
                 created ? "ok" : "FAILED");
}

void HandleFrameSubmit(ServerState& state, const std::vector<uint8_t>& payload)
{
    ipc::FrameSubmit submit;
    if (!ipc::FrameSubmit::Deserialize(payload.data(), payload.size(), submit))
    {
        return; // unparseable: no frame identity to answer for
    }
    if (!state.configured || submit.generation != state.generation)
    {
        state.SendFrameDropped(submit.generation, submit.frameId, ipc::kDropBadGeneration, 0);
        return;
    }

    // Resolve the slot buffer; the surface may still be in flight on the mach
    // channel (no cross-channel ordering), so wait bounded.
    std::shared_ptr<SlotBuffer> slotBuffer;
    {
        std::unique_lock<std::mutex> lock(state.slotMutex);
        const uint64_t key = ServerState::SlotKey(submit.generation, submit.slot);
        state.slotCondition.wait_for(lock, std::chrono::milliseconds(500),
                                     [&] { return state.slots.count(key) != 0; });
        auto it = state.slots.find(key);
        if (it != state.slots.end())
        {
            slotBuffer = it->second;
        }
    }
    if (slotBuffer == nullptr)
    {
        state.SendFrameDropped(submit.generation, submit.frameId, ipc::kDropUnknownSlot, 0);
        return;
    }

    // Authoritative bitrate on EVERY frame: apply only the delta.
    if (submit.bitrateBps != 0 && submit.bitrateBps != state.appliedBitrateBps)
    {
        const uint32_t mbps =
            (uint32_t)std::max<uint64_t>(1, (submit.bitrateBps + 500000) / 1000000);
        if (state.engine.SetBitrate(mbps))
        {
            state.appliedBitrateBps = submit.bitrateBps;
        }
    }

    const uint32_t frameGeneration = submit.generation;
    const uint64_t frameId = submit.frameId;
    const uint32_t wireCodec = state.wireCodec;
    const uint64_t encodeStartNs = NowNs();
    auto resultSent = std::make_shared<std::atomic<bool>>(false);
    ServerState* statePtr = &state;

    FrameCallbacks callbacks;
    callbacks.onEncodedFrame = [statePtr, frameGeneration, frameId, wireCodec, encodeStartNs,
                                resultSent](const EncodedFrameResult& result)
    {
        ipc::FrameResult wire;
        wire.generation = frameGeneration;
        wire.frameId = frameId;
        wire.status = 0;
        wire.flags = result.isIdr ? ipc::kFrameResultFlagIsIdr : 0;
        wire.encodeStartNs = encodeStartNs;
        wire.callbackAtNs = NowNs();
        if (result.size > ipc::kMaxFramePayloadBytes)
        {
            statePtr->SendFrameDropped(frameGeneration, frameId, ipc::kDropQueueFull, 0);
            resultSent->store(true);
            return;
        }
        wire.nalUnits.reserve(result.nalUnits.size());
        for (const NalUnitDescriptor& nal : result.nalUnits)
        {
            ipc::NalDescriptor descriptor;
            descriptor.offset = (uint32_t)nal.offset;
            descriptor.length = (uint32_t)nal.size;
            descriptor.type = NalTypeOf(wireCodec, result.data + nal.offset, nal.size);
            wire.nalUnits.push_back(descriptor);
        }
        wire.data.assign(result.data, result.data + result.size);

        OutMessage message;
        message.type = ipc::MessageType::FrameResult;
        message.bounded = true;
        wire.Serialize(message.payload);
        if (statePtr->writer.Enqueue(std::move(message)))
        {
            resultSent->store(true);
        }
        else
        {
            // Bounded lane full: convert to a (tiny, always-enqueued) drop so
            // the parent still sees exactly one terminal transition.
            statePtr->SendFrameDropped(frameGeneration, frameId, ipc::kDropQueueFull, 0);
            resultSent->store(true);
        }
    };
    callbacks.onFrameComplete = [statePtr, frameGeneration, frameId,
                                 resultSent](const EncodedFrameMetrics& metrics)
    {
        if (metrics.frameDropped && !resultSent->load())
        {
            statePtr->SendFrameDropped(frameGeneration, frameId, ipc::kDropEncoderReported, 0);
            resultSent->store(true);
        }
    };
    callbacks.releaseResources = [] {};

    EncodedFrameMetrics seedMetrics;
    seedMetrics.frameNumber = frameId;
    seedMetrics.timestampNs = submit.ptsNs;

    auto pendingFrame = state.engine.BeginFrame(std::move(callbacks), seedMetrics);
    if (pendingFrame == nullptr)
    {
        state.SendFrameDropped(frameGeneration, frameId, ipc::kDropShuttingDown, 0);
        return;
    }
    // ALL bookkeeping is done above — the LL-RC callback may fire before
    // Submit returns (engine enforces the same rule internally).
    const int64_t ptsNs = submit.ptsNs * (int64_t)(1000000000 / submit.ptsTimescale);
    const bool forceIdr = (submit.flags & ipc::kFrameFlagForceIdr) != 0;
    pendingFrame->Submit(slotBuffer->pixelBuffer, ptsNs, forceIdr);
    // Submit==false already finalized as dropped via onFrameComplete.
}

} // namespace

int EncoderHelperServer::Run(int socketFd, const std::string& bootstrapName)
{
    ServerState state;
    state.socket.Adopt(socketFd);
    ipc::EncoderIpcSocket::SetNoSigPipe(socketFd);

    if (!state.rendezvous.CheckIn(bootstrapName, 5000))
    {
        spdlog::error("encoder-helper: bootstrap rendezvous '{}' failed", bootstrapName);
        return 2;
    }

    state.writer.Start(&state.socket);
    state.machThread = std::thread(
        [&state]
        {
            MachReceiveLoop(state);
        });

    int exitCode = 0;
    for (;;)
    {
        ipc::MessageHeader header;
        std::vector<uint8_t> payload;
        const ipc::IoResult ioResult = state.socket.ReadMessage(header, payload);
        if (ioResult == ipc::IoResult::Eof)
        {
            // Parent is gone (crash or fast-path exit): immediate clean exit.
            spdlog::info("encoder-helper: socket EOF, exiting");
            break;
        }
        if (ioResult != ipc::IoResult::Ok)
        {
            spdlog::error("encoder-helper: socket read failed ({})", (int)ioResult);
            exitCode = 3;
            break;
        }

        switch ((ipc::MessageType)header.type)
        {
            case ipc::MessageType::Hello:
            {
                ipc::HelloReply reply;
                reply.arch = CurrentArch();
                reply.translated = RunningTranslated() ? 1 : 0;
                reply.capsH264 = ProbeCodecCaps(kCMVideoCodecType_H264);
                reply.capsH265 = ProbeCodecCaps(kCMVideoCodecType_HEVC);
                reply.macosMajor = MacOSMajor();
                reply.helperPid = (uint32_t)getpid();
                std::vector<uint8_t> out;
                reply.Serialize(out);
                state.SendControl(ipc::MessageType::HelloReply, std::move(out));
                break;
            }
            case ipc::MessageType::ConfigureGeneration:
                HandleConfigure(state, payload);
                break;
            case ipc::MessageType::FrameSubmit:
                HandleFrameSubmit(state, payload);
                break;
            case ipc::MessageType::Drain:
            {
                ipc::Drain drain;
                ipc::Drain::Deserialize(payload.data(), payload.size(), drain);
                state.engine.Drain();
                state.writer.Flush(200);
                ipc::DrainComplete complete;
                complete.generation = drain.generation;
                std::vector<uint8_t> out;
                complete.Serialize(out);
                state.SendControl(ipc::MessageType::DrainComplete, std::move(out));
                break;
            }
            case ipc::MessageType::Shutdown:
            {
                state.SendControl(ipc::MessageType::ShutdownAck, {});
                state.writer.Flush(200);
                goto out; // orderly: drain + teardown below
            }
            default:
                // Unknown-but-valid message types are skipped (payload already
                // consumed) — additive minor-version tolerance.
                break;
        }
    }
out:
    state.shuttingDown.store(true);
    state.engine.Drain();
    state.engine.DestroySession();
    if (state.machThread.joinable())
    {
        state.machThread.join();
    }
    state.writer.Stop();
    state.socket.Close();
    return exitCode;
}

} // namespace oxrsys::encoder::helper
