// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "CompositionOverlay.h"
#include "EncoderSessionColor.h"
#include "OpenXRPlatform.h"
#include "Swapchain.h"
#include "VideoEncoder.h"
#include "VuiColorTestSupport.h"
#include "VulkanDispatch.h"

#import <CoreMedia/CoreMedia.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <future>
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
                            MTLPixelFormat pixelFormat, uint32_t pixel = 0xff304050u)
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

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, pixel);
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
                    bool forceSoftware = false,
                    uint32_t sourceEyeWidth = 64,
                    uint32_t sourceEyeHeight = 64,
                    bool expectEncoded = true)
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
    frame.left = MakeSource(device, sourceEyeWidth, sourceEyeHeight, pixelFormat);
    frame.right = MakeSource(device, sourceEyeWidth, sourceEyeHeight, pixelFormat);

    encoder.ForceKeyframe();
    const bool submitted = encoder.EncodeStereo(
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
        });
    if (!expectEncoded)
    {
        // Rejected before any GPU or encoder work: the submit fails and nothing is emitted.
        CHECK_FALSE(submitted);
        CHECK(nalCount == 0);
        encoder.Shutdown();
        [queue release];
        [device release];
        return;
    }
    REQUIRE(submitted);

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

TEST_CASE("Foveated encoding accepts source eyes of another size with the target aspect",
          "[video][encoder][videotoolbox][foveation]")
{
    // The foveation kernel samples in normalized coordinates, so apps that render larger or
    // smaller than the 64x64 target eye (The Lab renders 1600x1777 for a 1512x1680 Quest eye)
    // must still encode instead of having every frame dropped.
    EncodeOneFrame(oxr::protocol::VideoCodec::H264, MTLPixelFormatBGRA8Unorm, /*foveated=*/true,
                   /*tenBit=*/false, /*forceSoftware=*/false, 128, 128);
    EncodeOneFrame(oxr::protocol::VideoCodec::H264, MTLPixelFormatBGRA8Unorm, /*foveated=*/true,
                   /*tenBit=*/false, /*forceSoftware=*/false, 32, 32);
}

TEST_CASE("Foveated encoding allows a 1% aspect mismatch and rejects more",
          "[video][encoder][videotoolbox][foveation]")
{
    // 128x129 is within 1% of the 64x64 target aspect; 128x130 is not and is dropped.
    EncodeOneFrame(oxr::protocol::VideoCodec::H264, MTLPixelFormatBGRA8Unorm, /*foveated=*/true,
                   /*tenBit=*/false, /*forceSoftware=*/false, 128, 129);
    EncodeOneFrame(oxr::protocol::VideoCodec::H264, MTLPixelFormatBGRA8Unorm, /*foveated=*/true,
                   /*tenBit=*/false, /*forceSoftware=*/false, 128, 130, /*expectEncoded=*/false);
}

TEST_CASE("Every drop before encode is counted, and logged on the first and every 450th",
          "[video][encoder][videotoolbox][foveation]")
{
    CHECK_FALSE(VideoEncoder::ShouldLogPreEncodeDrop(0));
    CHECK(VideoEncoder::ShouldLogPreEncodeDrop(1));
    CHECK_FALSE(VideoEncoder::ShouldLogPreEncodeDrop(2));
    CHECK_FALSE(VideoEncoder::ShouldLogPreEncodeDrop(449));
    CHECK(VideoEncoder::ShouldLogPreEncodeDrop(450));
    CHECK_FALSE(VideoEncoder::ShouldLogPreEncodeDrop(451));
    CHECK(VideoEncoder::ShouldLogPreEncodeDrop(900));

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);
    {
        VideoEncoder encoder;
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
        REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H264));

        // 128x130 is more than 1% off the 64x64 target aspect, so each frame is dropped before encode.
        constexpr uint32_t kFrames = 451;
        for (uint32_t i = 0; i < kFrames; ++i)
        {
            FrameSource frame = {};
            frame.left = MakeSource(device, 128, 130, MTLPixelFormatBGRA8Unorm);
            frame.right = MakeSource(device, 128, 130, MTLPixelFormatBGRA8Unorm);
            CHECK_FALSE(encoder.EncodeStereo(
                std::move(frame), 1'000'000 + i,
                [](const uint8_t*, size_t, bool, int64_t) {},
                [](const VideoEncoder::FrameMetrics&) {}));
        }
        CHECK(encoder.GetPreEncodeDropCount() == kFrames);
        encoder.Shutdown();
    }
    [queue release];
    [device release];
}

