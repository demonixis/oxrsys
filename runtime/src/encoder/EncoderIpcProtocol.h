// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <mach/mach_time.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Wire protocol between the runtime's NativeHelperEncoderTransport (parent,
 * possibly x86_64 under Rosetta) and the native-arm64 encoder helper process
 * (child), spoken over a Unix stream socket. IOSurface transfer is NOT part of
 * this protocol — surfaces travel as Mach send rights (EncoderMachSurface.h);
 * this socket carries control, frame submission, and encoded results.
 *
 * Rules this file enforces by construction:
 *  - Framework-free: no Foundation/Metal/VideoToolbox/ALVR includes; the
 *    header must compile in both the x86_64 runtime and the arm64 helper.
 *  - Explicit little-endian field-by-field serialization. Raw struct
 *    memcpy is forbidden: the two sides are different compilers/arches and
 *    struct padding is not part of the contract.
 *  - All timestamps that cross the boundary are wall-clock NANOSECONDS
 *    (mach_absolute_time * numer / denom). Gate B proved raw mach ticks are
 *    not comparable across the Rosetta boundary (x86_64 sees a 1/1 timebase,
 *    native arm64 125/3).
 *  - Every frame submission carries the authoritative bitrate; the helper
 *    compares against its applied value and applies deltas, so a lost or
 *    reordered control update can never wedge the stream at a stale bitrate.
 */
namespace oxrsys::encoder::ipc
{

// 'OXEH' big-endian byte order on the wire when written LE as 0x4845584F —
// what matters is both sides agree on the u32 value below.
constexpr uint32_t kMagic = 0x4F584548u; // 'OXEH'
constexpr uint16_t kProtocolMajor = 1;
constexpr uint16_t kProtocolMinor = 0;

/// Hard cap for one FrameResult's contiguous Annex-B payload.
constexpr uint32_t kMaxFramePayloadBytes = 16u * 1024u * 1024u;
constexpr uint32_t kMaxNalUnits = 256;
constexpr uint32_t kMaxErrorMessageBytes = 1024;
/// Upper bound for any message payload (frame payload + descriptors + fixed
/// fields, rounded up). Bounds the read loop before any allocation happens.
constexpr uint32_t kMaxPayloadSize = kMaxFramePayloadBytes + 64u * 1024u;

/// Fixed slot count of the rotating IOSurface ring (mirrors the compose ring).
constexpr uint32_t kSlotCount = 3;

/// fourcc of the compose target, 'BGRA'.
constexpr uint32_t kPixelFormatBGRA = 0x42475241;

/**
 * The wire clock: normalized wall nanoseconds, per the timestamp rule above.
 * Lives here rather than in either process because it IS the contract — the
 * parent and the helper must derive their timestamps identically, and they run
 * under different timebases (x86_64 Rosetta 1/1, native arm64 125/3). Only
 * needs <mach/mach_time.h>, so the framework-free rule still holds.
 */
inline uint64_t NowNs()
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return tb;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

enum class MessageType : uint16_t
{
    Hello = 1,             ///< parent -> child: version + parent pid
    HelloReply = 2,        ///< child -> parent: arch/translated/capabilities
    ConfigureGeneration = 3, ///< parent -> child: (re)create the encode session
    ConfigureAck = 4,      ///< child -> parent: session create outcome
    FrameSubmit = 5,       ///< parent -> child: encode slot N as frame id F
    FrameResult = 6,       ///< child -> parent: encoded Annex-B payload
    FrameDropped = 7,      ///< child -> parent: terminal drop for a frame id
    Drain = 8,             ///< parent -> child: flush in-flight frames
    DrainComplete = 9,     ///< child -> parent: drain finished
    Shutdown = 10,         ///< parent -> child: orderly exit request
    ShutdownAck = 11,      ///< child -> parent: about to exit
    FatalError = 12,       ///< child -> parent: unrecoverable failure
};

// HelloReply.arch values.
constexpr uint32_t kArchUnknown = 0;
constexpr uint32_t kArchX86_64 = 1;
constexpr uint32_t kArchArm64 = 2;

// Per-codec capability bits (HelloReply). Obtained by TRIAL SESSION CREATE
// (RequireHardware + EnableLowLatencyRateControl), never by
// VTCopyVideoEncoderList: Gate A/B2 proved the UsingHardware query itself
// fails (-12900) on arm64 LL-RC sessions while the rtvc hardware encoder is in
// fact in use — create-success is the only trustworthy probe.
constexpr uint32_t kCodecCapHardware = 1u << 0;
constexpr uint32_t kCodecCapLowLatency = 1u << 1;

// Codec identifiers (ConfigureGeneration.codec). Values deliberately mirror
// oxr::protocol::VideoCodec so the transport can cast without a mapping table;
// TestEncoderIpc statically re-asserts the equivalence.
constexpr uint32_t kCodecH265 = 0;
constexpr uint32_t kCodecH264 = 1;

// Color contract enums: 1 = ITU-R BT.709 for all three fields. Only BT.709 is
// defined today; the field exists so a future contract change is negotiated
// instead of assumed.
constexpr uint32_t kColorBt709 = 1;

// Encoder preset (ConfigureGeneration.preset), mirroring the config strings
// the VideoToolbox engine understands. The mapping lives here so parent and
// helper cannot diverge on it.
constexpr uint32_t kPresetBalanced = 0;
constexpr uint32_t kPresetSpeed = 1;
constexpr uint32_t kPresetQuality = 2;

inline uint32_t PresetToWire(const std::string& preset)
{
    if (preset == "speed")
    {
        return kPresetSpeed;
    }
    if (preset == "quality")
    {
        return kPresetQuality;
    }
    return kPresetBalanced;
}

inline const char* PresetFromWire(uint32_t preset)
{
    switch (preset)
    {
        case kPresetSpeed: return "speed";
        case kPresetQuality: return "quality";
        default: return "balanced";
    }
}

// FrameSubmit.flags
constexpr uint32_t kFrameFlagForceIdr = 1u << 0;
// FrameResult.flags
constexpr uint32_t kFrameResultFlagIsIdr = 1u << 0;

// FrameDropped.reason
constexpr uint32_t kDropEncodeFailed = 1;
constexpr uint32_t kDropQueueFull = 2;
constexpr uint32_t kDropUnknownSlot = 3;
constexpr uint32_t kDropBadGeneration = 4;
constexpr uint32_t kDropShuttingDown = 5;
constexpr uint32_t kDropEncoderReported = 6; ///< VT callback reported a drop

// ---------------------------------------------------------------------------
// Little-endian field serialization helpers.
// ---------------------------------------------------------------------------

class PayloadWriter
{
public:
    explicit PayloadWriter(std::vector<uint8_t>& out) : out_(out) {}

