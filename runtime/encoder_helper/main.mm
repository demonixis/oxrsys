// SPDX-License-Identifier: MPL-2.0
//
// oxrsys-encoder-helper — native arm64 out-of-process video encoder.
//
// The OXRSys runtime dylib is loaded in-process by CrossOver's x86_64 Wine host
// (Rosetta), and VideoToolbox refuses the hardware HEVC encoder to an x86_64
// process (kVTCouldNotFindVideoEncoderErr / silent software fallback). This
// helper runs native arm64, so VideoToolbox grants it the hardware encoder.
//
// It encodes whichever codec and profile the runtime negotiated with the client
// — H.265 Main, H.265 Main10 or H.264 Main — and never substitutes another: if
// it cannot honour the request it says so in InitAck and exits, and the runtime
// keeps using its in-process session.
//
// It receives the runtime's compose IOSurfaces once at startup (zero-copy, as
// mach send rights), then per frame it is told "encode slot N" over a Unix
// socket, runs VTCompressionSessionEncodeFrame with RequireHardware=YES, and
// streams the Annex-B NAL units back over the same socket. All GPU composition
// (blit / downscale / foveation) stays in the runtime; only the VT encode moves
// here. See EncoderHelperIpc.h for the wire protocol.

#define OXRSYS_ENC_IPC_WANT_MACH 1
#include "EncoderHelperIpc.h"
#include "EncoderSessionColor.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <servers/bootstrap.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace oxrsys::enc_ipc;

namespace
{

// --------------------------------------------------------------------------
// Logging — plain stderr, line-buffered; the parent captures this into the
// runtime log via a pipe. Prefix makes helper lines obvious in a merged log.
// --------------------------------------------------------------------------
void LogLine(const char* level, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[enc-helper %s] %s\n", level, buf);
    fflush(stderr);
}
#define LOGI(...) LogLine("info", __VA_ARGS__)
#define LOGW(...) LogLine("warn", __VA_ARGS__)
#define LOGE(...) LogLine("error", __VA_ARGS__)

double MachToMs(uint64_t startTicks, uint64_t endTicks)
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    long double ns = (long double)(endTicks - startTicks) * tb.numer / tb.denom;
    return (double)(ns / 1.0e6L);
}

// --------------------------------------------------------------------------
// Session state
// --------------------------------------------------------------------------
struct HelperState
{
    int sock = -1;
    std::mutex writeMutex; // serializes socket writes (main + VT callback threads)

    VTCompressionSessionRef session = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrateMbps = 0;
    uint32_t keyframeIntervalSec = 2;
    uint32_t slotCount = 0;
    PresetCode preset = PresetCode::Balanced;
    CodecCode codec = CodecCode::H265;
    ProfileCode profile = ProfileCode::Main;

    IOSurfaceRef surfaces[kMaxSlots] = {};
    CVPixelBufferRef pixelBuffers[kMaxSlots] = {};

    std::atomic<bool> running{true};
};

HelperState g;

// Per-frame context threaded through VideoToolbox as sourceFrameRefCon.
struct FrameCtx
{
    uint64_t cookie = 0;
    int64_t ptsNs = 0;
    uint64_t submitTicks = 0;
};

// --------------------------------------------------------------------------
// Socket I/O
// --------------------------------------------------------------------------
bool SendMsg(MsgType type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> framed = Frame(type, payload);
    std::lock_guard<std::mutex> lock(g.writeMutex);
    if (SendAll(g.sock, framed.data(), framed.size()))
    {
        return true;
    }
    // The host closed its end (orderly Stop, or it vanished). Nothing to do
    // but say so once; the main loop's next read hits EOF and exits cleanly.
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        LOGW("socket write failed (%s) — host gone, dropping output", strerror(errno));
    }
    return false;
}

