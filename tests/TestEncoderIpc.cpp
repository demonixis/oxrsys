// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>
#include <vector>

#include "encoder/EncoderIpcProtocol.h"
#include "encoder/EncoderIpcSocket.h"

#include <oxrsys/protocol/Protocol.h>

namespace ipc = oxrsys::encoder::ipc;

// The wire codec ids are defined to mirror oxr::protocol::VideoCodec so the
// transport can cast without a mapping table; keep that pinned.
static_assert(ipc::kCodecH265 == (uint32_t)oxr::protocol::VideoCodec::H265);
static_assert(ipc::kCodecH264 == (uint32_t)oxr::protocol::VideoCodec::H264);

namespace
{

std::vector<uint8_t> SerializeHeader(const ipc::MessageHeader& header)
{
    std::vector<uint8_t> bytes(ipc::MessageHeader::kWireSize);
    header.Serialize(bytes.data());
    return bytes;
}

} // namespace

TEST_CASE("Encoder IPC header round-trips little-endian", "[encoder-ipc]")
{
    ipc::MessageHeader header;
    header.type = (uint16_t)ipc::MessageType::FrameSubmit;
    header.flags = 0x1234;
    header.payloadSize = ipc::FrameSubmit::kWireSize;
    header.sequence = 0x0123456789ABCDEFULL;
    const auto bytes = SerializeHeader(header);

    // Spot-check explicit little-endian layout (no struct memcpy on the wire).
    CHECK(bytes[0] == (ipc::kMagic & 0xFF));
    CHECK(bytes[3] == (ipc::kMagic >> 24));
    CHECK(bytes[16] == 0xEF); // sequence LSB

    ipc::MessageHeader parsed;
    REQUIRE(ipc::MessageHeader::Deserialize(bytes.data(), bytes.size(), parsed));
    CHECK(parsed.magic == ipc::kMagic);
    CHECK(parsed.versionMajor == ipc::kProtocolMajor);
    CHECK(parsed.type == header.type);
    CHECK(parsed.flags == header.flags);
    CHECK(parsed.payloadSize == header.payloadSize);
    CHECK(parsed.sequence == header.sequence);
    CHECK(ipc::ValidateHeader(parsed));
}

TEST_CASE("Encoder IPC header validation rejects bad frames", "[encoder-ipc]")
{
    ipc::MessageHeader header;
    SECTION("bad magic")
    {
        header.magic = 0xDEADBEEF;
        CHECK_FALSE(ipc::ValidateHeader(header));
    }
    SECTION("incompatible major version")
    {
        header.versionMajor = ipc::kProtocolMajor + 1;
        CHECK_FALSE(ipc::ValidateHeader(header));
    }
    SECTION("oversize payload")
    {
        header.payloadSize = ipc::kMaxPayloadSize + 1;
        CHECK_FALSE(ipc::ValidateHeader(header));
    }
    SECTION("newer minor version is tolerated")
    {
        header.versionMinor = ipc::kProtocolMinor + 3;
        CHECK(ipc::ValidateHeader(header));
    }
    SECTION("short header buffer")
    {
        uint8_t bytes[ipc::MessageHeader::kWireSize];
        header.Serialize(bytes);
        ipc::MessageHeader parsed;
        CHECK_FALSE(ipc::MessageHeader::Deserialize(bytes, sizeof(bytes) - 1, parsed));
    }
}

