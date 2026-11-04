// SPDX-License-Identifier: MPL-2.0
//
// Offline end-to-end smoke test AND benchmark for the native-arm64 encoder
// helper.
//
// Mimics the exact runtime scenario WITHOUT Wine/the game: a parent process
// (build it x86_64 and it runs under Rosetta, just like the runtime dylib's
// host) creates IOSurface-backed BGRA buffers via a CVPixelBufferPool (as the
// runtime does), then encodes the same frames twice — once in-process, once via
// the spawned arm64 helper over the mach/socket handshake — and reports what
// each path cost. Those two numbers are the entire argument for routing the
// encode out of process, so the test measures both rather than asserting one.
//
// PASS criteria: the helper reports "InitAck hardware=YES" and returns NAL units
// for every frame. That proves surface sharing + IPC + hardware VT session
// across the x86_64->arm64 boundary — the whole mechanism.
//
// Build (parent as x86_64 to mimic the Rosetta host; build it arm64 to get the
// native-host numbers):
//   xcrun clang++ -arch x86_64 -std=c++17 -O2 runtime/encoder_helper/smoke_test.mm \
//     -o build/helper/smoke_test_x64 \
//     -framework Foundation -framework CoreFoundation -framework CoreVideo \
//     -framework CoreMedia -framework IOSurface -framework VideoToolbox
//   build/helper/smoke_test_x64 build/helper/oxrsys-encoder-helper [options]
//
// Options:
//   --codec hevc|h264     codec to encode (default: both, in turn)
//   --profile main|main10 HEVC profile (default: main)
//   --frames N            frames per path (default 40; the first 5 are warmup)
//   --skip-in-process     helper path only
//   --skip-helper         in-process path only

#define OXRSYS_ENC_IPC_WANT_MACH 1
#include "EncoderHelperIpc.h"
#include "EncoderSessionColor.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <servers/bootstrap.h>

#include <condition_variable>
#include <pthread/qos.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;
using namespace oxrsys::enc_ipc;

static const int kChildSocketFd = 3;
static const uint32_t kW = 2272, kH = 1264, kSlots = 3, kFps = 72, kMbps = 50;
static const int kWarmupFrames = 5;

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------
static double MachToMs(uint64_t start, uint64_t end)
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    long double ns = (long double)(end - start) * tb.numer / tb.denom;
    return (double)(ns / 1.0e6L);
}

struct Stats
{
    double sumMs = 0;
    double maxMs = 0;
    int count = 0;

    void Add(double ms)
    {
        sumMs += ms;
        maxMs = ms > maxMs ? ms : maxMs;
        ++count;
    }
    double Avg() const { return count > 0 ? sumMs / count : 0.0; }
};

static const char* CodecLabel(CodecCode codec, ProfileCode profile)
{
    if (codec == CodecCode::H264) return "H.264 Main";
    return profile == ProfileCode::Main10 ? "H.265 Main10" : "H.265 Main";
}

static CMVideoCodecType CodecType(CodecCode codec)
{
    return codec == CodecCode::H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
}

static CFStringRef ProfileLevel(CodecCode codec, ProfileCode profile)
{
    if (codec == CodecCode::H264) return kVTProfileLevel_H264_Main_AutoLevel;
    return profile == ProfileCode::Main10 ? kVTProfileLevel_HEVC_Main10_AutoLevel
                                          : kVTProfileLevel_HEVC_Main_AutoLevel;
}

// Fresh, non-degenerate content (a moving gradient) so the encoder does
// representative work rather than re-emitting an identical frame.
static void PaintSurface(IOSurfaceRef surface, int frame)
{
    IOSurfaceLock(surface, 0, nullptr);
    uint8_t* base = (uint8_t*)IOSurfaceGetBaseAddress(surface);
    const size_t bpr = IOSurfaceGetBytesPerRow(surface);
    for (uint32_t y = 0; y < kH; ++y)
    {
        memset(base + y * bpr, (uint8_t)((frame * 7 + y) & 0xFF), bpr);
    }
    IOSurfaceUnlock(surface, 0, nullptr);
}