// Read one framed message; returns false on EOF/error. On success fills type +
// payload.
bool RecvMsg(MsgType& outType, std::vector<uint8_t>& outPayload)
{
    uint8_t header[kHeaderBytes];
    if (!RecvAll(g.sock, header, kHeaderBytes)) return false;

    const FrameHeader parsed = ParseHeader(header, kHeaderBytes);
    if (!parsed.ok)
    {
        LOGE("bad frame header (version=%u len=%u) — expected protocol v%u", parsed.version,
             parsed.payloadLength, (unsigned)kProtocolVersion);
        return false;
    }
    outType = parsed.type;
    outPayload.resize(parsed.payloadLength);
    if (parsed.payloadLength > 0 && !RecvAll(g.sock, outPayload.data(), parsed.payloadLength))
        return false;
    return true;
}

// --------------------------------------------------------------------------
// Mach rendezvous: hand the parent a send right to our receive port, then
// collect one IOSurface send right per slot. Parent-side is the mirror in
// EncoderHelperClient.mm. See EncoderHelperIpc.h for the ownership rules.
// --------------------------------------------------------------------------
bool MachRendezvousAndReceiveSurfaces(const std::string& rendezvousName, uint32_t expectedSlots)
{
    mach_port_t bootstrapPort = MACH_PORT_NULL;
    task_get_bootstrap_port(mach_task_self(), &bootstrapPort);
    if (bootstrapPort == MACH_PORT_NULL)
    {
        LOGE("no bootstrap port");
        return false;
    }

    // Look up the parent's rendezvous receive right (checked-in pre-spawn).
    mach_port_t parentPort = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrapPort, rendezvousName.c_str(), &parentPort);
    if (kr != KERN_SUCCESS || parentPort == MACH_PORT_NULL)
    {
        LOGE("bootstrap_look_up('%s') failed: %d (%s)", rendezvousName.c_str(), kr,
             bootstrap_strerror(kr));
        return false;
    }

    // Allocate our own receive port and hand the parent a send right to it.
    mach_port_t childRx = MACH_PORT_NULL;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &childRx);
    if (kr != KERN_SUCCESS)
    {
        LOGE("mach_port_allocate(childRx) failed: %d", kr);
        return false;
    }

    PortMsg msg = {};
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof(msg);
    msg.header.msgh_remote_port = parentPort;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id = kMsgIdChildPort;
    msg.body.msgh_descriptor_count = 1;
    msg.port.name = childRx;
    msg.port.disposition = MACH_MSG_TYPE_MAKE_SEND;
    msg.port.type = MACH_MSG_PORT_DESCRIPTOR;
    kr = mach_msg(&msg.header, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL,
                  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    // We no longer need the send right to the parent's port.
    mach_port_deallocate(mach_task_self(), parentPort);
    if (kr != KERN_SUCCESS)
    {
        LOGE("mach_msg(send child port) failed: %d", kr);
        mach_port_mod_refs(mach_task_self(), childRx, MACH_PORT_RIGHT_RECEIVE, -1);
        return false;
    }

    // Receive one surface send right per slot, tagged with slot + geometry.
    uint32_t received = 0;
    for (uint32_t i = 0; i < expectedSlots; ++i)
    {
        PortMsgRecv rmsg = {};
        // Generous timeout: parent creates ports then sends immediately.
        kr = mach_msg(&rmsg.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(rmsg),
                      childRx, 5000 /*ms*/, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
        {
            LOGE("mach_msg(recv surface %u/%u) failed: %d", i, expectedSlots, kr);
            break;
        }
        if (rmsg.msg.header.msgh_id != kMsgIdSurface ||
            rmsg.msg.body.msgh_descriptor_count != 1)
        {
            LOGE("unexpected mach msg id=0x%x", rmsg.msg.header.msgh_id);
            continue;
        }

        uint32_t slot = rmsg.msg.slot;
        mach_port_t surfPort = rmsg.msg.port.name;
        if (slot >= expectedSlots || slot >= kMaxSlots)
        {
            LOGE("surface slot %u out of range", slot);
            mach_port_deallocate(mach_task_self(), surfPort);
            continue;
        }

        IOSurfaceRef surf = IOSurfaceLookupFromMachPort(surfPort);
        // Lookup does NOT consume the received right — release it now.
        mach_port_deallocate(mach_task_self(), surfPort);
        if (surf == nullptr)
        {
            LOGE("IOSurfaceLookupFromMachPort(slot %u) returned null", slot);
            continue;
        }

        CVPixelBufferRef pb = nullptr;
        CVReturn cvr = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surf, nullptr, &pb);
        if (cvr != kCVReturnSuccess || pb == nullptr)
        {
            LOGE("CVPixelBufferCreateWithIOSurface(slot %u) failed: %d", slot, cvr);
            CFRelease(surf);
            continue;
        }

        g.surfaces[slot] = surf;       // keep the ref; released at shutdown
        g.pixelBuffers[slot] = pb;
        ++received;
        LOGI("received surface slot=%u %ux%u fmt=0x%x", slot, rmsg.msg.width, rmsg.msg.height,
             rmsg.msg.pixelFormat);
    }

    // Drop our receive right; the parent's send right dies with it — no further
    // surfaces expected for the life of the session.
    mach_port_mod_refs(mach_task_self(), childRx, MACH_PORT_RIGHT_RECEIVE, -1);

    if (received != expectedSlots)
    {
        LOGE("surface transfer incomplete: %u/%u", received, expectedSlots);
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// NAL emission — Annex-B, matching the runtime's in-process path exactly.
// --------------------------------------------------------------------------
bool IsKeyframeSample(CMSampleBufferRef sb)
{
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (atts == nullptr || CFArrayGetCount(atts) == 0) return true;
    CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(atts, 0);
    CFBooleanRef notSync = nullptr;
    if (!CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, (const void**)&notSync))
        return true;
    return !CFBooleanGetValue(notSync);
}

void SendNal(uint64_t cookie, int64_t ptsNs, const uint8_t* body, size_t bodyLen, bool key)
{
    std::vector<uint8_t> payload;
    payload.reserve(24 + 4 + bodyLen);
    PutU64(payload, cookie);
    PutI64(payload, ptsNs);
    payload.push_back(key ? 1 : 0);
    PutU32(payload, (uint32_t)(bodyLen + 4)); // include 4-byte Annex-B start code
    // Annex-B start code + payload
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x01);
    payload.insert(payload.end(), body, body + bodyLen);
    SendMsg(MsgType::Nal, payload);
}

