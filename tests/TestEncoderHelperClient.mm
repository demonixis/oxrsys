// SPDX-License-Identifier: MPL-2.0
//
// Drives EncoderHelperClient against the real native-arm64 helper binary.
//
// These cover what only a live child process can: that the host survives the
// helper vanishing (no SIGPIPE on the control socket), that the helper survives
// the host vanishing, and that the helper's bitstream carries the same BT.709
// colour description as the in-process encoder.
//
// The helper binary comes from $OXRSYS_ENCODER_HELPER_PATH (ctest sets it to
// the build's own helper). Each case SKIPs when the helper cannot come up on a
// hardware encoder here — an Intel machine cannot execute it, and a VM runner
// may expose no hardware encoder — because then there is no helper to test.

#include <catch2/catch_test_macros.hpp>

#include "EncoderHelperClient.h"
#include "EncoderSessionColor.h"
#include "VuiColorTestSupport.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace
{

constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 64;
constexpr size_t kSlots = 3;

std::string HelperPath()
{
    const char* path = std::getenv("OXRSYS_ENCODER_HELPER_PATH");
    return path != nullptr ? std::string(path) : std::string();
}

// IOSurface-backed BGRA buffers, the same shape the runtime's compose pool
// hands the helper.
struct Surfaces
{
    std::vector<CVPixelBufferRef> buffers;
    std::vector<void*> surfaces;

    Surfaces()
    {
        NSDictionary* attributes = @{
            (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        };
        for (size_t i = 0; i < kSlots; ++i)
        {
            CVPixelBufferRef buffer = nullptr;
            REQUIRE(CVPixelBufferCreate(kCFAllocatorDefault, kWidth, kHeight,
                                        kCVPixelFormatType_32BGRA,
                                        (__bridge CFDictionaryRef)attributes,
                                        &buffer) == kCVReturnSuccess);
            CVPixelBufferLockBaseAddress(buffer, 0);
            auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
            for (uint32_t y = 0; y < kHeight; ++y)
            {
                auto* row = reinterpret_cast<uint32_t*>(base + y * stride);
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    row[x] = 0xff000000u | ((x * 2u) << 16) | ((y * 4u) << 8) | (uint32_t)(i * 60u);
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
            REQUIRE(surface != nullptr);
            buffers.push_back(buffer);
            surfaces.push_back((void*)surface);
        }
    }

    ~Surfaces()
    {
        for (CVPixelBufferRef buffer : buffers)
        {
            CVPixelBufferRelease(buffer);
        }
    }
};

EncoderHelperClient::Config MakeConfig(EncoderHelperClient::Codec codec,
                                       EncoderHelperClient::Profile profile)
{
    EncoderHelperClient::Config config;
    config.width = kWidth;
    config.height = kHeight;
    config.fps = 60;
    config.bitrateMbps = 8;
    config.keyframeIntervalSec = 2;
    config.codec = codec;
    config.profile = profile;
    config.helperPath = HelperPath();
    return config;
}

// Everything the helper sends back, collected under one lock.
struct Collected
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<uint8_t>> nals;
    size_t framesDone = 0;
    size_t framesDropped = 0;
    std::atomic<int> died{0};

    template <typename Predicate>
    bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }
};

bool StartOrSkip(EncoderHelperClient& client, Surfaces& surfaces,
                 const EncoderHelperClient::Config& config, Collected& collected,
                 EncoderHelperClient::OnFrameDone onFrameDone = nullptr)
{
    client.SetDiedCallback([&collected]() {
        collected.died.fetch_add(1);
        collected.changed.notify_all();
    });
    if (!onFrameDone)
    {
        onFrameDone = [&collected](uint64_t, bool dropped, double, bool) {
            {
                std::lock_guard<std::mutex> lock(collected.mutex);
                ++collected.framesDone;
                collected.framesDropped += dropped ? 1 : 0;
            }
            collected.changed.notify_all();
        };
    }
    return client.Start(
        config, surfaces.surfaces.data(), surfaces.surfaces.size(),
        [&collected](uint64_t, const uint8_t* data, size_t size, bool, int64_t) {
            std::lock_guard<std::mutex> lock(collected.mutex);
            collected.nals.emplace_back(data, data + size);
        },
        std::move(onFrameDone));
}

} // namespace