TEST_CASE("Encoder IPC control messages round-trip", "[encoder-ipc]")
{
    SECTION("HelloReply")
    {
        ipc::HelloReply reply;
        reply.arch = ipc::kArchArm64;
        reply.translated = 0;
        reply.capsH264 = ipc::kCodecCapHardware | ipc::kCodecCapLowLatency;
        reply.capsH265 = ipc::kCodecCapHardware;
        reply.macosMajor = 27;
        reply.helperPid = 4242;
        std::vector<uint8_t> payload;
        reply.Serialize(payload);
        REQUIRE(payload.size() == ipc::HelloReply::kWireSize);
        ipc::HelloReply parsed;
        REQUIRE(ipc::HelloReply::Deserialize(payload.data(), payload.size(), parsed));
        CHECK(parsed.arch == ipc::kArchArm64);
        CHECK(parsed.capsH264 == (ipc::kCodecCapHardware | ipc::kCodecCapLowLatency));
        CHECK(parsed.capsH265 == ipc::kCodecCapHardware);
        CHECK(parsed.macosMajor == 27);
        CHECK(parsed.helperPid == 4242);
        // Trailing garbage must be rejected (exact-consumption contract).
        payload.push_back(0);
        CHECK_FALSE(ipc::HelloReply::Deserialize(payload.data(), payload.size(), parsed));
    }
    SECTION("ConfigureGeneration")
    {
        ipc::ConfigureGeneration configure;
        configure.generation = 7;
        configure.width = 3008;
        configure.height = 1664;
        configure.codec = ipc::kCodecH264;
        configure.bitDepth = 8;
        configure.fps = 72;
        configure.keyframeIntervalSec = 5;
        configure.initialBitrateBps = 42000000;
        configure.slotCount = ipc::kSlotCount;
        std::vector<uint8_t> payload;
        configure.Serialize(payload);
        REQUIRE(payload.size() == ipc::ConfigureGeneration::kWireSize);
        ipc::ConfigureGeneration parsed;
        REQUIRE(ipc::ConfigureGeneration::Deserialize(payload.data(), payload.size(), parsed));
        CHECK(parsed.generation == 7);
        CHECK(parsed.width == 3008);
        CHECK(parsed.height == 1664);
        CHECK(parsed.initialBitrateBps == 42000000);
        CHECK(parsed.colorPrimaries == ipc::kColorBt709);
        CHECK(parsed.slotCount == ipc::kSlotCount);
    }
    SECTION("ConfigureGeneration rejects zero dims and absurd slot counts")
    {
        ipc::ConfigureGeneration configure;
        configure.width = 0;
        configure.height = 1664;
        std::vector<uint8_t> payload;
        configure.Serialize(payload);
        ipc::ConfigureGeneration parsed;
        CHECK_FALSE(ipc::ConfigureGeneration::Deserialize(payload.data(), payload.size(), parsed));

        configure.width = 3008;
        configure.slotCount = 64;
        payload.clear();
        configure.Serialize(payload);
        CHECK_FALSE(ipc::ConfigureGeneration::Deserialize(payload.data(), payload.size(), parsed));
    }
    SECTION("FrameSubmit")
    {
        ipc::FrameSubmit submit;
        submit.generation = 3;
        submit.slot = 2;
        submit.frameId = 123456789;
        submit.ptsNs = -5; // signed survives the trip
        submit.flags = ipc::kFrameFlagForceIdr;
        submit.bitrateBps = 30000000;
        submit.composedAtNs = 111;
        submit.submittedAtNs = 222;
        std::vector<uint8_t> payload;
        submit.Serialize(payload);
        REQUIRE(payload.size() == ipc::FrameSubmit::kWireSize);
        ipc::FrameSubmit parsed;
        REQUIRE(ipc::FrameSubmit::Deserialize(payload.data(), payload.size(), parsed));
        CHECK(parsed.slot == 2);
        CHECK(parsed.ptsNs == -5);
        CHECK(parsed.flags == ipc::kFrameFlagForceIdr);
        CHECK(parsed.bitrateBps == 30000000);
    }
    SECTION("FatalError bounded message")
    {
        ipc::FatalError fatal;
        fatal.code = 9;
        fatal.message = "vt session died";
        std::vector<uint8_t> payload;
        fatal.Serialize(payload);
        ipc::FatalError parsed;
        REQUIRE(ipc::FatalError::Deserialize(payload.data(), payload.size(), parsed));
        CHECK(parsed.code == 9);
        CHECK(parsed.message == "vt session died");
        // Declared length larger than the actual payload must be rejected.
        payload[4] = 0xFF;
        CHECK_FALSE(ipc::FatalError::Deserialize(payload.data(), payload.size(), parsed));
    }
}

