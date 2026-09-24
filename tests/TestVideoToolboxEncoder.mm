// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "EncoderSessionColor.h"
#include "VideoEncoder.h"
#include "VuiColorTestSupport.h"

#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{

// How long a backend is given to deliver the completion for a single submitted
// frame unprompted, before the encoder is flushed instead. Hardware sessions
// and the out-of-process helper both answer in single-digit milliseconds, so
// this is ~100x their latency; it is deliberately not a "the encode failed"
// timeout, because on a software session it is simply the wrong question.
constexpr std::chrono::milliseconds kSpontaneousCompletionGrace{750};

std::shared_ptr<void> AdoptTexture(id<MTLTexture> texture)
{
    return std::shared_ptr<void>((void*)texture, [](void* value) {
        [(id)value release];
    });
}

FrameImageSource MakeSource(id<MTLDevice> device, uint32_t width, uint32_t height,
                            MTLPixelFormat pixelFormat)
{
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    REQUIRE(texture != nil);

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0xff304050u);
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:width * sizeof(uint32_t)];

    FrameImageSource source = {};
    source.image = AdoptTexture(texture);
    source.sourceWidth = width;
    source.sourceHeight = height;
    source.imageWidth = width;
    source.imageHeight = height;
    return source;
}

void EncodeOneFrame(oxr::protocol::VideoCodec codec,
                    MTLPixelFormat pixelFormat = MTLPixelFormatBGRA8Unorm,
                    bool foveated = false,
                    bool tenBit = false,
                    bool forceSoftware = false)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);

    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    // Declared ahead of the encoder so it outlives it. The callbacks below
    // capture this state by reference and the last of them can still fire from
    // the encoder's own teardown flush, which runs while these are in scope
    // only if they are destroyed after it.
    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    bool dropped = true;
    size_t nalCount = 0;
    bool annexB = true;
    std::vector<std::vector<uint8_t>> nals;

    VideoEncoder encoder;
    if (foveated)
    {
        VideoEncoder::FoveationSettings settings = {};
        settings.enabled = true;
        settings.targetEyeWidth = 64;
        settings.targetEyeHeight = 64;
        settings.eyeWidthRatio = 1.0f;
        settings.eyeHeightRatio = 1.0f;
        settings.centerSizeX = 0.5f;
        settings.centerSizeY = 0.5f;
        settings.edgeRatioX = 2.0f;
        settings.edgeRatioY = 2.0f;
        encoder.SetFoveationSettings(settings);
    }
    // HEVC Main10 is a 10-bit bitstream from the same 8-bit BGRA compose
    // surface, both in-process and in the helper; it must produce Annex-B NAL
    // units like any other profile.
    encoder.SetTenBitEncoding(tenBit);
    // The software session a Rosetta host falls back to when the helper dies,
    // pinned here so its colour contract is checked on every machine.
    encoder.SetForceSoftwareEncoderForTesting(forceSoftware);
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, codec));
    const int helperPid = encoder.EncoderHelperPid();
    if (forceSoftware)
    {
        CHECK_FALSE(encoder.InProcessSessionUsesHardware());
        CHECK(encoder.EncoderHelperPid() == -1);
    }

    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, pixelFormat);
    frame.right = MakeSource(device, 64, 64, pixelFormat);

    encoder.ForceKeyframe();
    REQUIRE(encoder.EncodeStereo(
        std::move(frame),
        1'000'000,
        [&](const uint8_t* data, size_t size, bool /*keyframe*/, int64_t /*timestampNs*/) {
            std::lock_guard<std::mutex> lock(mutex);
            ++nalCount;
            annexB = annexB && size >= 4 && data != nullptr &&
                     data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
            if (data != nullptr)
            {
                nals.emplace_back(data, data + size);
            }
        },
        [&](const VideoEncoder::FrameMetrics& metrics) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                dropped = metrics.frameDropped;
                completed = true;
            }
            ready.notify_one();
        }));

    // A software encoder holds output until the session completes, so wait on a drained
    // pipeline rather than on one frame emerging unprompted.
    encoder.FlushPendingFrames();

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, kSpontaneousCompletionGrace, [&] { return completed; }))
        {
            // A hardware session hands this lone frame back on its own within a
            // few milliseconds. A software VideoToolbox session - which is all
            // an encoder-less CI runner can offer - buffers it until the
            // compression session is flushed, and no amount of further waiting
            // will produce it. So drain the encoder and re-check: the assertion
            // is that the frame encoded, not that a particular backend chose to
            // volunteer it unprompted.
            //
            // The lock is dropped first because both callbacks take `mutex`
            // from VideoToolbox's own thread during the flush. Shutdown only
            // returns true once every one of them has run, and it is idempotent
            // (a second call short-circuits on resourcesDestroyed_), so the
            // Shutdown below stays correct.
            lock.unlock();
            encoder.Shutdown(std::chrono::seconds(5));
            lock.lock();
        }
        CHECK(completed);
        CHECK_FALSE(dropped);
        CHECK(nalCount > 0);
        CHECK(annexB);

        // Whichever process and encoder produced this (in-process hardware,
        // the helper when the policy or encoder_helper picked it, or the
        // in-process software session), the stream must carry the one BT.709
        // limited-range colour description (EncoderSessionColor.h). Range is
        // the part a software session gets wrong unaided: it writes full range
        // for a BGRA source, so it is fed a video-range 4:2:0 conversion.
        const oxrsys::test::VuiColor color = oxrsys::test::ParseVuiColor(
            nals, codec == oxr::protocol::VideoCodec::H265);
        INFO("codec " << static_cast<int>(codec) << ", ten-bit " << tenBit << ", software "
                      << forceSoftware << ", helper pid " << helperPid);
        CHECK(color.present);
        CHECK(color.primaries == oxrsys::test::CFToString(oxrsys::encoder_color::kPrimaries));
        CHECK(color.transfer ==
              oxrsys::test::CFToString(oxrsys::encoder_color::kTransferFunction));
        CHECK(color.matrix == oxrsys::test::CFToString(oxrsys::encoder_color::kYCbCrMatrix));
        CHECK_FALSE(color.fullRange);
    }

    encoder.Shutdown();
    [queue release];
    [device release];
}

