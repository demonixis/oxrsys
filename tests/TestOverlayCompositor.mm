// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "CompositionOverlay.h"
#include "OverlayCompositor.h"

#import <Metal/Metal.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace oxrsys::runtime;
using oxrsys::video::OverlayCompositor;

namespace
{

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kEye = 200; // square eye image, so NDC (x, y) -> pixel (100 + 100x, 100 - 100y)

struct Bgra
{
    uint8_t b, g, r, a;
};

id<MTLTexture> MakeTexture(id<MTLDevice> device, MTLPixelFormat format, uint32_t w, uint32_t h,
                           const std::vector<Bgra>& pixels)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    // Shared textures are only CPU-coherent with what the GPU renders on unified memory (Apple
    // silicon); an Intel Mac reads back the bytes it uploaded. Managed works on both for the upload,
    // and ReadPixel reads through a blit instead of from the texture's CPU copy.
    d.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    REQUIRE(t != nil);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:w * 4];
    return t;
}

Bgra ReadPixel(id<MTLTexture> t, uint32_t x, uint32_t y)
{
    // Copy the pixel on the GPU into a shared buffer, so the read sees what was rendered on any Mac.
    id<MTLCommandQueue> queue = [t.device newCommandQueue];
    id<MTLBuffer> buffer = [t.device newBufferWithLength:sizeof(Bgra) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(x, y, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:sizeof(Bgra)
        destinationBytesPerImage:sizeof(Bgra)];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    Bgra p = {};
    std::memcpy(&p, buffer.contents, sizeof(p));
    [buffer release];
    [queue release];
    return p;
}

XrPosef Identity()
{
    XrPosef p = {};
    p.orientation.w = 1.0f;
    return p;
}

XrPosef At(float x, float y, float z)
{
    XrPosef p = Identity();
    p.position = {x, y, z};
    return p;
}

FrameOverlayLayer Overlay(id<MTLTexture> image, const XrFovf& fov, const XrPosef& pose)
{
    FrameOverlayLayer o = {};
    o.image.image = std::shared_ptr<void>((void*)[image retain], [](void* p) { [(id)p release]; });
    o.eyeMask = 1u;
    const glm::mat4 m = OverlayClipFromLayer(Identity(), fov, Identity(), pose);
    std::memcpy(o.clipFromLayer[0], &m[0][0], sizeof(o.clipFromLayer[0]));
    return o;
}

void Draw(id<MTLCommandQueue> queue, OverlayCompositor& compositor, id<MTLTexture> target,
          const std::vector<FrameOverlayLayer>& overlays, int expectedDrawn)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    REQUIRE(compositor.Draw((void*)cb, (void*)target, 0, overlays) == expectedDrawn);
    [cb commit];
    [cb waitUntilCompleted];
    REQUIRE(cb.status == MTLCommandBufferStatusCompleted);
}

} // namespace

TEST_CASE("A quad lands on the pixels its angles say, top row up", "[overlay][metal]")
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        OverlayCompositor compositor;

        id<MTLTexture> eye = MakeTexture(device, MTLPixelFormatBGRA8Unorm, kEye, kEye,
                                         std::vector<Bgra>(kEye * kEye, Bgra{0, 0, 0, 255}));
        // 1 x 2 image: top row red, bottom row green.
        id<MTLTexture> image = MakeTexture(device, MTLPixelFormatBGRA8Unorm, 1, 2,
                                           {Bgra{0, 0, 255, 255}, Bgra{0, 255, 0, 255}});

        // A 2 x 2 m quad 2 m ahead under a 45 degree half-fov spans NDC [-0.5, 0.5]: pixels 50..150.
        FrameOverlayLayer quad = Overlay(image, {-kPi / 4, kPi / 4, kPi / 4, -kPi / 4}, At(0, 0, -2));
        quad.shape = FrameOverlayLayer::Shape::Quad;
        quad.width = 2.0f;
        quad.height = 2.0f;
        Draw(queue, compositor, eye, {quad}, 1);

        const Bgra top = ReadPixel(eye, 100, 55);    // near the quad's top edge
        const Bgra bottom = ReadPixel(eye, 100, 145); // near its bottom edge
        CHECK(top.r > 200);
        CHECK(top.g < 60);
        CHECK(bottom.g > 200);
        CHECK(bottom.r < 60);
        // Just outside each edge stays the scene (black).
        for (auto [x, y] : {std::pair{45u, 100u}, std::pair{155u, 100u}, std::pair{100u, 45u}, std::pair{100u, 155u}})
        {
            const Bgra p = ReadPixel(eye, x, y);
            CHECK((p.r | p.g | p.b) == 0);
        }
        // Just inside each edge is the quad.
        for (auto [x, y] : {std::pair{52u, 100u}, std::pair{148u, 100u}})
        {
            const Bgra p = ReadPixel(eye, x, y);
            CHECK(p.r + p.g > 200);
        }

        // The other eye's mask: nothing drawn.
        quad.eyeMask = 2u;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        CHECK(compositor.Draw((void*)cb, (void*)eye, 0, {quad}) == 0);
        [cb commit];
        [cb waitUntilCompleted];
        [eye release];
        [image release];
        [queue release];
    }
}