    void U16(uint16_t v)
    {
        out_.push_back((uint8_t)(v & 0xFF));
        out_.push_back((uint8_t)(v >> 8));
    }
    void U32(uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            out_.push_back((uint8_t)(v >> (8 * i)));
        }
    }
    void U64(uint64_t v)
    {
        for (int i = 0; i < 8; i++)
        {
            out_.push_back((uint8_t)(v >> (8 * i)));
        }
    }
    void I32(int32_t v) { U32((uint32_t)v); }
    void I64(int64_t v) { U64((uint64_t)v); }
    void Bytes(const uint8_t* data, size_t size)
    {
        out_.insert(out_.end(), data, data + size);
    }

private:
    std::vector<uint8_t>& out_;
};

class PayloadReader
{
public:
    PayloadReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint16_t U16()
    {
        if (!Need(2))
        {
            return 0;
        }
        uint16_t v = (uint16_t)(data_[off_] | (data_[off_ + 1] << 8));
        off_ += 2;
        return v;
    }
    uint32_t U32()
    {
        if (!Need(4))
        {
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
        {
            v |= (uint32_t)data_[off_ + i] << (8 * i);
        }
        off_ += 4;
        return v;
    }
    uint64_t U64()
    {
        if (!Need(8))
        {
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
        {
            v |= (uint64_t)data_[off_ + i] << (8 * i);
        }
        off_ += 8;
        return v;
    }
    int32_t I32() { return (int32_t)U32(); }
    int64_t I64() { return (int64_t)U64(); }
    bool Bytes(uint8_t* out, size_t size)
    {
        if (!Need(size))
        {
            return false;
        }
        for (size_t i = 0; i < size; i++)
        {
            out[i] = data_[off_ + i];
        }
        off_ += size;
        return true;
    }
    const uint8_t* Peek(size_t size)
    {
        if (!Need(size))
        {
            return nullptr;
        }
        const uint8_t* p = data_ + off_;
        off_ += size;
        return p;
    }
    size_t Remaining() const { return ok_ ? size_ - off_ : 0; }
    /// True when every read stayed in bounds AND the payload was fully consumed.
    bool FinishExact() const { return ok_ && off_ == size_; }
    bool Ok() const { return ok_; }

private:
    bool Need(size_t n)
    {
        if (!ok_ || size_ - off_ < n)
        {
            ok_ = false;
            return false;
        }
        return true;
    }
    const uint8_t* data_;
    size_t size_;
    size_t off_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Common message header. 24 bytes on the wire, always little-endian.
// ---------------------------------------------------------------------------

struct MessageHeader
{
    uint32_t magic = kMagic;
    uint16_t versionMajor = kProtocolMajor;
    uint16_t versionMinor = kProtocolMinor;
    uint16_t type = 0; ///< MessageType
    uint16_t flags = 0;
    uint32_t payloadSize = 0;
    uint64_t sequence = 0;

    static constexpr size_t kWireSize = 24;

    void Serialize(uint8_t out[kWireSize]) const
    {
        std::vector<uint8_t> buf;
        buf.reserve(kWireSize);
        PayloadWriter w(buf);
        w.U32(magic);
        w.U16(versionMajor);
        w.U16(versionMinor);
        w.U16(type);
        w.U16(flags);
        w.U32(payloadSize);
        w.U64(sequence);
        for (size_t i = 0; i < kWireSize; i++)
        {
            out[i] = buf[i];
        }
    }

    static bool Deserialize(const uint8_t* data, size_t size, MessageHeader& out)
    {
        PayloadReader r(data, size);
        out.magic = r.U32();
        out.versionMajor = r.U16();
        out.versionMinor = r.U16();
        out.type = r.U16();
        out.flags = r.U16();
        out.payloadSize = r.U32();
        out.sequence = r.U64();
        return r.FinishExact();
    }
};
static_assert(MessageHeader::kWireSize == 24, "header layout is part of the wire contract");

/// Reject bad magic, incompatible major version, and oversize payloads BEFORE
/// any payload allocation. Minor-version differences are tolerated (additive
/// changes only by contract).
inline bool ValidateHeader(const MessageHeader& header)
{
    return header.magic == kMagic && header.versionMajor == kProtocolMajor &&
           header.payloadSize <= kMaxPayloadSize;
}

// ---------------------------------------------------------------------------
// Messages.
// ---------------------------------------------------------------------------

struct Hello
{
    uint32_t parentPid = 0;
    uint32_t reserved = 0;

    static constexpr size_t kWireSize = 8;
    static constexpr MessageType kType = MessageType::Hello;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(parentPid);
        w.U32(reserved);
    }
    static bool Deserialize(const uint8_t* data, size_t size, Hello& out)
    {
        PayloadReader r(data, size);
        out.parentPid = r.U32();
        out.reserved = r.U32();
        return r.FinishExact();
    }
};

struct HelloReply
{
    uint32_t arch = kArchUnknown; ///< kArch*
    uint32_t translated = 0;      ///< 1 when running under Rosetta
    uint32_t capsH264 = 0;        ///< kCodecCap* (trial-create probed)
    uint32_t capsH265 = 0;        ///< kCodecCap* (trial-create probed)
    uint32_t macosMajor = 0;
    uint32_t helperPid = 0;

    static constexpr size_t kWireSize = 24;
    static constexpr MessageType kType = MessageType::HelloReply;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(arch);
        w.U32(translated);
        w.U32(capsH264);
        w.U32(capsH265);
        w.U32(macosMajor);
        w.U32(helperPid);
    }
    static bool Deserialize(const uint8_t* data, size_t size, HelloReply& out)
    {
        PayloadReader r(data, size);
        out.arch = r.U32();
        out.translated = r.U32();
        out.capsH264 = r.U32();
        out.capsH265 = r.U32();
        out.macosMajor = r.U32();
        out.helperPid = r.U32();
        return r.FinishExact();
    }
};

struct ConfigureGeneration
{
    uint32_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t codec = kCodecH265;      ///< kCodec*
    uint32_t bitDepth = 8;            ///< 8 or 10 (HEVC Main10)
    uint32_t pixelFormat = kPixelFormatBGRA;
    uint32_t fps = 0;
    uint32_t keyframeIntervalSec = 0;
    uint64_t initialBitrateBps = 0;
    uint32_t colorPrimaries = kColorBt709;
    uint32_t transferFunction = kColorBt709;
    uint32_t ycbcrMatrix = kColorBt709;
    uint32_t slotCount = kSlotCount;
    uint32_t preset = kPresetBalanced; ///< kPreset*