TEST_CASE("A run of frames dropped before encode does not stall the next frame",
          "[video][encoder][videotoolbox][foveation]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    auto encoder = std::make_unique<VideoEncoder>();
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
    encoder->SetFoveationSettings(settings);
    REQUIRE(encoder->Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H264));

    // 128x130 is more than 1% off the 64x64 target aspect, so each of these is dropped after the
    // frame's command buffer was made: more of them than a Metal queue's default 64 command buffers,
    // then a good frame. All of it runs on a thread we can stop waiting on.
    constexpr uint32_t kDrops = 100;
    auto done = std::make_shared<std::promise<bool>>();
    std::future<bool> result = done->get_future();
    VideoEncoder* raw = encoder.get();
    std::thread([raw, device, done] {
        bool allDropped = true;
        for (uint32_t i = 0; i < kDrops; ++i)
        {
            FrameSource frame = {};
            frame.left = MakeSource(device, 128, 130, MTLPixelFormatBGRA8Unorm);
            frame.right = MakeSource(device, 128, 130, MTLPixelFormatBGRA8Unorm);
            allDropped = !raw->EncodeStereo(std::move(frame), 1'000'000 + i,
                                            [](const uint8_t*, size_t, bool, int64_t) {},
                                            [](const VideoEncoder::FrameMetrics&) {}) && allDropped;
        }
        FrameSource frame = {};
        frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
        frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
        const bool submitted = raw->EncodeStereo(std::move(frame), 2'000'000,
                                                 [](const uint8_t*, size_t, bool, int64_t) {},
                                                 [](const VideoEncoder::FrameMetrics&) {});
        done->set_value(allDropped && submitted);
    }).detach();
    if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        // The encoder is stuck inside EncodeStereo; leave it (and its device) alive rather than
        // destroy it under that thread.
        encoder.release();
        FAIL("EncodeStereo did not return within 10 s: " << kDrops << " drops before encode, then a good frame");
    }
    CHECK(result.get());
    encoder->Shutdown();
    [queue release];
    [device release];
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

TEST_CASE("A frame superseded during its snapshot wait is not encoded",
          "[video][encoder][videotoolbox]")
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

    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    uint32_t nalCount = 0;
    uint32_t callbacks = 0;
    uint32_t supersededChecks = 0;
    CHECK_FALSE(encoder.EncodeStereo(std::move(frame), 2'000'000,
                                     [&](const uint8_t*, size_t, bool, int64_t) { ++nalCount; },
                                     [&](const VideoEncoder::FrameMetrics&) { ++callbacks; },
                                     [&] {
                                         ++supersededChecks;
                                         return true;
                                     }));
    CHECK(supersededChecks == 1);
    CHECK(callbacks == 0);
    CHECK(nalCount == 0);
    // Replaced by a newer frame, not dropped by the encoder.
    CHECK(encoder.GetDroppedFrameCount() == 0);
    REQUIRE(encoder.Shutdown(std::chrono::seconds(5)));

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

