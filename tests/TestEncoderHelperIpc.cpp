// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "EncoderHelperIpc.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace oxrsys::enc_ipc;

namespace
{

// Mirrors EncoderHelperClient::SubmitFrame / the helper's Encode handler.
std::vector<uint8_t> BuildEncodePayload(uint64_t cookie, uint32_t slot, int64_t ptsNs,
                                        bool forceKeyframe)
{
    std::vector<uint8_t> payload;
    PutU64(payload, cookie);
    PutU32(payload, slot);
    PutI64(payload, ptsNs);
    payload.push_back(forceKeyframe ? 1 : 0);
    return payload;
}

} // namespace

TEST_CASE("Encoder helper frames carry a little-endian header the peer can parse", "[encoder_helper]")
{
    const std::vector<uint8_t> payload = BuildEncodePayload(0x0123456789ABCDEFull, 2, -1234567890LL, true);
    const std::vector<uint8_t> framed = Frame(MsgType::Encode, payload);

    REQUIRE(framed.size() == kHeaderBytes + payload.size());

    // Header bytes are explicitly little-endian, never a memcpy'd struct: the
    // parent is x86_64 (Rosetta) and the child is native arm64.
    Reader header(framed.data(), framed.size());
    CHECK(header.U32() == kMagic);
    CHECK(header.U16() == static_cast<uint16_t>(MsgType::Encode));
    CHECK(header.U16() == kProtocolVersion);
    CHECK(header.U32() == static_cast<uint32_t>(payload.size()));
    CHECK(header.ok());

    Reader body(framed.data() + kHeaderBytes, framed.size() - kHeaderBytes);
    CHECK(body.U64() == 0x0123456789ABCDEFull);
    CHECK(body.U32() == 2u);
    CHECK(body.I64() == -1234567890LL);
    const uint8_t* forceKeyframe = body.Bytes(1);
    REQUIRE(forceKeyframe != nullptr);
    CHECK(*forceKeyframe == 1);
    CHECK(body.ok());
    CHECK(body.remaining() == 0);
}

TEST_CASE("Encoder helper FrameDone round-trips its metrics", "[encoder_helper]")
{
    std::vector<uint8_t> payload;
    PutU64(payload, 0xDEADBEEFCAFEF00Dull);
    payload.push_back(0); // not dropped
    PutF64(payload, 8.125);
    payload.push_back(1); // keyframe

    Reader reader(payload.data(), payload.size());
    CHECK(reader.U64() == 0xDEADBEEFCAFEF00Dull);
    const uint8_t* dropped = reader.Bytes(1);
    REQUIRE(dropped != nullptr);
    CHECK(*dropped == 0);
    CHECK(reader.F64() == 8.125);
    const uint8_t* keyframe = reader.Bytes(1);
    REQUIRE(keyframe != nullptr);
    CHECK(*keyframe == 1);
    CHECK(reader.ok());
}

TEST_CASE("Encoder helper reader reports underflow instead of reading past the payload",
          "[encoder_helper]")
{
    std::vector<uint8_t> payload;
    PutU32(payload, 7);

    Reader reader(payload.data(), payload.size());
    CHECK(reader.U32() == 7u);
    CHECK(reader.ok());

    // A truncated message must fail the whole parse, not return garbage.
    CHECK(reader.U64() == 0u);
    CHECK_FALSE(reader.ok());
    CHECK(reader.Bytes(1) == nullptr);
}

TEST_CASE("Encoder helper protocol bounds are sane for the runtime's slot count", "[encoder_helper]")
{
    // The parent sends the authoritative slot count in Init; kMaxSlots only
    // bounds validation on the child.
    CHECK(kMaxSlots >= 3u);
    CHECK(kMaxPayloadBytes >= 1u * 1024u * 1024u);
    // 'BGRA' is the compose format the runtime hands the helper.
    CHECK(kPixelFormatBGRA == 0x42475241u);
}