TEST_CASE("Source alpha blends, its absence is opaque", "[overlay][metal]")
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        OverlayCompositor compositor;
        const XrFovf fov = {-kPi / 4, kPi / 4, kPi / 4, -kPi / 4};

        // Premultiplied white at half alpha over a blue scene: (0.5 white + 0.5 blue).
        id<MTLTexture> eye = MakeTexture(device, MTLPixelFormatBGRA8Unorm, kEye, kEye,
                                         std::vector<Bgra>(kEye * kEye, Bgra{255, 0, 0, 255}));
        id<MTLTexture> half = MakeTexture(device, MTLPixelFormatBGRA8Unorm, 1, 1, {Bgra{128, 128, 128, 128}});
        FrameOverlayLayer quad = Overlay(half, fov, At(0, 0, -2));
        quad.width = quad.height = 2.0f;
        quad.sourceAlpha = true;
        Draw(queue, compositor, eye, {quad}, 1);
        Bgra p = ReadPixel(eye, 100, 100);
        CHECK(std::abs(int(p.b) - 255) <= 2);
        CHECK(std::abs(int(p.r) - 128) <= 2);
        CHECK(std::abs(int(p.g) - 128) <= 2);

        // Same image without the source-alpha flag: opaque grey, alpha ignored.
        id<MTLTexture> eye2 = MakeTexture(device, MTLPixelFormatBGRA8Unorm, kEye, kEye,
                                          std::vector<Bgra>(kEye * kEye, Bgra{255, 0, 0, 255}));
        quad.sourceAlpha = false;
        Draw(queue, compositor, eye2, {quad}, 1);
        p = ReadPixel(eye2, 100, 100);
        CHECK(std::abs(int(p.b) - 128) <= 2);
        CHECK(std::abs(int(p.r) - 128) <= 2);
        CHECK(p.a == 255);

        [eye release];
        [eye2 release];
        [half release];
        [queue release];
    }
}

TEST_CASE("Display-encoded bytes keep their value across sRGB and UNORM formats", "[overlay][metal]")
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        OverlayCompositor compositor;
        const XrFovf fov = {-kPi / 4, kPi / 4, kPi / 4, -kPi / 4};
        const std::vector<Bgra> black(kEye * kEye, Bgra{0, 0, 0, 255});

        // UNORM image (bytes are display-encoded) into an sRGB eye: the byte must survive.
        // Without the decode it would be stored as sRGB(128/255) = 188.
        id<MTLTexture> srgbEye = MakeTexture(device, MTLPixelFormatBGRA8Unorm_sRGB, kEye, kEye, black);
        id<MTLTexture> unormImage = MakeTexture(device, MTLPixelFormatBGRA8Unorm, 1, 1, {Bgra{128, 128, 128, 255}});
        FrameOverlayLayer a = Overlay(unormImage, fov, At(0, 0, -2));
        a.width = a.height = 2.0f;
        Draw(queue, compositor, srgbEye, {a}, 1);
        CHECK(std::abs(int(ReadPixel(srgbEye, 100, 100).g) - 128) <= 1);

        // sRGB image into a UNORM eye: sampled linear, re-encoded. Without it: 55.
        id<MTLTexture> unormEye = MakeTexture(device, MTLPixelFormatBGRA8Unorm, kEye, kEye, black);
        id<MTLTexture> srgbImage = MakeTexture(device, MTLPixelFormatBGRA8Unorm_sRGB, 1, 1, {Bgra{128, 128, 128, 255}});
        FrameOverlayLayer b = Overlay(srgbImage, fov, At(0, 0, -2));
        b.width = b.height = 2.0f;
        Draw(queue, compositor, unormEye, {b}, 1);
        CHECK(std::abs(int(ReadPixel(unormEye, 100, 100).g) - 128) <= 1);

        [srgbEye release];
        [unormEye release];
        [unormImage release];
        [srgbImage release];
        [queue release];
    }
}

TEST_CASE("A cylinder spans its central angle around the viewer", "[overlay][metal]")
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        OverlayCompositor compositor;
        // 60 degree half-fov: a direction 45 degrees off-axis lands at NDC tan45/tan60 = 0.577.
        const XrFovf fov = {-kPi / 3, kPi / 3, kPi / 3, -kPi / 3};
        id<MTLTexture> eye = MakeTexture(device, MTLPixelFormatBGRA8Unorm, kEye, kEye,
                                         std::vector<Bgra>(kEye * kEye, Bgra{0, 0, 0, 255}));
        id<MTLTexture> white = MakeTexture(device, MTLPixelFormatBGRA8Unorm, 1, 1, {Bgra{255, 255, 255, 255}});

        FrameOverlayLayer cyl = Overlay(white, fov, Identity());
        cyl.shape = FrameOverlayLayer::Shape::Cylinder;
        cyl.radius = 2.0f;
        cyl.centralAngle = kPi / 2;
        cyl.height = 1.0f;
        Draw(queue, compositor, eye, {cyl}, 1);

        CHECK(ReadPixel(eye, 100, 100).r == 255);
        CHECK(ReadPixel(eye, 100 + 54, 100).r == 255); // inside 0.577
        CHECK(ReadPixel(eye, 100 + 61, 100).r == 0);   // outside
        CHECK(ReadPixel(eye, 100 - 54, 100).r == 255);
        CHECK(ReadPixel(eye, 100 - 61, 100).r == 0);
        // Height 1 m at 2 m straight ahead: half-height tan = 0.25 -> NDC 0.144, about 14 px.
        CHECK(ReadPixel(eye, 100, 100 - 12).r == 255);
        CHECK(ReadPixel(eye, 100, 100 - 17).r == 0);

        [eye release];
        [white release];
        [queue release];
    }
}
