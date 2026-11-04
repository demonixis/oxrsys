// SPDX-License-Identifier: MPL-2.0

#pragma once

// -----------------------------------------------------------------------------
// Wire protocol between the OXRSys runtime (parent, x86_64 under Rosetta) and
// the native-arm64 video encoder helper (child), spoken over an inherited Unix
// stream socket. This header is FRAMEWORK-FREE by construction so it compiles
// unchanged in the x86_64 runtime dylib and the arm64 helper executable.
//
// IOSurface transfer is NOT carried on this socket. Surfaces travel once, at
// session start, as Mach send rights (IOSurfaceCreateMachPort ->
// IOSurfaceLookupFromMachPort) over a bootstrap rendezvous. The mach message
// layout for that transfer lives at the bottom of this header (a kernel-copied
// same-host message, so a fixed struct is safe there). Everything else — frame
// submission and encoded results — is this socket protocol.
//
// Rules enforced here:
//  * No Foundation / Metal / VideoToolbox / IOSurface includes.
//  * Explicit little-endian field serialization (both macOS arches are LE, but
//    we never memcpy a struct across the boundary — padding is not contract).
//  * Presentation timestamps cross the boundary as int64 nanoseconds that the
//    parent supplies and the child echoes back verbatim. The child never
//    generates a timestamp the parent compares against: mach_absolute_time is
//    NOT comparable across the Rosetta boundary (x86_64 timebase 1/1, native
//    arm64 125/3), so the helper only ever measures deltas within its own
//    process and reports them as already-converted milliseconds.
// -----------------------------------------------------------------------------

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace oxrsys::enc_ipc
{

// Frame magic on the wire, ASCII "OXH1".
inline constexpr uint32_t kMagic = 0x4F584831u;
// v2 added the negotiated codec + profile to Init. Both peers reject a frame
// whose version is not theirs: an out-of-date helper binary next to a newer
// runtime then fails at handshake (and the runtime falls back to the in-process
// encoder) instead of misreading a payload.
inline constexpr uint16_t kProtocolVersion = 2;

// Bound the read loop before any allocation. One HEVC access unit for a
// 2272x1264 stereo frame is far below this.
inline constexpr uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u;

// Matches VideoEncoder::SlotCount. The parent sends the authoritative count in
// Init; this is only an upper bound for validation on the child.
inline constexpr uint32_t kMaxSlots = 16;

// fourcc of the compose target the runtime hands us, 'BGRA'.
inline constexpr uint32_t kPixelFormatBGRA = 0x42475241u;

enum class MsgType : uint16_t
{
    Init = 1,       // parent -> child: session config (surfaces arrive via mach next)
    InitAck = 2,    // child -> parent: session create outcome + hardware flag
    Encode = 3,     // parent -> child: encode slot N as opaque cookie F
    Nal = 4,        // child -> parent: one Annex-B NAL unit for a cookie
    FrameDone = 5,  // child -> parent: terminal result for a cookie (+ metrics)
    SetBitrate = 6, // parent -> child: live bitrate change
    Shutdown = 7,   // parent -> child: orderly exit
};

// Preset selector, mirrors ConfigValues::encoderPreset.
enum class PresetCode : uint32_t
{
    Balanced = 0,
    Speed = 1,
    Quality = 2,
};

// Negotiated codec. Deliberately its own enum rather than oxr::protocol::
// VideoCodec: this header must not depend on the runtime's protocol headers,
// and the wire value must not move if that enum is ever reordered. The values
// happen to match today; CodecFromProtocol on the runtime side is the only
// place that mapping lives.
enum class CodecCode : uint32_t
{
    H265 = 0,
    H264 = 1,
};

// Bitstream profile the parent negotiated. The helper must honour it exactly —
// it never substitutes a codec or profile the client did not agree to. Main10
// is encoded from the same 8-bit BGRA compose surface as Main, matching what
// the in-process path does: 10-bit bitstream precision, 8-bit source.
enum class ProfileCode : uint32_t
{
    Main = 0,   // HEVC Main / H.264 Main, 8-bit
    Main10 = 1, // HEVC Main10 (HEVC only; ignored for H.264)
};

// InitAck status codes — reported so the parent log can say exactly where the
// helper failed rather than a generic "unavailable".
enum class InitStatus : uint32_t
{
    Ok = 0,
    SurfaceTransferFailed = 1,
    SessionCreateFailed = 2,
    HardwareUnavailable = 3, // session created but RequireHardware not honored
    UnsupportedCodec = 4,    // Init named a codec this helper cannot encode
    BadProtocolVersion = 5,  // parent speaks a protocol version this helper does not
};

// The Init payload, in wire order. Shared by both peers so the field order can
// only be got wrong in one place.
struct InitPayload
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrateMbps = 0;
    uint32_t keyframeIntervalSec = 2;
    uint32_t slotCount = 0;
    PresetCode preset = PresetCode::Balanced;
    CodecCode codec = CodecCode::H265;    // v2
    ProfileCode profile = ProfileCode::Main; // v2
};