// VPS/SPS/PPS (HEVC) or SPS/PPS (H.264). The two live behind different
// CoreMedia accessors, which is the only place the NAL path is codec-specific:
// both emit the same Annex-B framing the runtime's in-process path emits.
size_t ParameterSetCount(CMFormatDescriptionRef fd)
{
    size_t count = 0;
    const OSStatus st =
        g.codec == CodecCode::H264
            ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, nullptr, nullptr, &count,
                                                                 nullptr)
            : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, nullptr, nullptr, &count,
                                                                 nullptr);
    return st == noErr ? count : 0;
}

bool ParameterSetAt(CMFormatDescriptionRef fd, size_t index, const uint8_t** ps, size_t* psSize)
{
    const OSStatus st =
        g.codec == CodecCode::H264
            ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, index, ps, psSize, nullptr,
                                                                 nullptr)
            : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, index, ps, psSize, nullptr,
                                                                 nullptr);
    return st == noErr && *ps != nullptr && *psSize > 0;
}

void EmitSampleNalUnits(CMSampleBufferRef sb, bool key, uint64_t cookie, int64_t ptsNs)
{
    if (key)
    {
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
        if (fd != nullptr)
        {
            const size_t count = ParameterSetCount(fd);
            for (size_t i = 0; i < count; ++i)
            {
                const uint8_t* ps = nullptr;
                size_t psSize = 0;
                if (ParameterSetAt(fd, i, &ps, &psSize))
                {
                    SendNal(cookie, ptsNs, ps, psSize, true);
                }
            }
        }
    }

    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (bb == nullptr) return;
    size_t total = 0;
    char* ptr = nullptr;
    if (CMBlockBufferGetDataPointer(bb, 0, nullptr, &total, &ptr) != noErr || ptr == nullptr)
        return;

    size_t off = 0;
    while (off + 4 <= total)
    {
        uint32_t naluLen = 0;
        memcpy(&naluLen, ptr + off, 4);
        naluLen = CFSwapInt32BigToHost(naluLen);
        off += 4;
        if (naluLen == 0 || off + naluLen > total) break;
        SendNal(cookie, ptsNs, (const uint8_t*)(ptr + off), naluLen, key);
        off += naluLen;
    }
}