TEST_CASE("Encoder IPC frame result validates descriptor arithmetic", "[encoder-ipc]")
{
    ipc::FrameResult result;
    result.generation = 2;
    result.frameId = 42;
    result.flags = ipc::kFrameResultFlagIsIdr;
    result.encodeStartNs = 1000;
    result.callbackAtNs = 2000;
    result.data = {0, 0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x65, 0xBB, 0xCC};
    result.nalUnits.push_back({0, 6, 7});
    result.nalUnits.push_back({6, 7, 5});

    std::vector<uint8_t> payload;
    result.Serialize(payload);
    REQUIRE(payload.size() ==
            ipc::FrameResult::kFixedWireSize + 2 * ipc::NalDescriptor::kWireSize +
                result.data.size());

    SECTION("round-trip")
    {
        ipc::FrameResult parsed;
        REQUIRE(ipc::FrameResult::Deserialize(payload.data(), payload.size(), parsed));
        CHECK(parsed.frameId == 42);
        CHECK(parsed.flags == ipc::kFrameResultFlagIsIdr);
        REQUIRE(parsed.nalUnits.size() == 2);
        CHECK(parsed.nalUnits[1].offset == 6);
        CHECK(parsed.nalUnits[1].length == 7);
        CHECK(parsed.nalUnits[1].type == 5);
        CHECK(parsed.data == result.data);
    }
    SECTION("descriptor past the payload end is rejected")
    {
        ipc::FrameResult bad = result;
        bad.nalUnits[1].length = 8; // 6 + 8 > 13
        payload.clear();
        bad.Serialize(payload);
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(payload.data(), payload.size(), parsed));
    }
    SECTION("descriptor offset+length overflow is rejected")
    {
        ipc::FrameResult bad = result;
        bad.nalUnits[0].offset = 0xFFFFFFF0u;
        bad.nalUnits[0].length = 0x20;
        payload.clear();
        bad.Serialize(payload);
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(payload.data(), payload.size(), parsed));
    }
    SECTION("zero-length descriptor is rejected")
    {
        ipc::FrameResult bad = result;
        bad.nalUnits[0].length = 0;
        payload.clear();
        bad.Serialize(payload);
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(payload.data(), payload.size(), parsed));
    }
    SECTION("truncated payload is rejected")
    {
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(payload.data(), payload.size() - 1, parsed));
    }
    SECTION("nal count over the cap is rejected")
    {
        // Hand-build a payload declaring kMaxNalUnits+1 descriptors.
        std::vector<uint8_t> crafted;
        ipc::PayloadWriter writer(crafted);
        writer.U32(1);          // generation
        writer.U64(1);          // frameId
        writer.I32(0);          // status
        writer.U32(0);          // flags
        writer.U64(0);          // encodeStartNs
        writer.U64(0);          // callbackAtNs
        writer.U32(ipc::kMaxNalUnits + 1);
        writer.U32(0);          // dataSize
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(crafted.data(), crafted.size(), parsed));
    }
}

#if !defined(_WIN32)