TEST_CASE("Encoder helper Init carries the negotiated codec and profile", "[encoder_helper]")
{
    // The helper must encode what the client and runtime agreed, never a
    // substitute, so the negotiated codec and profile travel explicitly.
    InitPayload sent;
    sent.width = 2272;
    sent.height = 1264;
    sent.fps = 72;
    sent.bitrateMbps = 50;
    sent.keyframeIntervalSec = 2;
    sent.slotCount = 3;
    sent.preset = PresetCode::Speed;
    sent.codec = CodecCode::H264;
    sent.profile = ProfileCode::Main;

    const std::vector<uint8_t> payload = SerializeInit(sent);
    InitPayload received;
    REQUIRE(DeserializeInit(payload.data(), payload.size(), received));
    CHECK(received.width == 2272u);
    CHECK(received.height == 1264u);
    CHECK(received.fps == 72u);
    CHECK(received.bitrateMbps == 50u);
    CHECK(received.keyframeIntervalSec == 2u);
    CHECK(received.slotCount == 3u);
    CHECK(received.preset == PresetCode::Speed);
    CHECK(received.codec == CodecCode::H264);
    CHECK(received.profile == ProfileCode::Main);

    // HEVC Main10 rides the same 8-bit BGRA surface contract; only the profile
    // field changes.
    sent.codec = CodecCode::H265;
    sent.profile = ProfileCode::Main10;
    const std::vector<uint8_t> tenBit = SerializeInit(sent);
    CHECK(tenBit.size() == payload.size());
    REQUIRE(DeserializeInit(tenBit.data(), tenBit.size(), received));
    CHECK(received.codec == CodecCode::H265);
    CHECK(received.profile == ProfileCode::Main10);
    CHECK(std::string(CodecName(received.codec)) == "H.265");
    CHECK(std::string(CodecName(CodecCode::H264)) == "H.264");
}

TEST_CASE("Encoder helper Init rejects payloads it cannot honour", "[encoder_helper]")
{
    InitPayload valid;
    valid.width = 1920;
    valid.height = 1080;
    valid.slotCount = 3;
    const std::vector<uint8_t> payload = SerializeInit(valid);
    InitPayload out;

    // Truncated.
    CHECK_FALSE(DeserializeInit(payload.data(), payload.size() - 1, out));

    // Out-of-range enum values: reading a codec the helper has no mapping for
    // must fail the handshake rather than fall through to a default codec.
    std::vector<uint8_t> badCodec = payload;
    badCodec[7 * 4] = 9;
    CHECK_FALSE(DeserializeInit(badCodec.data(), badCodec.size(), out));

    std::vector<uint8_t> badProfile = payload;
    badProfile[8 * 4] = 7;
    CHECK_FALSE(DeserializeInit(badProfile.data(), badProfile.size(), out));

    // Geometry / slot count the child cannot allocate for.
    InitPayload noSlots = valid;
    noSlots.slotCount = 0;
    const std::vector<uint8_t> zeroSlots = SerializeInit(noSlots);
    CHECK_FALSE(DeserializeInit(zeroSlots.data(), zeroSlots.size(), out));

    InitPayload tooManySlots = valid;
    tooManySlots.slotCount = kMaxSlots + 1;
    const std::vector<uint8_t> overflow = SerializeInit(tooManySlots);
    CHECK_FALSE(DeserializeInit(overflow.data(), overflow.size(), out));

    InitPayload noGeometry = valid;
    noGeometry.width = 0;
    const std::vector<uint8_t> zeroWidth = SerializeInit(noGeometry);
    CHECK_FALSE(DeserializeInit(zeroWidth.data(), zeroWidth.size(), out));
}