static bool ReadAll(int fd, uint8_t* d, size_t n) { return RecvAll(fd, d, n); }
static bool Send(int fd, MsgType t, const std::vector<uint8_t>& p)
{
    auto f = Frame(t, p);
    return SendAll(fd, f.data(), f.size()); // EPIPE, not SIGPIPE, if the helper died
}
static bool Recv(int fd, MsgType& t, std::vector<uint8_t>& p)
{
    uint8_t h[kHeaderBytes];
    if (!ReadAll(fd, h, kHeaderBytes)) return false;
    const FrameHeader parsed = ParseHeader(h, kHeaderBytes);
    if (!parsed.ok)
    {
        fprintf(stderr, "[smoke] bad frame header (version=%u) — helper/protocol mismatch?\n",
                parsed.version);
        return false;
    }
    t = parsed.type;
    p.resize(parsed.payloadLength);
    return parsed.payloadLength == 0 || ReadAll(fd, p.data(), parsed.payloadLength);
}

// ---------------------------------------------------------------------------
// Surfaces: one IOSurface-backed BGRA pool, shared by both paths so they encode
// byte-identical content.
// ---------------------------------------------------------------------------
struct SurfaceSet
{
    CVPixelBufferPoolRef pool = nullptr;
    CVPixelBufferRef pixelBuffers[kSlots] = {};
    IOSurfaceRef surfaces[kSlots] = {};
};

static bool CreateSurfaces(SurfaceSet& out)
{
    NSDictionary* attrs = @{
        (NSString*)kCVPixelBufferWidthKey : @(kW),
        (NSString*)kCVPixelBufferHeightKey : @(kH),
        (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs,
                                &out.pool) != kCVReturnSuccess)
    {
        fprintf(stderr, "pool create failed\n");
        return false;
    }
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, out.pool,
                                               &out.pixelBuffers[i]) != kCVReturnSuccess)
        {
            fprintf(stderr, "pb create %u failed\n", i);
            return false;
        }
        out.surfaces[i] = CVPixelBufferGetIOSurface(out.pixelBuffers[i]);
        PaintSurface(out.surfaces[i], (int)i);
    }
    printf("[smoke] created %u IOSurface-backed %ux%u BGRA buffers\n", kSlots, kW, kH);
    return true;
}

static void DestroySurfaces(SurfaceSet& set)
{
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        if (set.pixelBuffers[i]) CVPixelBufferRelease(set.pixelBuffers[i]);
    }
    if (set.pool) CVPixelBufferPoolRelease(set.pool);
}

// ---------------------------------------------------------------------------
// Path 1: in-process encode, in whatever architecture this binary was built for.
// This is the baseline the helper has to beat.
// ---------------------------------------------------------------------------
struct InProcessFrame
{
    uint64_t submitTicks = 0;
    double encodeMs = 0;
    bool done = false;
    bool dropped = false;
    size_t bytes = 0;
    std::mutex mutex;
    std::condition_variable cv;
};

static void InProcessCallback(void* /*outputRefCon*/, void* sourceFrameRefCon, OSStatus status,
                              VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer)
{
    // Match the helper's callback QoS exactly, or the comparison measures
    // thread priority rather than encoders.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    InProcessFrame* frame = static_cast<InProcessFrame*>(sourceFrameRefCon);
    if (frame == nullptr) return;
    const double ms = MachToMs(frame->submitTicks, mach_absolute_time());
    size_t bytes = 0;
    if (sampleBuffer != nullptr)
    {
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
        if (block != nullptr) bytes = CMBlockBufferGetDataLength(block);
    }
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        frame->encodeMs = ms;
        frame->bytes = bytes;
        frame->dropped = (status != noErr) || sampleBuffer == nullptr ||
                         (infoFlags & kVTEncodeInfo_FrameDropped) != 0;
        frame->done = true;
    }
    frame->cv.notify_one();
}