    static constexpr size_t kWireSize = 60;
    static constexpr MessageType kType = MessageType::ConfigureGeneration;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
        w.U32(width);
        w.U32(height);
        w.U32(codec);
        w.U32(bitDepth);
        w.U32(pixelFormat);
        w.U32(fps);
        w.U32(keyframeIntervalSec);
        w.U64(initialBitrateBps);
        w.U32(colorPrimaries);
        w.U32(transferFunction);
        w.U32(ycbcrMatrix);
        w.U32(slotCount);
        w.U32(preset);
    }
    static bool Deserialize(const uint8_t* data, size_t size, ConfigureGeneration& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        out.width = r.U32();
        out.height = r.U32();
        out.codec = r.U32();
        out.bitDepth = r.U32();
        out.pixelFormat = r.U32();
        out.fps = r.U32();
        out.keyframeIntervalSec = r.U32();
        out.initialBitrateBps = r.U64();
        out.colorPrimaries = r.U32();
        out.transferFunction = r.U32();
        out.ycbcrMatrix = r.U32();
        out.slotCount = r.U32();
        out.preset = r.U32();
        return r.FinishExact() && out.width != 0 && out.height != 0 &&
               out.slotCount != 0 && out.slotCount <= 16;
    }
};
static_assert(ConfigureGeneration::kWireSize == 13 * 4 + 8);