TEST_CASE("Encoder helper header parse rejects a foreign or stale peer", "[encoder_helper]")
{
    const std::vector<uint8_t> framed = Frame(MsgType::Init, SerializeInit(InitPayload{
                                                                 2272, 1264, 72, 50, 2, 3,
                                                                 PresetCode::Balanced,
                                                                 CodecCode::H265,
                                                                 ProfileCode::Main}));
    const FrameHeader good = ParseHeader(framed.data(), framed.size());
    CHECK(good.ok);
    CHECK(good.type == MsgType::Init);
    CHECK(good.version == kProtocolVersion);
    CHECK(good.payloadLength == framed.size() - kHeaderBytes);

    // A helper binary left over from protocol v1 beside a v2 runtime: reject the
    // frame so the runtime falls back to the in-process encoder instead of
    // misreading a payload whose layout it does not know.
    std::vector<uint8_t> staleVersion = framed;
    staleVersion[6] = 1;
    staleVersion[7] = 0;
    CHECK_FALSE(ParseHeader(staleVersion.data(), staleVersion.size()).ok);

    std::vector<uint8_t> foreignMagic = framed;
    foreignMagic[0] ^= 0xFF;
    CHECK_FALSE(ParseHeader(foreignMagic.data(), foreignMagic.size()).ok);

    // An absurd length must be refused before anything is allocated for it.
    std::vector<uint8_t> hugePayload = framed;
    hugePayload[8] = 0xFF;
    hugePayload[9] = 0xFF;
    hugePayload[10] = 0xFF;
    hugePayload[11] = 0xFF;
    CHECK_FALSE(ParseHeader(hugePayload.data(), hugePayload.size()).ok);

    CHECK_FALSE(ParseHeader(framed.data(), kHeaderBytes - 1).ok);
    CHECK_FALSE(ParseHeader(nullptr, kHeaderBytes).ok);
}

namespace
{

// The runtime is a library inside someone else's process, so these tests must
// not lean on a process-wide SIG_IGN either: assert SIGPIPE is at its default
// (terminate) disposition, so that a write that raised it would kill the test
// binary rather than pass unnoticed.
void RequireDefaultSigPipe()
{
    struct sigaction current = {};
    REQUIRE(sigaction(SIGPIPE, nullptr, &current) == 0);
    REQUIRE(current.sa_handler == SIG_DFL);
}

} // namespace

TEST_CASE("Encoder helper control socket reports a vanished peer as EPIPE, not SIGPIPE",
          "[encoder_helper]")
{
    RequireDefaultSigPipe();
    const std::vector<uint8_t> framed = Frame(MsgType::Encode, BuildEncodePayload(1, 0, 0, false));

    SECTION("SO_NOSIGPIPE on the descriptor")
    {
        int sv[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        REQUIRE(DisableSigPipe(sv[0]));
        ::close(sv[1]); // the helper died

        // Before the fix this raised SIGPIPE and the process died right here.
        errno = 0;
        CHECK_FALSE(SendAll(sv[0], framed.data(), framed.size()));
        CHECK(errno == EPIPE);
        // And keeps failing cleanly on every later write.
        CHECK_FALSE(SendAll(sv[0], framed.data(), framed.size()));
        ::close(sv[0]);
    }

    SECTION("MSG_NOSIGNAL alone, for a descriptor that missed SO_NOSIGPIPE")
    {
        int sv[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ::close(sv[1]);
        errno = 0;
        CHECK_FALSE(SendAll(sv[0], framed.data(), framed.size()));
        CHECK(errno == EPIPE);
        ::close(sv[0]);
    }

    SECTION("a locally shut-down socket, as MarkDead leaves it")
    {
        int sv[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        REQUIRE(DisableSigPipe(sv[0]));
        REQUIRE(::shutdown(sv[0], SHUT_RDWR) == 0);
        errno = 0;
        CHECK_FALSE(SendAll(sv[0], framed.data(), framed.size()));
        CHECK(errno == EPIPE);
        uint8_t byte = 0;
        CHECK_FALSE(RecvAll(sv[0], &byte, 1)); // EOF, which ends the reader loop
        ::close(sv[0]);
        ::close(sv[1]);
    }

    SECTION("a live peer still receives the whole frame")
    {
        int sv[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        REQUIRE(DisableSigPipe(sv[0]));
        REQUIRE(SendAll(sv[0], framed.data(), framed.size()));
        std::vector<uint8_t> received(framed.size());
        REQUIRE(RecvAll(sv[1], received.data(), received.size()));
        CHECK(received == framed);
        ::close(sv[0]);
        ::close(sv[1]);
    }
}