static bool RunInProcess(SurfaceSet& surfaces, CodecCode codec, ProfileCode profile, int frames,
                         Stats& stats, bool& usedHardware)
{
    // RequireHardware=NO deliberately: this reproduces exactly what the runtime
    // used to do unconditionally, so the number below is the cost of the silent
    // software fallback when there is one.
    NSDictionary* spec = @{
        (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES,
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @NO,
    };
    VTCompressionSessionRef session = nullptr;
    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, kW, kH, CodecType(codec),
                                             (__bridge CFDictionaryRef)spec, nullptr,
                                             kCFAllocatorDefault, InProcessCallback, nullptr,
                                             &session);
    if (st != noErr || session == nullptr)
    {
        fprintf(stderr, "[smoke] in-process VTCompressionSessionCreate failed: %d\n", (int)st);
        return false;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    // Same colour contract as the runtime and the helper, so the comparison
    // stays property-for-property.
    oxrsys::encoder_color::ApplySessionColorProperties(session);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                         ProfileLevel(codec, profile));
    int32_t bitrate = (int32_t)(kMbps * 1000000u);
    CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitrate);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);
    const double peakBytesPerSecond = (double)(kMbps * 1000000u) * 1.5 / 8.0;
    NSArray* limits = @[ @(peakBytesPerSecond), @(1.0) ];
    VTSessionSetProperty(session, kVTCompressionPropertyKey_DataRateLimits,
                         (__bridge CFArrayRef)limits);
    double keyDuration = 2.0;
    CFNumberRef keyDurationRef =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &keyDuration);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                         keyDurationRef);
    CFRelease(keyDurationRef);
    int32_t keyInterval = (int32_t)(2 * kFps);
    CFNumberRef keyRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &keyInterval);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyRef);
    CFRelease(keyRef);
    int32_t expectedFps = (int32_t)kFps;
    CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &expectedFps);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);
    int32_t maxDelay = 0;
    CFNumberRef delayRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &maxDelay);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, delayRef);
    CFRelease(delayRef);
    VTCompressionSessionPrepareToEncodeFrames(session);

    usedHardware = false;
    CFBooleanRef hwRef = nullptr;
    if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                              kCFAllocatorDefault, &hwRef) == noErr && hwRef != nullptr)
    {
        usedHardware = CFBooleanGetValue(hwRef);
        CFRelease(hwRef);
    }
    printf("[smoke] in-process %s session: hardware=%s\n", CodecLabel(codec, profile),
           usedHardware ? "YES" : "NO");

    size_t totalBytes = 0;
    for (int f = 0; f < frames; ++f)
    {
        const uint32_t slot = (uint32_t)(f % kSlots);
        PaintSurface(surfaces.surfaces[slot], f);

        InProcessFrame frame;
        CFMutableDictionaryRef frameProps = nullptr;
        if (f == 0)
        {
            frameProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(frameProps, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        }
        frame.submitTicks = mach_absolute_time();
        const CMTime pts = CMTimeMake((int64_t)f * 13888889, 1000000000);
        st = VTCompressionSessionEncodeFrame(session, surfaces.pixelBuffers[slot], pts,
                                             kCMTimeInvalid, frameProps, &frame, nullptr);
        if (frameProps != nullptr) CFRelease(frameProps);
        if (st != noErr)
        {
            fprintf(stderr, "[smoke] in-process encode frame %d failed: %d\n", f, (int)st);
            break;
        }
        // One frame in flight at a time, matching the helper measurement below
        // and the runtime's slot-gated submission.
        std::unique_lock<std::mutex> lock(frame.mutex);
        frame.cv.wait(lock, [&frame] { return frame.done; });
        totalBytes += frame.bytes;
        if (f >= kWarmupFrames && !frame.dropped) stats.Add(frame.encodeMs);
        lock.unlock();
        usleep(2000);
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    printf("[smoke] in-process %s: avg=%.2fms max=%.2fms over %d frames (%zu bytes)\n",
           CodecLabel(codec, profile), stats.Avg(), stats.maxMs, stats.count, totalBytes);
    return true;
}

// ---------------------------------------------------------------------------
// Path 2: the helper. Spawn, rendezvous, transfer surfaces, encode.
// ---------------------------------------------------------------------------
static bool RunHelper(const char* helperPath, SurfaceSet& surfaces, CodecCode codec,
                      ProfileCode profile, int frames, Stats& encodeStats, Stats& roundTripStats,
                      bool& usedHardware)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return false; }
    DisableSigPipe(sv[0]);
    DisableSigPipe(sv[1]);
    std::random_device rd;
    char name[128];
    snprintf(name, sizeof(name), "org.oxrsys.enc.smoke.%d.%08x", getpid(), rd());
    mach_port_t bp = MACH_PORT_NULL;
    task_get_bootstrap_port(mach_task_self(), &bp);
    mach_port_t parentRx = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_check_in(bp, name, &parentRx);
    if (kr != KERN_SUCCESS)
    {
        fprintf(stderr, "[smoke] bootstrap_check_in failed: %d (%s)\n", kr, bootstrap_strerror(kr));
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], kChildSocketFd);
    // Only close the parent end if it is not the dup2 target (dup2 already
    // closed the target's old occupant). Closing the target would kill the
    // socket we just installed.
    if (sv[0] != kChildSocketFd) posix_spawn_file_actions_addclose(&fa, sv[0]);
    if (sv[1] != kChildSocketFd) posix_spawn_file_actions_addclose(&fa, sv[1]);
    char fdArg[16];
    snprintf(fdArg, sizeof(fdArg), "%d", kChildSocketFd);
    char* av[] = { (char*)helperPath, (char*)"--socket-fd", fdArg, (char*)"--rendezvous", name,
                   nullptr };
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, helperPath, &fa, nullptr, av, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (rc != 0) { fprintf(stderr, "[smoke] posix_spawn failed: %s\n", strerror(rc)); return false; }
    const int sock = sv[0];
    printf("[smoke] spawned helper pid=%d for %s\n", pid, CodecLabel(codec, profile));

    {
        InitPayload init;
        init.width = kW;
        init.height = kH;
        init.fps = kFps;
        init.bitrateMbps = kMbps;
        init.keyframeIntervalSec = 2;
        init.slotCount = kSlots;
        init.preset = PresetCode::Balanced;
        init.codec = codec;
        init.profile = profile;
        Send(sock, MsgType::Init, SerializeInit(init));
    }

    PortMsgRecv rmsg = {};
    kr = mach_msg(&rmsg.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(rmsg), parentRx, 5000,
                  MACH_PORT_NULL);
    if (kr != KERN_SUCCESS || rmsg.msg.header.msgh_id != kMsgIdChildPort)
    {
        fprintf(stderr, "[smoke] no child port: kr=%d id=0x%x\n", kr, rmsg.msg.header.msgh_id);
        return false;
    }
    const mach_port_t childPort = rmsg.msg.port.name;
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        mach_port_t sp = IOSurfaceCreateMachPort(surfaces.surfaces[i]);
        PortMsg m = {};
        m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
        m.header.msgh_size = sizeof(m);
        m.header.msgh_remote_port = childPort;
        m.header.msgh_id = kMsgIdSurface;
        m.body.msgh_descriptor_count = 1;
        m.port.name = sp;
        m.port.disposition = MACH_MSG_TYPE_MOVE_SEND;
        m.port.type = MACH_MSG_PORT_DESCRIPTOR;
        m.slot = i;
        m.width = kW;
        m.height = kH;
        m.pixelFormat = kPixelFormatBGRA;
        kr = mach_msg(&m.header, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                      MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
        {
            fprintf(stderr, "[smoke] send surface %u failed: %d\n", i, kr);
            return false;
        }
    }
    printf("[smoke] transferred %u surfaces via mach\n", kSlots);

    MsgType t;
    std::vector<uint8_t> p;
    if (!Recv(sock, t, p) || t != MsgType::InitAck)
    {
        fprintf(stderr, "[smoke] no InitAck\n");
        return false;
    }
    usedHardware = false;
    {
        Reader r(p.data(), p.size());
        const uint32_t st = r.U32();
        const uint8_t* hw = r.Bytes(1);
        usedHardware = hw != nullptr && *hw != 0;
        printf("[smoke] InitAck status=%u hardware=%s\n", st, usedHardware ? "YES" : "NO");
        if (st != 0 || !usedHardware)
        {
            fprintf(stderr, "[smoke] FAIL: helper did not get the hardware encoder\n");
            return false;
        }
    }

    // Encode one frame at a time (submit, wait for its FrameDone), exactly as
    // the runtime gates slot reuse on encode completion. `encodeStats` is what
    // the helper measured inside its own process; `roundTripStats` is what the
    // runtime actually waits for, IPC included — the honest comparison against
    // the in-process number.
    size_t nalBytes = 0;
    int nalCount = 0;
    for (int f = 0; f < frames; ++f)
    {
        const uint32_t slot = (uint32_t)(f % kSlots);
        PaintSurface(surfaces.surfaces[slot], f);

        std::vector<uint8_t> e;
        PutU64(e, (uint64_t)(f + 1));
        PutU32(e, slot);
        PutI64(e, (int64_t)f * 13888889);
        e.push_back(f == 0 ? 1 : 0);
        const uint64_t submitTicks = mach_absolute_time();
        Send(sock, MsgType::Encode, e);

        bool frameDone = false;
        while (!frameDone)
        {
            if (!Recv(sock, t, p)) { fprintf(stderr, "[smoke] socket closed early\n"); return false; }
            Reader r(p.data(), p.size());
            if (t == MsgType::Nal)
            {
                r.U64(); r.I64(); r.Bytes(1);
                const uint32_t len = r.U32();
                nalBytes += len;
                nalCount++;
            }
            else if (t == MsgType::FrameDone)
            {
                r.U64();
                const uint8_t* dropped = r.Bytes(1);
                const double ms = r.F64();
                r.Bytes(1);
                const double roundTripMs = MachToMs(submitTicks, mach_absolute_time());
                if (f >= kWarmupFrames && !(dropped && *dropped))
                {
                    encodeStats.Add(ms);
                    roundTripStats.Add(roundTripMs);
                }
                frameDone = true;
            }
        }
        usleep(2000);
    }
    printf("[smoke] helper %s: encode avg=%.2fms max=%.2fms | parent round-trip avg=%.2fms "
           "max=%.2fms over %d frames (%d NAL units, %zu bytes)\n",
           CodecLabel(codec, profile), encodeStats.Avg(), encodeStats.maxMs, roundTripStats.Avg(),
           roundTripStats.maxMs, encodeStats.count, nalCount, nalBytes);

    Send(sock, MsgType::Shutdown, {});
    close(sock);
    int status = 0;
    waitpid(pid, &status, 0);
    mach_port_mod_refs(mach_task_self(), parentRx, MACH_PORT_RIGHT_RECEIVE, -1);
    printf("[smoke] helper exited (status=%d)\n", WEXITSTATUS(status));
    return true;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <helper-path> [--codec hevc|h264] [--profile main|main10] "
                        "[--frames N] [--skip-in-process] [--skip-helper]\n",
                argv[0]);
        return 2;
    }
    const char* helperPath = argv[1];
    std::vector<CodecCode> codecs = { CodecCode::H265, CodecCode::H264 };
    ProfileCode profile = ProfileCode::Main;
    int frames = 40;
    bool runInProcess = true;
    bool runHelper = true;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--codec" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            codecs = { value == "h264" ? CodecCode::H264 : CodecCode::H265 };
        }
        else if (arg == "--profile" && i + 1 < argc)
        {
            profile = std::string(argv[++i]) == "main10" ? ProfileCode::Main10 : ProfileCode::Main;
        }
        else if (arg == "--frames" && i + 1 < argc)
        {
            frames = atoi(argv[++i]);
        }
        else if (arg == "--skip-in-process") { runInProcess = false; }
        else if (arg == "--skip-helper") { runHelper = false; }
    }