void SendFrameDone(uint64_t cookie, bool dropped, double encodeMs, bool key)
{
    std::vector<uint8_t> payload;
    PutU64(payload, cookie);
    payload.push_back(dropped ? 1 : 0);
    PutF64(payload, encodeMs);
    payload.push_back(key ? 1 : 0);
    SendMsg(MsgType::FrameDone, payload);
}

void CompressionCallback(void* /*refcon*/, void* sourceFrameRefCon, OSStatus status,
                         VTEncodeInfoFlags infoFlags, CMSampleBufferRef sb)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    FrameCtx* ctx = static_cast<FrameCtx*>(sourceFrameRefCon);
    if (ctx == nullptr) return;
    double encodeMs = MachToMs(ctx->submitTicks, mach_absolute_time());

    if (status != noErr || sb == nullptr || (infoFlags & kVTEncodeInfo_FrameDropped))
    {
        SendFrameDone(ctx->cookie, true, encodeMs, false);
        delete ctx;
        return;
    }

    bool key = IsKeyframeSample(sb);
    EmitSampleNalUnits(sb, key, ctx->cookie, ctx->ptsNs);
    SendFrameDone(ctx->cookie, false, encodeMs, key);
    delete ctx;
}

// --------------------------------------------------------------------------
// VideoToolbox session
// --------------------------------------------------------------------------
void SetNumberProp(VTCompressionSessionRef s, CFStringRef key, int32_t value)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

CMVideoCodecType CodecType(CodecCode codec)
{
    return codec == CodecCode::H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
}

// Mirrors VideoToolboxProfileLevel() in runtime/src/VideoEncoder.mm — the helper
// must request exactly the profile the runtime negotiated, never a substitute.
CFStringRef ProfileLevel(CodecCode codec, ProfileCode profile)
{
    if (codec == CodecCode::H264)
    {
        return kVTProfileLevel_H264_Main_AutoLevel;
    }
    return profile == ProfileCode::Main10 ? kVTProfileLevel_HEVC_Main10_AutoLevel
                                          : kVTProfileLevel_HEVC_Main_AutoLevel;
}

InitStatus CreateSession()
{
    NSDictionary* spec = @{
        (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES,
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES,
    };

    VTCompressionSessionRef s = nullptr;
    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, g.width, g.height,
                                             CodecType(g.codec),
                                             (__bridge CFDictionaryRef)spec, nullptr,
                                             kCFAllocatorDefault, CompressionCallback, nullptr, &s);
    if (st != noErr || s == nullptr)
    {
        LOGE("VTCompressionSessionCreate(%s, RequireHardware=YES) failed: %d",
             CodecName(g.codec), (int)st);
        return InitStatus::SessionCreateFailed;
    }

    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    // The same BT.709 colour contract as the in-process session, from the one
    // shared definition, so both paths' bitstreams carry identical VUI colour
    // descriptions and use the same RGB-to-YCbCr matrix (Main10 included).
    const oxrsys::encoder_color::ApplyResult color =
        oxrsys::encoder_color::ApplySessionColorProperties(s);
    if (!color.ok())
    {
        LOGW("failed to apply complete BT.709 color metadata (primaries=%d transfer=%d matrix=%d)",
             (int)color.primaries, (int)color.transferFunction, (int)color.yCbCrMatrix);
    }
    const OSStatus profileStatus = VTSessionSetProperty(
        s, kVTCompressionPropertyKey_ProfileLevel, ProfileLevel(g.codec, g.profile));
    if (profileStatus != noErr)
    {
        // Same contract as the in-process path: an unavailable profile is a
        // downgrade to the encoder default, logged, not a failed session.
        LOGW("profile for %s/%s unavailable (%d) — falling back to the encoder default",
             CodecName(g.codec), g.profile == ProfileCode::Main10 ? "Main10" : "Main",
             (int)profileStatus);
    }

    if (g.preset == PresetCode::Speed)
        VTSessionSetProperty(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                             kCFBooleanTrue);
    else if (g.preset == PresetCode::Quality)
        VTSessionSetProperty(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                             kCFBooleanFalse);

    int32_t avgBitrate = (int32_t)(g.bitrateMbps * 1000000u);
    SetNumberProp(s, kVTCompressionPropertyKey_AverageBitRate, avgBitrate);

    double peakBytesPerSecond = (double)(g.bitrateMbps * 1000000u) * 1.5 / 8.0;
    NSArray* limits = @[ @(peakBytesPerSecond), @(1.0) ];
    VTSessionSetProperty(s, kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)limits);

    int32_t keyInterval = (int32_t)(g.keyframeIntervalSec * (g.fps > 0 ? g.fps : 1));
    SetNumberProp(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyInterval);
    double keyDur = (double)g.keyframeIntervalSec;
    CFNumberRef durRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &keyDur);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, durRef);
    CFRelease(durRef);

    SetNumberProp(s, kVTCompressionPropertyKey_ExpectedFrameRate, (int32_t)(g.fps > 0 ? g.fps : 1));
    SetNumberProp(s, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);

    VTCompressionSessionPrepareToEncodeFrames(s);
    g.session = s;

    // Confirm the hardware encoder was actually selected.
    bool usingHardware = false;
    CFBooleanRef hwRef = nullptr;
    if (VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                              kCFAllocatorDefault, &hwRef) == noErr &&
        hwRef != nullptr)
    {
        usingHardware = CFBooleanGetValue(hwRef);
        CFRelease(hwRef);
    }
    LOGI("VT session created %s/%s %ux%u @%ufps %uMbps hardware=%s", CodecName(g.codec),
         g.profile == ProfileCode::Main10 ? "Main10" : "Main", g.width, g.height, g.fps,
         g.bitrateMbps, usingHardware ? "YES" : "NO");
    return usingHardware ? InitStatus::Ok : InitStatus::HardwareUnavailable;
}