namespace
{

// Decodes one H.264 keyframe, given as Annex-B NAL units, to a BGRA pixel buffer.
CVPixelBufferRef DecodeH264Keyframe(const std::vector<std::vector<uint8_t>>& nals)
{
    std::vector<const uint8_t*> parameterSets;
    std::vector<size_t> parameterSetSizes;
    std::vector<uint8_t> sample;
    for (const std::vector<uint8_t>& nal : nals)
    {
        const uint8_t type = nal[4] & 0x1f;
        if (type == 7 || type == 8)
        {
            parameterSets.push_back(nal.data() + 4);
            parameterSetSizes.push_back(nal.size() - 4);
            continue;
        }
        const uint32_t size = static_cast<uint32_t>(nal.size() - 4);
        const uint8_t length[4] = {
            static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
            static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
        sample.insert(sample.end(), length, length + 4);
        sample.insert(sample.end(), nal.begin() + 4, nal.end());
    }

    CMVideoFormatDescriptionRef format = nullptr;
    REQUIRE(CMVideoFormatDescriptionCreateFromH264ParameterSets(
                kCFAllocatorDefault, parameterSets.size(), parameterSets.data(),
                parameterSetSizes.data(), 4, &format) == noErr);

    CMBlockBufferRef block = nullptr;
    REQUIRE(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, sample.size(),
                                               kCFAllocatorDefault, nullptr, 0, sample.size(),
                                               kCMBlockBufferAssureMemoryNowFlag, &block) == noErr);
    REQUIRE(CMBlockBufferReplaceDataBytes(sample.data(), block, 0, sample.size()) == noErr);
    CMSampleBufferRef sampleBuffer = nullptr;
    const size_t sampleSize = sample.size();
    REQUIRE(CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1,
                                      &sampleSize, &sampleBuffer) == noErr);

    NSDictionary* outputAttributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
    };
    VTDecompressionSessionRef session = nullptr;
    REQUIRE(VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr,
                                         (__bridge CFDictionaryRef)outputAttributes, nullptr,
                                         &session) == noErr);
    __block CVPixelBufferRef decoded = nullptr;
    REQUIRE(VTDecompressionSessionDecodeFrameWithOutputHandler(
                session, sampleBuffer, 0, nullptr,
                ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
                    if (status == noErr && image != nullptr)
                    {
                        decoded = CVPixelBufferRetain(image);
                    }
                }) == noErr);
    VTDecompressionSessionWaitForAsynchronousFrames(session);

    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(sampleBuffer);
    CFRelease(block);
    CFRelease(format);
    return decoded;
}

} // namespace

TEST_CASE("Eye-centred foveation centres each eye's sharp region on its own forward direction",
          "[video][encoder][videotoolbox][foveation]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    std::mutex mutex;
    std::vector<std::vector<uint8_t>> nals;

    const bool eyeCentered = GENERATE(false, true);
    CAPTURE(eyeCentered);
    VideoEncoder encoder;
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
    settings.eyeCentered = eyeCentered;
    encoder.SetFoveationSettings(settings);
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H264));

    // A column ramp shows which source column lands in each encoded column.
    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    std::vector<uint32_t> ramp(64 * 64);
    for (uint32_t y = 0; y < 64; ++y)
    {
        for (uint32_t x = 0; x < 64; ++x)
        {
            const uint32_t grey = 4 * x;
            ramp[y * 64 + x] = 0xff000000u | (grey << 16) | (grey << 8) | grey;
        }
    }
    for (FrameImageSource* eye : {&frame.left, &frame.right})
    {
        [(__bridge id<MTLTexture>)eye->image.get() replaceRegion:MTLRegionMake2D(0, 0, 64, 64)
                                                     mipmapLevel:0
                                                       withBytes:ramp.data()
                                                     bytesPerRow:64 * sizeof(uint32_t)];
    }
    // Each Quest eye sees further outwards than inwards, mirrored between the eyes.
    const float leftFov[4] = {-0.869f, 0.662f, 0.785f, -0.785f};
    const float rightFov[4] = {-0.662f, 0.869f, 0.785f, -0.785f};
    std::copy(std::begin(leftFov), std::end(leftFov), frame.left.fov);
    std::copy(std::begin(rightFov), std::end(rightFov), frame.right.fov);

    encoder.ForceKeyframe();
    REQUIRE(encoder.EncodeStereo(
        std::move(frame),
        1'000'000,
        [&](const uint8_t* data, size_t size, bool, int64_t) {
            std::lock_guard<std::mutex> lock(mutex);
            nals.emplace_back(data, data + size);
        },
        [](const VideoEncoder::FrameMetrics&) {}));
    encoder.FlushPendingFrames();
    REQUIRE(encoder.Shutdown(std::chrono::seconds(5)));

    std::lock_guard<std::mutex> lock(mutex);
    CVPixelBufferRef decoded = DecodeH264Keyframe(nals);
    REQUIRE(decoded != nullptr);
    CVPixelBufferLockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(decoded));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(decoded);
    // The source column at the centre of each encoded eye.
    const int leftCentre = base[32 * bytesPerRow + 32 * 4 + 1];
    const int rightCentre = base[32 * bytesPerRow + 96 * 4 + 1];
    CVPixelBufferUnlockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(decoded);
    CAPTURE(leftCentre, rightCentre);

    if (eyeCentered)
    {
        // The left eye looks right of its image centre and the right eye left of it, by mirrored
        // shifts, so each encoded centre samples a source column further towards the nose.
        CHECK(leftCentre > 128 + 8);
        CHECK(rightCentre < 128 - 8);
        CHECK(std::abs((leftCentre - 128) + (rightCentre - 128)) < 12);
    }
    else
    {
        CHECK(std::abs(leftCentre - rightCentre) < 8);
    }

    [queue release];
    [device release];
}

