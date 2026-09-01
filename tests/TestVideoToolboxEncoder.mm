// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "VideoEncoder.h"

#import <Metal/Metal.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace
{

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
                    bool foveated = false)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);

    GraphicsContext graphics = GraphicsContext::Metal((__bridge void*)device,
                                                       (__bridge void*)queue);
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
    REQUIRE(encoder.Initialize(128, 64, 60, 8, graphics, codec));

    FrameSource frame = {};
    frame.left = MakeSource(device, 64, 64, pixelFormat);
    frame.right = MakeSource(device, 64, 64, pixelFormat);

    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    bool dropped = true;
    size_t nalCount = 0;
    bool annexB = true;

    encoder.ForceKeyframe();
    REQUIRE(encoder.EncodeStereo(
        std::move(frame),
        1'000'000,
        [&](const uint8_t* data, size_t size, bool /*keyframe*/, int64_t /*timestampNs*/) {
            std::lock_guard<std::mutex> lock(mutex);
            ++nalCount;
            annexB = annexB && size >= 4 && data != nullptr &&
                     data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
        },
        [&](const VideoEncoder::FrameMetrics& metrics) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                dropped = metrics.frameDropped;
                completed = true;
            }
            ready.notify_one();
        }));

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
        CHECK_FALSE(dropped);
        CHECK(nalCount > 0);
        CHECK(annexB);
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
    }
}