void HandleEncode(Reader& r)
{
    uint64_t cookie = r.U64();
    uint32_t slot = r.U32();
    int64_t ptsNs = r.I64();
    uint8_t force = 0;
    const uint8_t* fp = r.Bytes(1);
    if (fp) force = *fp;
    if (!r.ok() || slot >= g.slotCount || g.pixelBuffers[slot] == nullptr)
    {
        LOGW("bad encode msg slot=%u", slot);
        SendFrameDone(cookie, true, 0.0, false);
        return;
    }

    FrameCtx* ctx = new FrameCtx();
    ctx->cookie = cookie;
    ctx->ptsNs = ptsNs;
    ctx->submitTicks = mach_absolute_time();

    CFMutableDictionaryRef frameProps = nullptr;
    if (force)
    {
        frameProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(frameProps, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
    }

    CMTime pts = CMTimeMake(ptsNs, 1000000000);
    OSStatus st = VTCompressionSessionEncodeFrame(g.session, g.pixelBuffers[slot], pts,
                                                  kCMTimeInvalid, frameProps, ctx, nullptr);
    if (frameProps != nullptr) CFRelease(frameProps);
    if (st != noErr)
    {
        LOGW("VTCompressionSessionEncodeFrame failed: %d", (int)st);
        SendFrameDone(cookie, true, MachToMs(ctx->submitTicks, mach_absolute_time()), false);
        delete ctx;
    }
}

} // namespace