struct ConfigureAck
{
    uint32_t generation = 0;
    uint32_t status = 0; ///< 0 = session created, nonzero = OSStatus-ish error

    static constexpr size_t kWireSize = 8;
    static constexpr MessageType kType = MessageType::ConfigureAck;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
        w.U32(status);
    }
    static bool Deserialize(const uint8_t* data, size_t size, ConfigureAck& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        out.status = r.U32();
        return r.FinishExact();
    }
};

struct FrameSubmit
{
    uint32_t generation = 0;
    uint32_t slot = 0;
    uint64_t frameId = 0;
    int64_t ptsNs = 0;         ///< presentation time, nanoseconds
    uint32_t ptsTimescale = 1000000000; ///< always ns today; explicit for audit
    uint32_t flags = 0;        ///< kFrameFlag*
    uint64_t bitrateBps = 0;   ///< authoritative EVERY frame (delta-applied)
    uint64_t composedAtNs = 0; ///< parent wall ns at GPU-compose completion
    uint64_t submittedAtNs = 0; ///< parent wall ns just before socket write

    static constexpr size_t kWireSize = 56;
    static constexpr MessageType kType = MessageType::FrameSubmit;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
        w.U32(slot);
        w.U64(frameId);
        w.I64(ptsNs);
        w.U32(ptsTimescale);
        w.U32(flags);
        w.U64(bitrateBps);
        w.U64(composedAtNs);
        w.U64(submittedAtNs);
    }
    static bool Deserialize(const uint8_t* data, size_t size, FrameSubmit& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        out.slot = r.U32();
        out.frameId = r.U64();
        out.ptsNs = r.I64();
        out.ptsTimescale = r.U32();
        out.flags = r.U32();
        out.bitrateBps = r.U64();
        out.composedAtNs = r.U64();
        out.submittedAtNs = r.U64();
        return r.FinishExact() && out.ptsTimescale != 0;
    }
};
static_assert(FrameSubmit::kWireSize == 4 + 4 + 8 + 8 + 4 + 4 + 8 + 8 + 8);

/// One Annex-B NAL unit inside FrameResult::data. 8 bytes on the wire.
struct NalDescriptor
{
    uint32_t offset = 0; ///< byte offset of the 00 00 00 01 start code
    uint32_t length = 0; ///< total bytes including the 4-byte start code

    static constexpr size_t kWireSize = 8;
};

struct FrameResult
{
    uint32_t generation = 0;
    uint64_t frameId = 0;
    int32_t status = 0;
    uint32_t flags = 0; ///< kFrameResultFlag*
    uint64_t encodeStartNs = 0;  ///< helper wall ns at EncodeFrame submission
    uint64_t callbackAtNs = 0;   ///< helper wall ns inside the VT callback
    std::vector<NalDescriptor> nalUnits;
    std::vector<uint8_t> data; ///< contiguous Annex-B payload