TEST_CASE("Packed alpha appends each eye's alpha below the eyes",
          "[video][encoder][videotoolbox][passthrough]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    std::mutex mutex;
    std::vector<std::vector<uint8_t>> nals;

    const bool foveated = GENERATE(false, true);
    CAPTURE(foveated);
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
    encoder.SetPackedAlpha(true);
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H264));

    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm, 0x00304050u);
    frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm, 0xff304050u);
    encoder.ForceKeyframe();
    REQUIRE(encoder.EncodeStereo(
        std::move(frame),
        1'000'000,
        [&](const uint8_t* data, size_t size, bool, int64_t) {
            std::lock_guard<std::mutex> lock(mutex);
            nals.emplace_back(data, data + size);
        },
        [](const VideoEncoder::FrameMetrics&) {}));
    encoder.FlushPendingFrames();
    REQUIRE(encoder.Shutdown(std::chrono::seconds(5)));

    std::lock_guard<std::mutex> lock(mutex);
    CVPixelBufferRef decoded = DecodeH264Keyframe(nals);
    REQUIRE(decoded != nullptr);
    CHECK(CVPixelBufferGetWidth(decoded) == 128);
    CHECK(CVPixelBufferGetHeight(decoded) == 64 + oxr::protocol::PackedAlphaRows(64));

    CVPixelBufferLockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(decoded));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(decoded);
    const size_t alphaRow = 64 + oxr::protocol::PackedAlphaRows(64) / 2;
    const uint8_t leftAlpha = base[alphaRow * bytesPerRow + 32 * 4 + 1];
    const uint8_t rightAlpha = base[alphaRow * bytesPerRow + 96 * 4 + 1];
    CVPixelBufferUnlockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(decoded);

    CHECK(leftAlpha < 16);
    CHECK(rightAlpha > 239);

    [queue release];
    [device release];
}

// The runtime defines this in EntryPoint.cpp; the Vulkan swapchain code links against it.
VulkanDispatch gVulkanDispatch;