TEST_CASE("VideoToolbox accepts every advertised Metal swapchain color format",
          "[video][encoder][videotoolbox][format]")
{
    constexpr MTLPixelFormat formats[] = {
        MTLPixelFormatBGRA8Unorm,
        MTLPixelFormatBGRA8Unorm_sRGB,
        MTLPixelFormatRGBA8Unorm,
        MTLPixelFormatRGBA8Unorm_sRGB,
    };
    for (const MTLPixelFormat format : formats)
    {
        INFO("Metal pixel format " << static_cast<uint64_t>(format));
        EncodeOneFrame(oxr::protocol::VideoCodec::H264, format);
    }
}

TEST_CASE("Foveated encoding converts sRGB Metal sources",
          "[video][encoder][videotoolbox][format][foveation]")
{
    EncodeOneFrame(oxr::protocol::VideoCodec::H264,
                   MTLPixelFormatRGBA8Unorm_sRGB,
                   true);
}

TEST_CASE("VideoToolbox shutdown drains submitted frame-source ownership",
          "[video][encoder][videotoolbox][lifecycle]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);

    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);
    VideoEncoder encoder;
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics,
                               oxr::protocol::VideoCodec::H264));

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weakLifetime = lifetime;
    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    frame.left.lifetime = lifetime;
    frame.right.lifetime = lifetime;
    lifetime.reset();

    REQUIRE(encoder.EncodeStereo(std::move(frame), 2'000'000,
                                 [](const uint8_t*, size_t, bool, int64_t) {}));
    REQUIRE(encoder.Shutdown(std::chrono::seconds(5)));
    CHECK(weakLifetime.expired());

    [queue release];
    [device release];
}

} // namespace

TEST_CASE("VideoToolbox encodes Metal textures with every advertised codec",
          "[video][encoder][videotoolbox]")
{
    const auto capabilities = VideoEncoder::QueryBackendCapabilities();
    REQUIRE(capabilities.backendName == "VideoToolbox");
    REQUIRE(capabilities.supportsH264);
    EncodeOneFrame(oxr::protocol::VideoCodec::H264);

    if (capabilities.supportsH265)
    {
        EncodeOneFrame(oxr::protocol::VideoCodec::H265);
        // Main10 goes down whichever encode path the policy picked, so this also
        // covers the helper when encoder_helper forces it on.
        EncodeOneFrame(oxr::protocol::VideoCodec::H265, MTLPixelFormatBGRA8Unorm,
                       /*foveated=*/false, /*tenBit=*/true);
    }
}