    static constexpr size_t kFixedWireSize = 44; // + 12*nalCount + data
    static constexpr MessageType kType = MessageType::FrameResult;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kFixedWireSize + nalUnits.size() * NalDescriptor::kWireSize +
                    data.size());
        PayloadWriter w(out);
        w.U32(generation);
        w.U64(frameId);
        w.I32(status);
        w.U32(flags);
        w.U64(encodeStartNs);
        w.U64(callbackAtNs);
        w.U32((uint32_t)nalUnits.size());
        w.U32((uint32_t)data.size());
        for (const NalDescriptor& nal : nalUnits)
        {
            w.U32(nal.offset);
            w.U32(nal.length);
        }
        w.Bytes(data.data(), data.size());
    }

    /// Full descriptor-arithmetic validation: every descriptor must lie inside
    /// the payload with no overflow, counts/caps enforced.
    static bool Deserialize(const uint8_t* payload, size_t size, FrameResult& out)
    {
        PayloadReader r(payload, size);
        out.generation = r.U32();
        out.frameId = r.U64();
        out.status = r.I32();
        out.flags = r.U32();
        out.encodeStartNs = r.U64();
        out.callbackAtNs = r.U64();
        const uint32_t nalCount = r.U32();
        const uint32_t dataSize = r.U32();
        if (!r.Ok() || nalCount > kMaxNalUnits || dataSize > kMaxFramePayloadBytes)
        {
            return false;
        }
        out.nalUnits.clear();
        out.nalUnits.reserve(nalCount);
        for (uint32_t i = 0; i < nalCount; i++)
        {
            NalDescriptor nal;
            nal.offset = r.U32();
            nal.length = r.U32();
            if (!r.Ok())
            {
                return false;
            }
            // Overflow-safe range check: offset + length <= dataSize.
            if (nal.length == 0 || nal.offset > dataSize || dataSize - nal.offset < nal.length)
            {
                return false;
            }
            out.nalUnits.push_back(nal);
        }
        if (r.Remaining() != dataSize)
        {
            return false;
        }
        const uint8_t* dataPtr = r.Peek(dataSize);
        if (dataPtr == nullptr)
        {
            return false;
        }
        out.data.assign(dataPtr, dataPtr + dataSize);
        return r.FinishExact();
    }
};

struct FrameDropped
{
    uint32_t generation = 0;
    uint64_t frameId = 0;
    uint32_t reason = 0; ///< kDrop*
    int32_t status = 0;  ///< underlying OSStatus when applicable

    static constexpr size_t kWireSize = 20;
    static constexpr MessageType kType = MessageType::FrameDropped;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
        w.U64(frameId);
        w.U32(reason);
        w.I32(status);
    }
    static bool Deserialize(const uint8_t* data, size_t size, FrameDropped& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        out.frameId = r.U64();
        out.reason = r.U32();
        out.status = r.I32();
        return r.FinishExact();
    }
};

struct Drain
{
    uint32_t generation = 0;

    static constexpr size_t kWireSize = 4;
    static constexpr MessageType kType = MessageType::Drain;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
    }
    static bool Deserialize(const uint8_t* data, size_t size, Drain& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        return r.FinishExact();
    }
};

struct DrainComplete
{
    uint32_t generation = 0;

    static constexpr size_t kWireSize = 4;
    static constexpr MessageType kType = MessageType::DrainComplete;

    void Serialize(std::vector<uint8_t>& out) const
    {
        out.reserve(out.size() + kWireSize);
        PayloadWriter w(out);
        w.U32(generation);
    }
    static bool Deserialize(const uint8_t* data, size_t size, DrainComplete& out)
    {
        PayloadReader r(data, size);
        out.generation = r.U32();
        return r.FinishExact();
    }
};

// Shutdown and ShutdownAck have empty payloads (header only).

struct FatalError
{
    uint32_t code = 0;
    std::string message; ///< bounded, UTF-8

    static constexpr MessageType kType = MessageType::FatalError;

    void Serialize(std::vector<uint8_t>& out) const
    {
        PayloadWriter w(out);
        w.U32(code);
        const uint32_t length =
            (uint32_t)(message.size() > kMaxErrorMessageBytes ? kMaxErrorMessageBytes
                                                              : message.size());
        w.U32(length);
        w.Bytes((const uint8_t*)message.data(), length);
    }
    static bool Deserialize(const uint8_t* data, size_t size, FatalError& out)
    {
        PayloadReader r(data, size);
        out.code = r.U32();
        const uint32_t length = r.U32();
        if (!r.Ok() || length > kMaxErrorMessageBytes || r.Remaining() != length)
        {
            return false;
        }
        const uint8_t* text = r.Peek(length);
        if (text == nullptr)
        {
            return false;
        }
        out.message.assign((const char*)text, length);
        return r.FinishExact();
    }
};

} // namespace oxrsys::encoder::ipc