TEST_CASE("A quad drawn before the headset connects is composited once it connects",
          "[video][encoder][videotoolbox][overlay]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);

    // The application draws its menu once into a dynamic swapchain, before any
    // headset is connected, and then submits that image with every frame.
    auto demand = std::make_shared<std::atomic_bool>(false);
    XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = MTLPixelFormatBGRA8Unorm;
    createInfo.sampleCount = 1;
    createInfo.width = 16;
    createInfo.height = 16;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    Swapchain menu(graphics, &createInfo);
    REQUIRE(menu.InitializationResult() == XR_SUCCESS);
    REQUIRE(menu.GetImageCount() > 1);
    menu.SetStreamingSnapshotDemand(demand);

    uint32_t index = 0;
    REQUIRE(menu.AcquireImage(nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    REQUIRE(menu.WaitImage(&waitInfo) == XR_SUCCESS);
    std::vector<XrSwapchainImageMetalKHR> images(menu.GetImageCount(), {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
    uint32_t imageCount = 0;
    REQUIRE(menu.EnumerateImages(static_cast<uint32_t>(images.size()), &imageCount,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) == XR_SUCCESS);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)images[index].texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
    id<MTLCommandBuffer> drawMenu = [queue commandBuffer];
    [[drawMenu renderCommandEncoderWithDescriptor:pass] endEncoding];
    [drawMenu commit];
    REQUIRE(menu.ReleaseImage(nullptr) == XR_SUCCESS);

    // Every xrEndFrame looks the overlay up this way. With no headset connected
    // it takes no snapshot.
    menu.SnapshotLastReleasedImageIfMissing();
    CHECK_FALSE(menu.GetLastReleasedFrameImageSource(0).IsValid());

    // The headset connects. The release took no snapshot, so the lookup takes it
    // now, and later frames reuse it instead of taking another staging slot.
    demand->store(true);
    menu.SnapshotLastReleasedImageIfMissing();
    const FrameImageSource menuImage = menu.GetLastReleasedFrameImageSource(0);
    REQUIRE(menuImage.IsValid());
    CHECK(menuImage.sync.kind == FrameSyncKind::MetalSharedEvent);
    CHECK(menuImage.sync.IsValid());
    menu.SnapshotLastReleasedImageIfMissing();
    CHECK(menu.GetLastReleasedFrameImageSource(0).sync.waitValue == menuImage.sync.waitValue);

    // The headset sees the menu drawn before it connected: a 1 m quad 1 m ahead,
    // under a 45 degree half-fov, covers the middle half of the left eye only.
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> nals;
    VideoEncoder encoder;
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, oxr::protocol::VideoCodec::H264));

    const float halfFov = static_cast<float>(M_PI_4);
    XrPosef eyePose = {};
    eyePose.orientation.w = 1.0f;
    XrPosef quadPose = eyePose;
    quadPose.position.z = -1.0f;
    const glm::mat4 clip = oxrsys::runtime::OverlayClipFromLayer(
        eyePose, XrFovf{-halfFov, halfFov, halfFov, -halfFov}, eyePose, quadPose);
    FrameOverlayLayer quad = {};
    quad.shape = FrameOverlayLayer::Shape::Quad;
    quad.image = menuImage;
    quad.eyeMask = 1u;
    quad.width = 1.0f;
    quad.height = 1.0f;
    std::memcpy(quad.clipFromLayer[0], &clip[0][0], sizeof(quad.clipFromLayer[0]));

    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm, 0xff000000u);
    frame.right = MakeSource(device, 64, 64, MTLPixelFormatBGRA8Unorm, 0xff000000u);
    frame.overlays.push_back(std::move(quad));
    encoder.ForceKeyframe();
    REQUIRE(encoder.EncodeStereo(
        std::move(frame),
        1'000'000,
        [&](const uint8_t* data, size_t size, bool, int64_t) {
            std::lock_guard<std::mutex> lock(mutex);
            nals.emplace_back(data, data + size);
        },
        [](const VideoEncoder::FrameMetrics&) {}));
    encoder.FlushPendingFrames();
    REQUIRE(encoder.Shutdown(std::chrono::seconds(5)));

    std::lock_guard<std::mutex> lock(mutex);
    CVPixelBufferRef decoded = DecodeH264Keyframe(nals);
    REQUIRE(decoded != nullptr);
    CVPixelBufferLockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(decoded));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(decoded);
    const uint8_t leftMiddle = base[32 * bytesPerRow + 32 * 4 + 1];
    const uint8_t leftCorner = base[4 * bytesPerRow + 4 * 4 + 1];
    const uint8_t rightMiddle = base[32 * bytesPerRow + 96 * 4 + 1];
    CVPixelBufferUnlockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(decoded);

    CHECK(leftMiddle > 239);
    CHECK(leftCorner < 16);
    CHECK(rightMiddle < 16);

    [queue release];
    [device release];
}