TEST_CASE("Encoder IPC socket framing over a socketpair", "[encoder-ipc]")
{
    int fds[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ipc::EncoderIpcSocket reader(fds[0]);

    SECTION("whole-message round-trip through WriteMessage")
    {
        ipc::EncoderIpcSocket writer(fds[1]);
        ipc::FrameDropped dropped;
        dropped.generation = 1;
        dropped.frameId = 77;
        dropped.reason = ipc::kDropQueueFull;
        dropped.status = -12900;
        std::vector<uint8_t> payload;
        dropped.Serialize(payload);
        REQUIRE(writer.WriteMessage(ipc::MessageType::FrameDropped, 0, 5, payload) ==
                ipc::IoResult::Ok);

        ipc::MessageHeader header;
        std::vector<uint8_t> received;
        REQUIRE(reader.ReadMessage(header, received) == ipc::IoResult::Ok);
        CHECK(header.type == (uint16_t)ipc::MessageType::FrameDropped);
        CHECK(header.sequence == 5);
        ipc::FrameDropped parsed;
        REQUIRE(ipc::FrameDropped::Deserialize(received.data(), received.size(), parsed));
        CHECK(parsed.frameId == 77);
        CHECK(parsed.status == -12900);
    }

    SECTION("split writes reassemble (partial reads)")
    {
        ipc::Drain drain;
        drain.generation = 9;
        std::vector<uint8_t> payload;
        drain.Serialize(payload);
        ipc::MessageHeader header;
        header.type = (uint16_t)ipc::MessageType::Drain;
        header.payloadSize = (uint32_t)payload.size();
        header.sequence = 1;
        std::vector<uint8_t> wire = SerializeHeader(header);
        wire.insert(wire.end(), payload.begin(), payload.end());

        // Dribble the bytes: split inside the header and inside the payload.
        std::thread producer(
            [fd = fds[1], wire]
            {
                size_t sent = 0;
                const size_t chunks[] = {5, 10, 7, 3, 100};
                for (size_t chunk : chunks)
                {
                    const size_t n = std::min(chunk, wire.size() - sent);
                    if (n == 0)
                    {
                        break;
                    }
                    REQUIRE(::write(fd, wire.data() + sent, n) == (ssize_t)n);
                    sent += n;
                    usleep(2000);
                }
                ::close(fd);
            });
        ipc::MessageHeader parsedHeader;
        std::vector<uint8_t> received;
        REQUIRE(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Ok);
        CHECK(parsedHeader.type == (uint16_t)ipc::MessageType::Drain);
        ipc::Drain parsed;
        REQUIRE(ipc::Drain::Deserialize(received.data(), received.size(), parsed));
        CHECK(parsed.generation == 9);
        // After the producer closed, the next read is EOF.
        CHECK(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Eof);
        producer.join();
    }

    SECTION("EOF mid-header is EOF, not a parse error")
    {
        ipc::MessageHeader header;
        header.type = (uint16_t)ipc::MessageType::Shutdown;
        const std::vector<uint8_t> wire = SerializeHeader(header);
        REQUIRE(::write(fds[1], wire.data(), 10) == 10); // partial header
        ::close(fds[1]);
        ipc::MessageHeader parsedHeader;
        std::vector<uint8_t> received;
        CHECK(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Eof);
    }

    SECTION("EOF mid-payload is EOF")
    {
        ipc::MessageHeader header;
        header.type = (uint16_t)ipc::MessageType::Drain;
        header.payloadSize = 4;
        const std::vector<uint8_t> wire = SerializeHeader(header);
        REQUIRE(::write(fds[1], wire.data(), wire.size()) == (ssize_t)wire.size());
        REQUIRE(::write(fds[1], "\1\2", 2) == 2); // 2 of 4 payload bytes
        ::close(fds[1]);
        ipc::MessageHeader parsedHeader;
        std::vector<uint8_t> received;
        CHECK(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Eof);
    }

    SECTION("bad magic on the stream is Invalid")
    {
        std::vector<uint8_t> wire(ipc::MessageHeader::kWireSize, 0xAB);
        REQUIRE(::write(fds[1], wire.data(), wire.size()) == (ssize_t)wire.size());
        ::close(fds[1]);
        ipc::MessageHeader parsedHeader;
        std::vector<uint8_t> received;
        CHECK(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Invalid);
    }

    SECTION("oversize declared payload is Invalid before allocation")
    {
        ipc::MessageHeader header;
        header.type = (uint16_t)ipc::MessageType::FrameResult;
        header.payloadSize = ipc::kMaxPayloadSize + 1;
        const std::vector<uint8_t> wire = SerializeHeader(header);
        REQUIRE(::write(fds[1], wire.data(), wire.size()) == (ssize_t)wire.size());
        ::close(fds[1]);
        ipc::MessageHeader parsedHeader;
        std::vector<uint8_t> received;
        CHECK(reader.ReadMessage(parsedHeader, received) == ipc::IoResult::Invalid);
    }

    // fds[0] is closed by `reader`; fds[1] by the writer socket or explicitly
    // in the section body.
}

#endif // !_WIN32