TEST_CASE("Host survives the encoder helper dying with a write pending",
          "[video][encoder][helper][lifecycle]")
{
    if (HelperPath().empty())
    {
        SKIP("OXRSYS_ENCODER_HELPER_PATH is not set");
    }

    Surfaces surfaces;
    Collected collected;

    // Park the client's reader thread inside the first FrameDone callback. While
    // it is parked it cannot notice the helper's socket closing, so the next
    // SubmitFrame is guaranteed to write to a socket whose peer is gone: the
    // exact case that raised SIGPIPE and took the whole host (the game, under
    // Wine) down with it.
    std::mutex parkMutex;
    std::condition_variable parkChanged;
    bool parked = false;
    bool release = false;
    EncoderHelperClient::OnFrameDone parkingDone = [&](uint64_t, bool, double, bool) {
        std::unique_lock<std::mutex> lock(parkMutex);
        if (parked)
        {
            return;
        }
        parked = true;
        parkChanged.notify_all();
        parkChanged.wait(lock, [&] { return release; });
    };

    EncoderHelperClient client;
    if (!StartOrSkip(client, surfaces,
                     MakeConfig(EncoderHelperClient::Codec::H264,
                                EncoderHelperClient::Profile::Main),
                     collected, parkingDone))
    {
        SKIP("encoder helper did not come up on a hardware encoder here");
    }
    const pid_t helperPid = client.HelperPid();
    REQUIRE(helperPid > 0);

    client.SubmitFrame(1, 0, 1'000'000, true);
    {
        std::unique_lock<std::mutex> lock(parkMutex);
        REQUIRE(parkChanged.wait_for(lock, std::chrono::seconds(5), [&] { return parked; }));
    }

    // The helper is our own child, not anything of Wine's; SIGKILL is the
    // faithful model of it crashing.
    REQUIRE(kill(helperPid, SIGKILL) == 0);
    // Wait until the kernel has torn its end of the socket down. The client
    // reaps it later and treats ECHILD as already reaped.
    int status = 0;
    REQUIRE(waitpid(helperPid, &status, 0) == helperPid);
    REQUIRE(WIFSIGNALED(status));

    REQUIRE(client.IsAlive());
    client.SubmitFrame(2, 1, 2'000'000, false); // must return, not kill this process
    CHECK_FALSE(client.IsAlive());
    client.SetBitrate(4);                       // a no-op on a dead helper, and no signal

    {
        std::lock_guard<std::mutex> lock(parkMutex);
        release = true;
    }
    parkChanged.notify_all();

    // The death is reported exactly once, from the reader thread, so the owner
    // can reclaim the frames that will never complete.
    CHECK(collected.WaitFor([&] { return collected.died.load() > 0; }, std::chrono::seconds(5)));
    client.Stop();
    CHECK(collected.died.load() == 1);
}

TEST_CASE("Encoder helper exits cleanly when the host closes on frames in flight",
          "[video][encoder][helper][lifecycle]")
{
    if (HelperPath().empty())
    {
        SKIP("OXRSYS_ENCODER_HELPER_PATH is not set");
    }

    Surfaces surfaces;
    Collected collected;
    EncoderHelperClient client;
    if (!StartOrSkip(client, surfaces,
                     MakeConfig(EncoderHelperClient::Codec::H264,
                                EncoderHelperClient::Profile::Main),
                     collected))
    {
        SKIP("encoder helper did not come up on a hardware encoder here");
    }

    // Queue frames and hang up at once. The helper still has encodes in flight
    // when its read hits EOF, and flushes them while tearing down: each flushed
    // frame writes to a socket nobody reads any more, and its last log line
    // goes to a stderr pipe the host has already closed. Neither may kill it.
    for (uint32_t i = 0; i < 12; ++i)
    {
        client.SubmitFrame(100 + i, i % kSlots, (int64_t)(i + 1) * 16'666'667, i == 0);
    }
    client.Stop();

    const int status = client.HelperExitStatus();
    INFO("helper wait status " << status);
    REQUIRE(status != -1);
    CHECK_FALSE(WIFSIGNALED(status));
    CHECK(WIFEXITED(status));
    CHECK(collected.died.load() == 0);
}

namespace
{

void RequireBt709FromHelper(EncoderHelperClient::Codec codec, EncoderHelperClient::Profile profile)
{
    Surfaces surfaces;
    Collected collected;
    EncoderHelperClient client;
    if (!StartOrSkip(client, surfaces, MakeConfig(codec, profile), collected))
    {
        SKIP("encoder helper did not come up on a hardware encoder here");
    }
    client.SubmitFrame(1, 0, 1'000'000, true);
    const bool done = collected.WaitFor([&] { return collected.framesDone > 0; },
                                        std::chrono::seconds(5));
    client.Stop();
    REQUIRE(done);

    std::vector<std::vector<uint8_t>> nals;
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        nals = collected.nals;
    }
    const oxrsys::test::VuiColor color =
        oxrsys::test::ParseVuiColor(nals, codec == EncoderHelperClient::Codec::H265);
    REQUIRE(color.present);
    // The exact contract the in-process session sets (EncoderSessionColor.h).
    CHECK(color.primaries == oxrsys::test::CFToString(oxrsys::encoder_color::kPrimaries));
    CHECK(color.transfer == oxrsys::test::CFToString(oxrsys::encoder_color::kTransferFunction));
    CHECK(color.matrix == oxrsys::test::CFToString(oxrsys::encoder_color::kYCbCrMatrix));
    // Video range, as every hardware VideoToolbox encoder (in-process on
    // arm64 included) signals for a BGRA source.
    CHECK_FALSE(color.fullRange);
}

} // namespace

TEST_CASE("Encoder helper bitstream carries the BT.709 colour description",
          "[video][encoder][helper][color]")
{
    if (HelperPath().empty())
    {
        SKIP("OXRSYS_ENCODER_HELPER_PATH is not set");
    }
    SECTION("H.264 Main")
    {
        RequireBt709FromHelper(EncoderHelperClient::Codec::H264,
                               EncoderHelperClient::Profile::Main);
    }
    SECTION("H.265 Main")
    {
        RequireBt709FromHelper(EncoderHelperClient::Codec::H265,
                               EncoderHelperClient::Profile::Main);
    }
    SECTION("H.265 Main10")
    {
        RequireBt709FromHelper(EncoderHelperClient::Codec::H265,
                               EncoderHelperClient::Profile::Main10);
    }
}
