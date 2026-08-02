// SPDX-License-Identifier: MPL-2.0

// End-to-end smoke for the out-of-process encoder path: spawn the helper
// built in this tree (OXRSYS_ENCODER_HELPER, set by the ctest entry),
// complete the socket handshake + Mach surface rendezvous through
// NativeHelperEncoderTransport, configure a small generation, submit 3
// GPU-written IOSurface frames, and consume the encoded results.
//
// May-skip conditions (environment, not regressions): helper path not set
// (plain `oxrsys_runtime_tests` run), helper binary not arm64 (x86_64 tree —
// the transport rightly rejects a translated child), or no Metal device.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include <mach-o/loader.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "encoder/NativeHelperEncoderTransport.h"

namespace
{

constexpr uint32_t kDim = 256;

bool IsArm64MachO(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    uint32_t words[3] = {};
    const size_t read = fread(words, sizeof(uint32_t), 3, file);
    fclose(file);
    if (read != 3)
    {
        return false;
    }
    // Thin 64-bit Mach-O: magic + cputype. (The build never produces fat
    // helpers; a fat binary would skip, which is the safe direction.)
    return words[0] == MH_MAGIC_64 && (cpu_type_t)words[1] == CPU_TYPE_ARM64;
}

CVPixelBufferRef MakeSurfaceBackedBuffer()
{
    NSDictionary* attrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferCreate(nullptr, kDim, kDim, kCVPixelFormatType_32BGRA,
                            (__bridge CFDictionaryRef)attrs, &buffer) != kCVReturnSuccess)
    {
        return nullptr;
    }
    return buffer;
}

} // namespace

TEST_CASE("Encoder helper end-to-end smoke (spawn, handshake, 3 GPU frames)",
          "[encoder-helper]")
{
    const char* helperPath = getenv("OXRSYS_ENCODER_HELPER");
    if (helperPath == nullptr || helperPath[0] == '\0')
    {
        SKIP("OXRSYS_ENCODER_HELPER not set (run the oxrsys_encoder_helper_smoke ctest entry)");
    }
    if (access(helperPath, X_OK) != 0)
    {
        SKIP("helper binary not present/executable: " << helperPath);
    }
    if (!IsArm64MachO(helperPath))
    {
        SKIP("helper binary is not native arm64 (x86_64 tree) — transport would reject it");
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
    {
        SKIP("no Metal device available");
    }

    oxrsys::encoder::NativeHelperEncoderTransport transport;
    oxrsys::encoder::EncoderConfig config;
    config.width = kDim;
    config.height = kDim;
    config.fps = 72;
    config.bitrateMbps = 5;
    config.codec = oxr::protocol::VideoCodec::H264;
    config.keyframeIntervalSec = 5;
    REQUIRE(transport.Configure(config));
    REQUIRE(transport.IsHealthy());

    // 3 IOSurface-backed slots, GPU-written via a blit from a shared buffer
    // (the completed handler ordering mirrors the production compose path).
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    std::vector<uint8_t> patternBytes(kDim * kDim * 4);

    struct FrameOutcome
    {
        std::mutex mutex;
        std::condition_variable condition;
        int completed = 0;
        int encoded = 0;
        int dropped = 0;
        int idr = 0;
        bool sawValidStartCodes = true;
    } outcome;

    CVPixelBufferRef buffers[3] = {};
    for (int i = 0; i < 3; i++)
    {
        buffers[i] = MakeSurfaceBackedBuffer();
        REQUIRE(buffers[i] != nullptr);
        REQUIRE(CVPixelBufferGetIOSurface(buffers[i]) != nullptr);
    }

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:kDim
                                                          height:kDim
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;

    for (uint64_t frame = 0; frame < 3; frame++)
    {
        const int slot = (int)(frame % 3);
        // Distinct per-frame pattern.
        for (size_t i = 0; i < patternBytes.size(); i++)
        {
            patternBytes[i] = (uint8_t)((i * 31 + frame * 97) & 0xFF);
        }
        id<MTLBuffer> source = [device newBufferWithBytes:patternBytes.data()
                                                   length:patternBytes.size()
                                                  options:MTLResourceStorageModeShared];
        REQUIRE(source != nil);
        id<MTLTexture> texture =
            [device newTextureWithDescriptor:descriptor
                                   iosurface:CVPixelBufferGetIOSurface(buffers[slot])
                                       plane:0];
        REQUIRE(texture != nil);

        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:source
                   sourceOffset:0
              sourceBytesPerRow:kDim * 4
            sourceBytesPerImage:patternBytes.size()
                     sourceSize:MTLSizeMake(kDim, kDim, 1)
                      toTexture:texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted]; // GPU write done before submit

        oxrsys::encoder::FrameCallbacks callbacks;
        callbacks.onEncodedFrame =
            [&outcome](const oxrsys::encoder::EncodedFrameResult& result)
        {
            std::lock_guard<std::mutex> lock(outcome.mutex);
            outcome.encoded++;
            if (result.isIdr)
            {
                outcome.idr++;
            }
            if (result.size < 5 || result.nalUnits.empty())
            {
                outcome.sawValidStartCodes = false;
                return;
            }
            for (const auto& nal : result.nalUnits)
            {
                if (nal.offset + 4 > result.size || result.data[nal.offset] != 0 ||
                    result.data[nal.offset + 1] != 0 || result.data[nal.offset + 2] != 0 ||
                    result.data[nal.offset + 3] != 1)
                {
                    outcome.sawValidStartCodes = false;
                }
            }
        };
        callbacks.onFrameComplete =
            [&outcome](const oxrsys::encoder::EncodedFrameMetrics& metrics)
        {
            std::lock_guard<std::mutex> lock(outcome.mutex);
            outcome.completed++;
            if (metrics.frameDropped)
            {
                outcome.dropped++;
            }
            outcome.condition.notify_all();
        };
        callbacks.releaseResources = [] {};

        oxrsys::encoder::EncodedFrameMetrics seed;
        seed.frameNumber = frame;
        auto token = transport.BeginFrame(std::move(callbacks), seed);
        REQUIRE(token != nullptr);
        const int64_t ptsNs = (int64_t)frame * 13888888;
        CHECK(token->Submit(buffers[slot], ptsNs, false));

        // Wait for this frame's terminal transition before the next submit.
        std::unique_lock<std::mutex> lock(outcome.mutex);
        const bool done = outcome.condition.wait_for(
            lock, std::chrono::seconds(10),
            [&] { return outcome.completed == (int)frame + 1; });
        REQUIRE(done);
    }

    {
        std::lock_guard<std::mutex> lock(outcome.mutex);
        CHECK(outcome.completed == 3);
        CHECK(outcome.dropped == 0);
        CHECK(outcome.encoded == 3);
        CHECK(outcome.idr >= 1); // first frame of a fresh session is an IDR
        CHECK(outcome.sawValidStartCodes);
    }

    transport.Drain();
    transport.Shutdown();
    CHECK_FALSE(transport.IsHealthy());

    for (CVPixelBufferRef buffer : buffers)
    {
        CVPixelBufferRelease(buffer);
    }
}