// -----------------------------------------------------------------------------
// Fixed 12-byte frame header: [magic u32][type u16][version u16][payloadLen u32].
// -----------------------------------------------------------------------------
inline constexpr size_t kHeaderBytes = 12;

// --- little-endian scalar helpers ---
inline void PutU16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}
inline void PutU32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void PutU64(std::vector<uint8_t>& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void PutI64(std::vector<uint8_t>& b, int64_t v) { PutU64(b, (uint64_t)v); }
inline void PutF64(std::vector<uint8_t>& b, double v)
{
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    PutU64(b, u);
}

// Cursor-based reader over a payload buffer. All getters bounds-check and set
// ok=false on underflow; callers check ok() once at the end.
class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint16_t U16()
    {
        if (pos_ + 2 > size_) { ok_ = false; return 0; }
        uint16_t v = (uint16_t)data_[pos_] | ((uint16_t)data_[pos_ + 1] << 8);
        pos_ += 2;
        return v;
    }
    uint32_t U32()
    {
        if (pos_ + 4 > size_) { ok_ = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t)data_[pos_ + i] << (8 * i);
        pos_ += 4;
        return v;
    }
    uint64_t U64()
    {
        if (pos_ + 8 > size_) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t)data_[pos_ + i] << (8 * i);
        pos_ += 8;
        return v;
    }
    int64_t I64() { return (int64_t)U64(); }
    double F64()
    {
        uint64_t u = U64();
        double v;
        std::memcpy(&v, &u, sizeof(v));
        return v;
    }
    // Returns a pointer into the buffer for `len` bytes (zero-copy) or nullptr.
    const uint8_t* Bytes(size_t len)
    {
        if (pos_ + len > size_) { ok_ = false; return nullptr; }
        const uint8_t* p = data_ + pos_;
        pos_ += len;
        return p;
    }

    bool ok() const { return ok_; }
    size_t remaining() const { return size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Build a complete framed message: header + payload.
inline std::vector<uint8_t> Frame(MsgType type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + payload.size());
    PutU32(out, kMagic);
    PutU16(out, (uint16_t)type);
    PutU16(out, kProtocolVersion);
    PutU32(out, (uint32_t)payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Parsed frame header. `ok` is false for a foreign magic, a protocol version
// that is not ours, or an implausible length — all of which must abort the
// connection rather than be interpreted.
struct FrameHeader
{
    MsgType type = MsgType::Init;
    uint16_t version = 0;
    uint32_t payloadLength = 0;
    bool ok = false;
};

inline FrameHeader ParseHeader(const uint8_t* header, size_t size)
{
    FrameHeader out;
    if (header == nullptr || size < kHeaderBytes)
    {
        return out;
    }
    Reader r(header, kHeaderBytes);
    const uint32_t magic = r.U32();
    out.type = (MsgType)r.U16();
    out.version = r.U16();
    out.payloadLength = r.U32();
    out.ok = r.ok() && magic == kMagic && out.version == kProtocolVersion &&
             out.payloadLength <= kMaxPayloadBytes;
    return out;
}

// --- Init payload (de)serialization: one definition, both peers ---
inline std::vector<uint8_t> SerializeInit(const InitPayload& init)
{
    std::vector<uint8_t> p;
    PutU32(p, init.width);
    PutU32(p, init.height);
    PutU32(p, init.fps);
    PutU32(p, init.bitrateMbps);
    PutU32(p, init.keyframeIntervalSec);
    PutU32(p, init.slotCount);
    PutU32(p, (uint32_t)init.preset);
    PutU32(p, (uint32_t)init.codec);
    PutU32(p, (uint32_t)init.profile);
    return p;
}

inline bool DeserializeInit(const uint8_t* data, size_t size, InitPayload& out)
{
    Reader r(data, size);
    out.width = r.U32();
    out.height = r.U32();
    out.fps = r.U32();
    out.bitrateMbps = r.U32();
    out.keyframeIntervalSec = r.U32();
    out.slotCount = r.U32();
    const uint32_t preset = r.U32();
    const uint32_t codec = r.U32();
    const uint32_t profile = r.U32();
    if (!r.ok())
    {
        return false;
    }
    if (preset > (uint32_t)PresetCode::Quality || codec > (uint32_t)CodecCode::H264 ||
        profile > (uint32_t)ProfileCode::Main10)
    {
        return false;
    }
    if (out.slotCount == 0 || out.slotCount > kMaxSlots || out.width == 0 || out.height == 0)
    {
        return false;
    }
    out.preset = (PresetCode)preset;
    out.codec = (CodecCode)codec;
    out.profile = (ProfileCode)profile;
    return true;
}

inline const char* CodecName(CodecCode codec)
{
    return codec == CodecCode::H264 ? "H.264" : "H.265";
}

// -----------------------------------------------------------------------------
// Control-socket I/O that never raises SIGPIPE.
//
// A write to a stream socket whose peer has gone raises SIGPIPE, whose default
// action terminates the writer. On the runtime side the writer is the game
// process (the dylib lives inside someone else's process, under Wine), so a
// helper crash would take the game down with it; on the helper side it would
// turn "the host went away" into a signal death. Suppressed per socket, never
// with a process-wide signal(SIGPIPE, SIG_IGN), which a library has no business
// installing in its host: SO_NOSIGPIPE on the descriptor, plus MSG_NOSIGNAL on
// every send as a second, per-call guard. A write to a dead peer then just
// fails with EPIPE, which both peers already treat as "the other side is gone".
// -----------------------------------------------------------------------------

// Call on every control-socket descriptor as soon as it exists.
inline bool DisableSigPipe(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0;
}

inline constexpr int kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif

// Writes all of `data`, retrying on EINTR. False on any other error (errno is
// left as send() set it: EPIPE when the peer is gone).
inline bool SendAll(int fd, const uint8_t* data, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        const ssize_t n = ::send(fd, data + off, len - off, kSendFlags);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

// Reads exactly `len` bytes, retrying on EINTR. False on EOF or error.
inline bool RecvAll(int fd, uint8_t* data, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        const ssize_t n = ::read(fd, data + off, len - off);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return false; // 0 = EOF: the peer is gone
    }
    return true;
}

// -----------------------------------------------------------------------------
// Mach rendezvous message (parent <-> child), same-host kernel copy. Layout is
// identical for both arches; a fixed struct is intentional and safe here. This
// is transport for IOSurface send rights only; nothing perf-sensitive rides it.
// -----------------------------------------------------------------------------
//
// The mach.h types are pulled in by whichever .mm includes this in a mach
// context; kept out of the framework-free section by guarding on the include.
#if defined(__MACH__) && defined(OXRSYS_ENC_IPC_WANT_MACH)
} // namespace oxrsys::enc_ipc

#include <mach/mach.h>

namespace oxrsys::enc_ipc
{

// Child -> parent: hand the parent a send right to the child's receive port so
// the parent can push surface ports back. msgh_id = kMsgIdChildPort.
// Parent -> child: one IOSurface send right (MOVE_SEND) tagged with the slot it
// backs and its geometry. msgh_id = kMsgIdSurface.
inline constexpr mach_msg_id_t kMsgIdChildPort = 0x0A78'0001;
inline constexpr mach_msg_id_t kMsgIdSurface = 0x0A78'0002;

struct PortMsg
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t slot;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
};

struct PortMsgRecv
{
    PortMsg msg;
    mach_msg_trailer_t trailer;
};

#endif // mach section

} // namespace oxrsys::enc_ipc