TEST_CASE("Software VideoToolbox sessions keep the limited-range colour contract",
          "[video][encoder][videotoolbox][color]")
{
    // Without the video-range conversion, VideoToolbox's software HEVC encoder
    // signals full range for HEVC Main, so a helper death under Rosetta would
    // flip the stream's range mid-session.
    const auto capabilities = VideoEncoder::QueryBackendCapabilities();
    REQUIRE(capabilities.supportsH264);
    EncodeOneFrame(oxr::protocol::VideoCodec::H264, MTLPixelFormatBGRA8Unorm,
                   /*foveated=*/false, /*tenBit=*/false, /*forceSoftware=*/true);
    if (capabilities.supportsH265)
    {
        EncodeOneFrame(oxr::protocol::VideoCodec::H265, MTLPixelFormatBGRA8Unorm,
                       /*foveated=*/false, /*tenBit=*/false, /*forceSoftware=*/true);
        EncodeOneFrame(oxr::protocol::VideoCodec::H265, MTLPixelFormatBGRA8Unorm,
                       /*foveated=*/false, /*tenBit=*/true, /*forceSoftware=*/true);
    }
}

TEST_CASE("VideoEncoder falls back in-process when the encoder helper dies mid-stream",
          "[video][encoder][videotoolbox][helper][lifecycle]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    std::mutex mutex;
    std::condition_variable changed;
    size_t accepted = 0;
    size_t completions = 0;
    size_t deliveredAfterKill = 0;
    size_t duplicateCompletions = 0;
    std::set<uint64_t> completedFrames;
    bool killed = false;
    std::vector<std::vector<uint8_t>> nalsAfterKill;

    VideoEncoder encoder;
    // H.265: the codec a Rosetta host is refused hardware for, so `auto` picks
    // the helper there; elsewhere this needs encoder_helper = "true".
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H265));
    const int helperPid = encoder.EncoderHelperPid();
    if (helperPid <= 0)
    {
        encoder.Shutdown();
        [queue release];
        [device release];
        SKIP("this configuration encodes in-process; nothing to kill");
    }

    auto submit = [&](int64_t timestampNs) {
        FrameSource frame = {};
        frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
        frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
        const bool ok = encoder.EncodeStereo(
            std::move(frame), timestampNs,
            [&](const uint8_t* data, size_t size, bool, int64_t) {
                std::lock_guard<std::mutex> lock(mutex);
                if (killed && data != nullptr)
                {
                    nalsAfterKill.emplace_back(data, data + size);
                }
            },
            [&](const VideoEncoder::FrameMetrics& metrics) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++completions;
                    if (!completedFrames.insert(metrics.frameNumber).second)
                    {
                        ++duplicateCompletions;
                    }
                    if (killed && !metrics.frameDropped)
                    {
                        ++deliveredAfterKill;
                    }
                }
                changed.notify_all();
            });
        std::lock_guard<std::mutex> lock(mutex);
        accepted += ok ? 1 : 0;
    };

    // Stream through the helper, then kill it (our own child, not Wine's) with
    // frames still in flight: their completions will never come, so the encoder
    // has to reclaim them, and every frame after that has to encode in-process.
    int64_t timestampNs = 1'000'000;
    for (int i = 0; i < 6; ++i, timestampNs += 16'666'667)
    {
        submit(timestampNs);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        killed = true;
    }
    REQUIRE(kill(helperPid, SIGKILL) == 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        submit(timestampNs);
        timestampNs += 16'666'667;
        std::unique_lock<std::mutex> lock(mutex);
        if (changed.wait_for(lock, std::chrono::milliseconds(20),
                             [&] { return deliveredAfterKill >= 3; }) ||
            std::chrono::steady_clock::now() > deadline)
        {
            break;
        }
    }

    CHECK(encoder.EncoderHelperPid() == -1);
    // Every slot and callback-drain lease held by a frame that was in flight to
    // the dead helper came back, or this would time out.
    CHECK(encoder.Shutdown(std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lock(mutex);
        INFO("accepted " << accepted << ", completions " << completions);
        CHECK(deliveredAfterKill >= 3);
        // Exactly one completion per frame: none lost with the helper, none
        // finalized twice (by both the death reclaim and the submit path).
        CHECK(completions >= accepted);
        CHECK(duplicateCompletions == 0);

        // The fallback keeps the stream's colour contract. Under Rosetta it is
        // the software HEVC session, which would signal full range for the
        // BGRA compose surface without its video-range conversion.
        const oxrsys::test::VuiColor color = oxrsys::test::ParseVuiColor(nalsAfterKill, true);
        INFO("in-process fallback is " << (encoder.InProcessSessionUsesHardware() ? "hardware"
                                                                                   : "software"));
        CHECK(color.present);
        CHECK(color.primaries == oxrsys::test::CFToString(oxrsys::encoder_color::kPrimaries));
        CHECK(color.matrix == oxrsys::test::CFToString(oxrsys::encoder_color::kYCbCrMatrix));
        CHECK_FALSE(color.fullRange);
    }
    [queue release];
    [device release];
}
