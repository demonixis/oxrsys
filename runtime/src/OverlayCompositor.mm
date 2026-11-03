// SPDX-License-Identifier: MPL-2.0

#include "OverlayCompositor.h"
#include "VideoTextureFormat.h"

#import <Metal/Metal.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oxrsys::video
{
namespace
{

// Must match OverlayUniforms in the shader below.
struct OverlayUniforms
{
    float clipFromLayer[16];
    float uvRect[4];      // u0, v0, du, dv in normalized texture coordinates
    float shapeParams[4]; // quad: width, height; cylinder: radius, centralAngle, height
    uint32_t shape;       // 0 quad, 1 cylinder
    uint32_t columns;     // tessellation across s
    uint32_t colorMode;   // 0 none, 1 decode sRGB -> linear, 2 encode linear -> sRGB
    uint32_t alphaMode;   // 0 opaque, 1 premultiplied, 2 unpremultiplied
};

const char* kOverlayShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct OverlayUniforms
{
    float4x4 clipFromLayer;
    float4 uvRect;
    float4 shapeParams;
    uint shape;
    uint columns;
    uint colorMode;
    uint alphaMode;
};

struct OverlayVertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex OverlayVertexOut overlay_vertex(uint vid [[vertex_id]],
                                       constant OverlayUniforms& u [[buffer(0)]])
{
    // Each column of the surface is two triangles.
    const uint2 corners[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1),
                              uint2(1, 0), uint2(1, 1), uint2(0, 1)};
    const uint column = vid / 6;
    const uint2 corner = corners[vid % 6];
    const float s = (float(column) + float(corner.x)) / float(u.columns);
    const float t = float(corner.y);

    float3 p;
    if (u.shape == 0)
    {
        p = float3((s - 0.5) * u.shapeParams.x, (0.5 - t) * u.shapeParams.y, 0.0);
    }
    else
    {
        const float theta = (s - 0.5) * u.shapeParams.y;
        p = float3(u.shapeParams.x * sin(theta), (0.5 - t) * u.shapeParams.z,
                   -u.shapeParams.x * cos(theta));
    }

    OverlayVertexOut out;
    out.position = u.clipFromLayer * float4(p, 1.0);
    out.uv = u.uvRect.xy + float2(s, t) * u.uvRect.zw;
    return out;
}

static float3 srgb_to_linear(float3 c)
{
    c = clamp(c, 0.0, 1.0);
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

static float3 linear_to_srgb(float3 c)
{
    c = clamp(c, 0.0, 1.0);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);
}

fragment float4 overlay_fragment(OverlayVertexOut in [[stage_in]],
                                 texture2d<float> image [[texture(0)]],
                                 sampler imageSampler [[sampler(0)]],
                                 constant OverlayUniforms& u [[buffer(0)]])
{
    float4 c = image.sample(imageSampler, in.uv);
    if (u.alphaMode == 0)
    {
        c.a = 1.0;
    }
    // Convert the straight colour, then premultiply for the One/OneMinusSrcAlpha blend.
    float3 rgb = c.rgb;
    if (u.alphaMode == 1)
    {
        rgb = c.a > 0.0 ? rgb / c.a : float3(0.0);
    }
    if (u.colorMode == 1)
    {
        rgb = srgb_to_linear(rgb);
    }
    else if (u.colorMode == 2)
    {
        rgb = linear_to_srgb(rgb);
    }
    return float4(rgb * c.a, c.a);
}
)METAL";

// Formats whose samples come back as linear light: sRGB formats are decoded by
// the sampler and float formats hold linear values. Plain UNORM formats are
// treated as already display-encoded, which is how the encoder treats the eye
// images too (VideoTextureFormat.h: BGRA8Unorm goes to the encoder as is).
bool SamplesAsLinear(MTLPixelFormat format)
{
    switch (format)
    {
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA16Float:
        case MTLPixelFormatRGBA32Float:
        case MTLPixelFormatRG11B10Float:
        case MTLPixelFormatRGB9E5Float:
        case MTLPixelFormatBGR10_XR_sRGB:
        case MTLPixelFormatBGRA10_XR_sRGB:
            return true;
        default:
            return false;
    }
}

uint32_t CylinderColumns(float centralAngle)
{
    // About 2.8 degrees per column: under a pixel of sagitta for any radius we draw.
    const float columns = std::ceil(centralAngle / (2.0f * static_cast<float>(M_PI)) * 128.0f);
    return static_cast<uint32_t>(std::clamp(columns, 8.0f, 128.0f));
}

} // namespace

OverlayCompositor::~OverlayCompositor()
{
    for (auto& entry : pipelines_)
    {
        [(id<MTLRenderPipelineState>)entry.second release];
    }
    pipelines_.clear();
    if (sampler_ != nullptr)
    {
        [(id<MTLSamplerState>)sampler_ release];
        sampler_ = nullptr;
    }
    if (library_ != nullptr)
    {
        [(id<MTLLibrary>)library_ release];
        library_ = nullptr;
    }
}