int main(int argc, char** argv)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // This is our own process, so a process-wide SIG_IGN is fine here (unlike
    // in the runtime dylib, which lives in the host's process). The host
    // vanishing must end this helper through EOF, not a signal: that covers the
    // control socket and also stderr, which is a pipe the host reads and may
    // already have closed when our last log line is written.
    signal(SIGPIPE, SIG_IGN);

    int sockFd = -1;
    std::string rendezvous;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--socket-fd") == 0 && i + 1 < argc)
            sockFd = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rendezvous") == 0 && i + 1 < argc)
            rendezvous = argv[++i];
    }
    if (sockFd < 0 || rendezvous.empty())
    {
        LOGE("usage: --socket-fd N --rendezvous NAME (got fd=%d name='%s')", sockFd,
             rendezvous.c_str());
        return 2;
    }
    g.sock = sockFd;
    // Belt and braces with SIG_IGN above; the runtime also sets it on its end.
    DisableSigPipe(sockFd);
    LOGI("started pid=%d socket-fd=%d rendezvous=%s", getpid(), sockFd, rendezvous.c_str());

    // 1. Read Init config from the socket.
    MsgType type;
    std::vector<uint8_t> payload;
    if (!RecvMsg(type, payload) || type != MsgType::Init)
    {
        LOGE("expected Init, got type=%d", (int)type);
        return 3;
    }
    {
        InitPayload init;
        if (!DeserializeInit(payload.data(), payload.size(), init))
        {
            LOGE("bad Init payload (%zu bytes)", payload.size());
            return 3;
        }
        g.width = init.width;
        g.height = init.height;
        g.fps = init.fps;
        g.bitrateMbps = init.bitrateMbps;
        g.keyframeIntervalSec = init.keyframeIntervalSec;
        g.slotCount = init.slotCount;
        g.preset = init.preset;
        g.codec = init.codec;
        g.profile = init.profile;
    }
    LOGI("Init: %s/%s %ux%u @%ufps %uMbps key=%us slots=%u preset=%u", CodecName(g.codec),
         g.profile == ProfileCode::Main10 ? "Main10" : "Main", g.width, g.height, g.fps,
         g.bitrateMbps, g.keyframeIntervalSec, g.slotCount, (uint32_t)g.preset);

    // 2. Mach rendezvous: receive the compose surfaces (zero-copy).
    InitStatus status = InitStatus::Ok;
    if (!MachRendezvousAndReceiveSurfaces(rendezvous, g.slotCount))
    {
        status = InitStatus::SurfaceTransferFailed;
    }
    else
    {
        // 3. Create the hardware VT session.
        status = CreateSession();
    }

    // 4. Reply with the outcome + hardware flag.
    bool usingHardware = (status == InitStatus::Ok);
    {
        std::vector<uint8_t> ack;
        PutU32(ack, (uint32_t)status);
        ack.push_back(usingHardware ? 1 : 0);
        SendMsg(MsgType::InitAck, ack);
    }
    if (status != InitStatus::Ok)
    {
        LOGE("init failed status=%u — exiting so the runtime falls back to software",
             (uint32_t)status);
        return 4;
    }
    LOGI("ready: hardware %s encoder live", CodecName(g.codec));

    // 5. Encode loop.
    while (g.running.load())
    {
        if (!RecvMsg(type, payload))
        {
            LOGW("socket closed — parent gone, exiting");
            break;
        }
        Reader r(payload.data(), payload.size());
        switch (type)
        {
        case MsgType::Encode:
            HandleEncode(r);
            break;
        case MsgType::SetBitrate:
        {
            uint32_t mbps = r.U32();
            if (r.ok() && mbps > 0 && g.session != nullptr)
            {
                SetNumberProp(g.session, kVTCompressionPropertyKey_AverageBitRate,
                              (int32_t)(mbps * 1000000u));
                double peak = (double)(mbps * 1000000u) * 1.5 / 8.0;
                NSArray* limits = @[ @(peak), @(1.0) ];
                VTSessionSetProperty(g.session, kVTCompressionPropertyKey_DataRateLimits,
                                     (__bridge CFArrayRef)limits);
                g.bitrateMbps = mbps;
                LOGI("bitrate -> %uMbps", mbps);
            }
            break;
        }
        case MsgType::Shutdown:
            LOGI("shutdown requested");
            g.running.store(false);
            break;
        default:
            LOGW("unexpected msg type=%d", (int)type);
            break;
        }
    }

    // 6. Teardown.
    if (g.session != nullptr)
    {
        VTCompressionSessionCompleteFrames(g.session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(g.session);
        CFRelease(g.session);
        g.session = nullptr;
    }
    for (uint32_t i = 0; i < kMaxSlots; ++i)
    {
        if (g.pixelBuffers[i]) CFRelease(g.pixelBuffers[i]);
        if (g.surfaces[i]) CFRelease(g.surfaces[i]);
    }
    LOGI("exited");
    return 0;
}
