// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
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

// Byte-for-byte FrameResult payload construction parameterized only on
// descriptor count: every field, and every descriptor (a real,
// safely-inside-dataSize offset/length/type triple backed by real data
// bytes), is built identically for every count. The count-cap tests below
// rely on that identity to isolate a rejection to the cap check alone —
// ruling out any other incidental malformation (missing descriptor bytes, a
// short data section, etc.) as the actual cause.
std::vector<uint8_t> BuildFrameResultPayloadWithNalCount(uint32_t nalCount,
                                                         uint32_t generation = 1,
                                                         uint64_t frameId = 1)
{
    const uint32_t dataSize = nalCount; // one data byte per descriptor
    std::vector<uint8_t> crafted;
    ipc::PayloadWriter writer(crafted);
    writer.U32(generation);
    writer.U64(frameId);
    writer.I32(0); // status
    writer.U32(0); // flags
    writer.U64(0); // encodeStartNs
    writer.U64(0); // callbackAtNs
    writer.U32(nalCount);
    writer.U32(dataSize);
    for (uint32_t i = 0; i < nalCount; i++)
    {
        writer.U32(i); // offset
        writer.U32(1); // length
    }
    std::vector<uint8_t> data(dataSize, 0xAB);
    writer.Bytes(data.data(), data.size());
    return crafted;
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
    result.nalUnits.push_back({0, 6});
    result.nalUnits.push_back({6, 7});

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
        // Positive control: EXACTLY at the cap must deserialize
        // successfully, with every field/descriptor intact — proving the
        // cap check is an off-by-one-correct boundary, not an overly eager
        // rejection that happens to also catch kMaxNalUnits+1.
        const std::vector<uint8_t> atCap =
            BuildFrameResultPayloadWithNalCount(ipc::kMaxNalUnits);
        ipc::FrameResult parsedAtCap;
        REQUIRE(ipc::FrameResult::Deserialize(atCap.data(), atCap.size(), parsedAtCap));
        REQUIRE(parsedAtCap.nalUnits.size() == ipc::kMaxNalUnits);
        CHECK(parsedAtCap.nalUnits.front().offset == 0);
        CHECK(parsedAtCap.nalUnits.back().offset == ipc::kMaxNalUnits - 1);
        CHECK(parsedAtCap.data.size() == ipc::kMaxNalUnits);

        // Negative: identical shape, one descriptor further — only the
        // count differs from the passing case above.
        const std::vector<uint8_t> overCap =
            BuildFrameResultPayloadWithNalCount(ipc::kMaxNalUnits + 1);
        ipc::FrameResult parsedOverCap;
        CHECK_FALSE(ipc::FrameResult::Deserialize(overCap.data(), overCap.size(), parsedOverCap));
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

    SECTION("coalesced reads: two messages in one write() yield intact via sequential reads")
    {
        // Two complete, differently-sized messages land in a single write();
        // ReadMessage must not consume past its own message even when more
        // bytes are already sitting in the socket buffer.
        ipc::Drain drain;
        drain.generation = 9;
        std::vector<uint8_t> drainPayload;
        drain.Serialize(drainPayload);
        ipc::MessageHeader drainHeader;
        drainHeader.type = (uint16_t)ipc::MessageType::Drain;
        drainHeader.payloadSize = (uint32_t)drainPayload.size();
        drainHeader.sequence = 1;
        std::vector<uint8_t> wire = SerializeHeader(drainHeader);
        wire.insert(wire.end(), drainPayload.begin(), drainPayload.end());

        ipc::FrameDropped dropped;
        dropped.generation = 1;
        dropped.frameId = 77;
        dropped.reason = ipc::kDropQueueFull;
        dropped.status = -12900;
        std::vector<uint8_t> droppedPayload;
        dropped.Serialize(droppedPayload);
        ipc::MessageHeader droppedHeader;
        droppedHeader.type = (uint16_t)ipc::MessageType::FrameDropped;
        droppedHeader.payloadSize = (uint32_t)droppedPayload.size();
        droppedHeader.sequence = 2;
        std::vector<uint8_t> droppedWire = SerializeHeader(droppedHeader);
        droppedWire.insert(droppedWire.end(), droppedPayload.begin(), droppedPayload.end());

        REQUIRE(drainPayload.size() != droppedPayload.size());

        wire.insert(wire.end(), droppedWire.begin(), droppedWire.end());
        REQUIRE(::write(fds[1], wire.data(), wire.size()) == (ssize_t)wire.size());
        ::close(fds[1]);

        ipc::MessageHeader firstHeader;
        std::vector<uint8_t> firstReceived;
        REQUIRE(reader.ReadMessage(firstHeader, firstReceived) == ipc::IoResult::Ok);
        CHECK(firstHeader.type == (uint16_t)ipc::MessageType::Drain);
        CHECK(firstHeader.sequence == 1);
        ipc::Drain parsedDrain;
        REQUIRE(ipc::Drain::Deserialize(firstReceived.data(), firstReceived.size(), parsedDrain));
        CHECK(parsedDrain.generation == 9);

        ipc::MessageHeader secondHeader;
        std::vector<uint8_t> secondReceived;
        REQUIRE(reader.ReadMessage(secondHeader, secondReceived) == ipc::IoResult::Ok);
        CHECK(secondHeader.type == (uint16_t)ipc::MessageType::FrameDropped);
        CHECK(secondHeader.sequence == 2);
        ipc::FrameDropped parsedDropped;
        REQUIRE(ipc::FrameDropped::Deserialize(secondReceived.data(), secondReceived.size(),
                                               parsedDropped));
        CHECK(parsedDropped.frameId == 77);
        CHECK(parsedDropped.status == -12900);
    }

    SECTION("oversized NAL count is legally framed but FrameResult::Deserialize rejects it")
    {
        // Build a FrameResult payload carrying kMaxNalUnits+1 *valid-looking*
        // descriptors (each safely inside dataSize, so descriptor-arithmetic
        // checks alone would pass them) with the whole message kept far under
        // kMaxPayloadSize. That means ValidateHeader and ReadMessage must
        // accept the framing as legal; only FrameResult::Deserialize's own
        // count check may reject it.
        const std::vector<uint8_t> crafted =
            BuildFrameResultPayloadWithNalCount(ipc::kMaxNalUnits + 1, 3, 99);
        REQUIRE(crafted.size() < ipc::kMaxPayloadSize);

        ipc::EncoderIpcSocket writer(fds[1]);
        REQUIRE(writer.WriteMessage(ipc::MessageType::FrameResult, 0, 11, crafted) ==
                ipc::IoResult::Ok);

        // The framing itself is legal: ReadMessage must accept it.
        ipc::MessageHeader header;
        std::vector<uint8_t> received;
        REQUIRE(reader.ReadMessage(header, received) == ipc::IoResult::Ok);
        CHECK(header.type == (uint16_t)ipc::MessageType::FrameResult);
        REQUIRE(received == crafted);

        // Only Deserialize's descriptor-count validation can catch this.
        ipc::FrameResult parsed;
        CHECK_FALSE(ipc::FrameResult::Deserialize(received.data(), received.size(), parsed));
    }

    SECTION("tiny SO_SNDBUF forces writev to resume across partial writes")
    {
        // Shrink the send buffer well below the payload size so
        // WriteMessage's partial-write resume loop cannot complete in one
        // call and must resume across partial writes while a concurrent
        // reader drains the other end.
        int sndbuf = 4096;
        REQUIRE(setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);

        ipc::FrameResult result;
        result.generation = 5;
        result.frameId = 4242;
        result.flags = ipc::kFrameResultFlagIsIdr;
        result.encodeStartNs = 10;
        result.callbackAtNs = 20;
        const size_t kDataSize = 128 * 1024;
        result.data.resize(kDataSize);
        for (size_t i = 0; i < kDataSize; i++)
        {
            result.data[i] = (uint8_t)((i * 37 + 11) & 0xFF); // patterned, not zero-filled
        }
        result.nalUnits.push_back({0, (uint32_t)(kDataSize / 3)});
        result.nalUnits.push_back({(uint32_t)(kDataSize / 3), (uint32_t)(kDataSize / 3)});
        result.nalUnits.push_back(
            {(uint32_t)(2 * kDataSize / 3), (uint32_t)(kDataSize - 2 * kDataSize / 3)});

        std::vector<uint8_t> payload;
        result.Serialize(payload);
        // Sanity: the payload must genuinely exceed the shrunk send buffer,
        // otherwise this test would not exercise the partial-write path.
        REQUIRE(payload.size() > 64u * 1024u);

        // Everything the reader thread touches lives in a heap-allocated,
        // shared_ptr'd struct captured BY VALUE: if the reader hangs, the
        // watchdog below detaches (rather than joins) the thread, and a
        // detached thread must never keep writing into this SECTION's stack
        // after the SECTION returns.
        struct ReaderOutcome
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
            ipc::IoResult result = ipc::IoResult::Error;
            ipc::MessageHeader header{};
            std::vector<uint8_t> received;
        };
        auto outcome = std::make_shared<ReaderOutcome>();
        std::thread readerThread(
            [&reader, outcome]
            {
                // ReadMessage drains the socket as bytes arrive.
                ipc::MessageHeader header;
                std::vector<uint8_t> received;
                const ipc::IoResult r = reader.ReadMessage(header, received);
                {
                    std::lock_guard<std::mutex> lock(outcome->mutex);
                    outcome->result = r;
                    outcome->header = header;
                    outcome->received = std::move(received);
                    outcome->done = true;
                }
                outcome->condition.notify_one();
            });

        ipc::EncoderIpcSocket writer(fds[1]);
        const ipc::IoResult writeResult =
            writer.WriteMessage(ipc::MessageType::FrameResult, 0, 21, payload);
        CHECK(writeResult == ipc::IoResult::Ok);

        // Generous timeout guard: proves no deadlock rather than hanging the
        // whole suite if the writev/ReadMessage pairing regresses. On
        // timeout, detach rather than join — mirrors RunWithWatchdog's
        // detach-on-timeout semantics (TestNativeHelperChaos.mm) so a
        // genuinely hung reader can never hang this test process.
        bool finishedInTime = false;
        {
            std::unique_lock<std::mutex> lock(outcome->mutex);
            finishedInTime = outcome->condition.wait_for(lock, std::chrono::seconds(15),
                                                          [&] { return outcome->done; });
        }
        if (finishedInTime)
        {
            readerThread.join();
        }
        else
        {
            readerThread.detach();
        }
        REQUIRE(finishedInTime);

        CHECK(outcome->result == ipc::IoResult::Ok);
        CHECK(outcome->header.type == (uint16_t)ipc::MessageType::FrameResult);
        CHECK(outcome->header.sequence == 21);
        REQUIRE(outcome->received == payload); // byte-exact reassembly

        ipc::FrameResult parsed;
        REQUIRE(
            ipc::FrameResult::Deserialize(outcome->received.data(), outcome->received.size(), parsed));
        CHECK(parsed.data == result.data); // pattern survived the partial writes intact
        REQUIRE(parsed.nalUnits.size() == 3);
        CHECK(parsed.nalUnits[2].offset == (uint32_t)(2 * kDataSize / 3));
        CHECK(parsed.nalUnits[2].length == (uint32_t)(kDataSize - 2 * kDataSize / 3));
    }

    // fds[0] is closed by `reader`; fds[1] by the writer socket or explicitly
    // in the section body.
}

#endif // !_WIN32