#if defined(__x86_64__)
    const char* hostArch = "x86_64 (Rosetta)";
#else
    const char* hostArch = "arm64 (native)";
#endif
    // The helper raises its own QoS; the measuring thread does the same so both
    // paths are timed under the same scheduling class.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    printf("[smoke] parent process architecture: %s\n", hostArch);

    SurfaceSet surfaces;
    if (!CreateSurfaces(surfaces)) return 3;

    int failures = 0;
    for (const CodecCode codec : codecs)
    {
        const ProfileCode effectiveProfile = codec == CodecCode::H264 ? ProfileCode::Main : profile;
        printf("\n[smoke] ===== %s =====\n", CodecLabel(codec, effectiveProfile));

        Stats inProcess;
        bool inProcessHardware = false;
        if (runInProcess && !RunInProcess(surfaces, codec, effectiveProfile, frames, inProcess,
                                          inProcessHardware))
        {
            ++failures;
        }

        Stats helperEncode, helperRoundTrip;
        bool helperHardware = false;
        if (runHelper && !RunHelper(helperPath, surfaces, codec, effectiveProfile, frames,
                                    helperEncode, helperRoundTrip, helperHardware))
        {
            ++failures;
        }

        if (runInProcess && runHelper && inProcess.count > 0 && helperRoundTrip.count > 0)
        {
            // Run-to-run spread on this measurement is a few tenths of a
            // millisecond, so anything inside that is a tie, not a win.
            const double delta = inProcess.Avg() - helperRoundTrip.Avg();
            const char* verdict = delta > 0.5    ? "helper wins"
                                  : delta < -0.5 ? "in-process wins"
                                                 : "tie (within run-to-run noise)";
            printf("[smoke] VERDICT %s on %s: in-process %.2fms (hardware=%s) vs helper %.2fms "
                   "round-trip (%.2fms encode + %.2fms IPC, hardware=%s) -> %s\n",
                   CodecLabel(codec, effectiveProfile), hostArch, inProcess.Avg(),
                   inProcessHardware ? "YES" : "NO", helperRoundTrip.Avg(), helperEncode.Avg(),
                   helperRoundTrip.Avg() - helperEncode.Avg(), helperHardware ? "YES" : "NO",
                   verdict);
        }
    }

    DestroySurfaces(surfaces);
    printf("\n[smoke] done (%d failure%s)\n", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