void* OverlayCompositor::PipelineFor(void* deviceHandle, uint64_t pixelFormat)
{
    id<MTLDevice> device = (id<MTLDevice>)deviceHandle;
    if (device_ != nullptr && device_ != deviceHandle)
    {
        // A different device (encoder re-created on another GPU): start over.
        for (auto& entry : pipelines_)
        {
            [(id<MTLRenderPipelineState>)entry.second release];
        }
        pipelines_.clear();
        if (sampler_ != nullptr)
        {
            [(id<MTLSamplerState>)sampler_ release];
            sampler_ = nullptr;
        }
        if (library_ != nullptr)
        {
            [(id<MTLLibrary>)library_ release];
            library_ = nullptr;
        }
        failed_ = false;
    }
    device_ = deviceHandle;
    if (failed_)
    {
        return nullptr;
    }

    auto existing = pipelines_.find(pixelFormat);
    if (existing != pipelines_.end())
    {
        return existing->second;
    }

    if (library_ == nullptr)
    {
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:kOverlayShaderSource];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (library == nil)
        {
            spdlog::error("OverlayCompositor: failed to compile the overlay shader: {}",
                          error != nil ? error.localizedDescription.UTF8String : "unknown error");
            failed_ = true;
            return nullptr;
        }
        library_ = (void*)library;

        MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
        samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_ = (void*)[device newSamplerStateWithDescriptor:samplerDesc];
        [samplerDesc release];
        if (sampler_ == nullptr)
        {
            spdlog::error("OverlayCompositor: failed to create the overlay sampler");
            failed_ = true;
            return nullptr;
        }
    }

    id<MTLLibrary> library = (id<MTLLibrary>)library_;
    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"overlay_vertex"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"overlay_fragment"];
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vertexFunction;
    desc.fragmentFunction = fragmentFunction;
    MTLRenderPipelineColorAttachmentDescriptor* color = desc.colorAttachments[0];
    color.pixelFormat = (MTLPixelFormat)pixelFormat;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    [desc release];
    [vertexFunction release];
    [fragmentFunction release];
    if (pipeline == nil)
    {
        spdlog::error("OverlayCompositor: failed to create the overlay pipeline for format {}: {}",
                      pixelFormat, error != nil ? error.localizedDescription.UTF8String : "unknown error");
        failed_ = true;
        return nullptr;
    }
    pipelines_[pixelFormat] = (void*)pipeline;
    spdlog::info("OverlayCompositor: overlay pipeline ready for format {}", pixelFormat);
    return (void*)pipeline;
}

int OverlayCompositor::Draw(void* commandBufferHandle, void* targetHandle, uint32_t eyeIndex,
                            const std::vector<FrameOverlayLayer>& overlays)
{
    if (!AnyVisibleTo(overlays, eyeIndex))
    {
        return 0;
    }
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)commandBufferHandle;
    id<MTLTexture> target = (id<MTLTexture>)targetHandle;
    if (commandBuffer == nil || target == nil)
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    id<MTLRenderPipelineState> pipeline =
        (id<MTLRenderPipelineState>)PipelineFor((void*)commandBuffer.device, (uint64_t)target.pixelFormat);
    if (pipeline == nil)
    {
        return -1;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil)
    {
        return -1;
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFragmentSamplerState:(id<MTLSamplerState>)sampler_ atIndex:0];

    const bool targetIsLinear = IsSrgbTextureFormat(static_cast<uint64_t>(target.pixelFormat));
    int drawn = 0;
    for (const auto& overlay : overlays)
    {
        if ((overlay.eyeMask & (1u << eyeIndex)) == 0)
        {
            continue;
        }
        id<MTLTexture> image = (id<MTLTexture>)overlay.image.GetImage();
        if (image == nil || image.width == 0 || image.height == 0)
        {
            continue;
        }

        OverlayUniforms u = {};
        std::memcpy(u.clipFromLayer, overlay.clipFromLayer[eyeIndex], sizeof(u.clipFromLayer));
        const float imageWidth = static_cast<float>(image.width);
        const float imageHeight = static_cast<float>(image.height);
        if (overlay.image.HasSourceRect())
        {
            u.uvRect[0] = static_cast<float>(overlay.image.sourceX) / imageWidth;
            u.uvRect[1] = static_cast<float>(overlay.image.sourceY) / imageHeight;
            u.uvRect[2] = static_cast<float>(overlay.image.sourceWidth) / imageWidth;
            u.uvRect[3] = static_cast<float>(overlay.image.sourceHeight) / imageHeight;
        }
        else
        {
            u.uvRect[2] = 1.0f;
            u.uvRect[3] = 1.0f;
        }
        if (overlay.shape == FrameOverlayLayer::Shape::Quad)
        {
            u.shape = 0;
            u.columns = 1;
            u.shapeParams[0] = overlay.width;
            u.shapeParams[1] = overlay.height;
        }
        else
        {
            u.shape = 1;
            u.columns = CylinderColumns(overlay.centralAngle);
            u.shapeParams[0] = overlay.radius;
            u.shapeParams[1] = overlay.centralAngle;
            u.shapeParams[2] = overlay.height;
        }
        const bool imageIsLinear = SamplesAsLinear(image.pixelFormat);
        u.colorMode = imageIsLinear == targetIsLinear ? 0u : (targetIsLinear ? 1u : 2u);
        u.alphaMode = !overlay.sourceAlpha ? 0u : (overlay.unpremultipliedAlpha ? 2u : 1u);

        [encoder setVertexBytes:&u length:sizeof(u) atIndex:0];
        [encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [encoder setFragmentTexture:image atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:u.columns * 6];
        ++drawn;
    }
    [encoder endEncoding];
    return drawn;
}

} // namespace oxrsys::video
