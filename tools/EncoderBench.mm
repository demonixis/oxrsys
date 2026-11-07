// SPDX-License-Identifier: MPL-2.0
// Encoder latency bench: drives the runtime's own VideoEncoder (its real
// VideoToolbox session setup, Metal compose and colour conversion) with a
// GPU-rendered moving pattern at a fixed frame rate, and prints the
// submit->callback latency distribution the "encode queue" log line reports.
//
//   oxrsys_encoder_bench [--preset off|light|medium|medium_plus|high] [--eye 2160]
//                        [--eye-h H] [--fps 90] [--mbps 50] [--frames 900] [--submit-hz N]
//                        [--codec h265|h264] [--ten-bit] [--pattern texture|scene|smooth]
//                        [--fov centerX,centerY,edgeRatioX,edgeRatioY]
//
// It reads the runtime config from $HOME/Library/Application Support/OXRSys,
// like the runtime, so run it with HOME pointing at a scratch directory whose
// oxrsys-runtime.toml holds the encoder keys under test (encoder_low_latency,
// encoder_full_range, encoder_preset).
//
// The RESULT line also says whether the in-process VideoToolbox session is
// hardware-accelerated (hw=) and whether frames went to the encoder helper
// (helper=), because a session classified as software is fed a video-range
// 4:2:0 conversion first and its latency is not the hardware encoder's.
//
// --submit-hz spaces submissions out while timestamps still advance at --fps,
// so rate control budgets bits per frame as it does live. Sparse submission
// (e.g. 15) measures per-frame latency without adding a second full-rate
// stream to the media engine.
//
// The media engine is shared by every process on the Mac: a run beside a live
// stream measures both, and slows the live one (a 3456x1536 run at 90 Hz beside
// a live 2560x1120 stream made the live one drop frames).

#include "VideoEncoder.h"
#include "VulkanDispatch.h"
#include <Foveation.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The runtime defines this in EntryPoint.cpp; the Vulkan swapchain code links against it.
VulkanDispatch gVulkanDispatch;

namespace
{
const char* kPatternSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void pattern(texture2d<float, access::write> out [[texture(0)]],
                    constant float& t [[buffer(0)]],
                    constant int& mode [[buffer(1)]],
                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    if (mode == 2 || mode == 3)
    {
        // A simple rendered scene: smooth shading, a few hard-edged shapes,
        // mild texture, panned - what a game scene gives the encoder.
        float2 s = float2(gid) + float2(t * 140.0, t * 55.0);
        float3 c = 0.45 + 0.3 * float3(sin(s.x * 0.0021), sin(s.y * 0.0033 + 1.0), sin((s.x - s.y) * 0.0017 + 2.0));
        float2 cell = fmod(s, 360.0);
        if (length(cell - 180.0) < 90.0) c = float3(0.85, 0.4, 0.2) * (0.7 + 0.3 * (cell.y / 360.0));
        if (fmod(floor(s.x / 48.0) + floor(s.y / 48.0), 2.0) < 1.0 && cell.y > 300.0) c *= 0.6;
        uint h = uint(int(s.x)) * 1973u + uint(int(s.y)) * 9277u;
        h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        c += (float(h & 255u) / 255.0 - 0.5) * (mode == 2 ? 0.03 : 0.004);
        out.write(float4(c, 1.0), gid);
        return;
    }
    if (mode == 1)
    {
        // Fine static texture in scene space, panned: per-pixel noise, 1-px
        // text-like strokes on a 6x9 cell grid, a soft colour field. Motion
        // compensation finds it, but every frame carries real detail.
        int2 s = int2(gid) + int2(int(t * 140.0), int(t * 55.0));
        uint h = uint(s.x) * 1973u + uint(s.y) * 9277u;
        h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        float n = float(h & 255u) / 255.0;
        int cx = ((s.x % 6) + 6) % 6, cy = ((s.y % 9) + 9) % 9;
        uint g = uint(s.x / 6) * 2654435761u ^ uint(s.y / 9) * 40503u;
        bool stroke = (cy < 7) && (((g >> (cx + cy * 3)) & 1u) != 0u) && (cx < 5);
        float3 field = 0.5 + 0.35 * float3(sin(s.x * 0.004), sin(s.y * 0.006 + 1.0), sin((s.x + s.y) * 0.003 + 2.0));
        float3 c = field * (0.75 + 0.25 * n);
        if (stroke) c *= 0.25;
        out.write(float4(c, 1.0), gid);
        return;
    }
    float2 p = float2(gid) / float2(out.get_width(), out.get_height());
    float2 q = p + float2(t * 0.11, t * 0.07);
    float bars = 0.5 + 0.5 * sin((q.x * 40.0) + t * 3.0);
    float rings = 0.5 + 0.5 * sin(length(p - float2(0.5 + 0.3 * sin(t), 0.5)) * 90.0 - t * 6.0);
    float checker = fmod(floor(q.x * 24.0) + floor(q.y * 24.0), 2.0);
    uint h = gid.x * 1973u + gid.y * 9277u + uint(t * 90.0) * 26699u;
    h = (h ^ (h >> 13)) * 1274126177u;
    float noise = float(h & 255u) / 255.0;
    float3 c = float3(bars, rings, checker) * 0.8 + noise * 0.08;
    out.write(float4(c, 1.0), gid);
}
)";

std::shared_ptr<void> Retained(id object)
{
    return std::shared_ptr<void>((void*)[object retain], [](void* v) { [(id)v release]; });
}

double Pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5));
    return v[i];
}
} // namespace

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        std::string preset = "medium";
        uint32_t eye = 2160, eyeH = 0, fps = 90, mbps = 50, frames = 900, submitHz = 0;
        int patternMode = 0;
        float fovParams[4] = {0, 0, 0, 0};
        std::string codecName = "h265";
        bool tenBit = false;
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
            if (a == "--preset") preset = next();
            else if (a == "--eye") eye = std::stoul(next());
            else if (a == "--eye-h") eyeH = std::stoul(next());
            else if (a == "--fps") fps = std::stoul(next());
            else if (a == "--mbps") mbps = std::stoul(next());
            else if (a == "--frames") frames = std::stoul(next());
            else if (a == "--codec") codecName = next();
            else if (a == "--ten-bit") tenBit = true;
            else if (a == "--fov") std::sscanf(next().c_str(), "%f,%f,%f,%f", &fovParams[0], &fovParams[1], &fovParams[2], &fovParams[3]);
            else if (a == "--pattern") { std::string v = next(); patternMode = v == "texture" ? 1 : v == "scene" ? 2 : v == "smooth" ? 3 : 0; }
            else if (a == "--submit-hz") submitHz = std::stoul(next());
        }
        if (eyeH == 0) eyeH = eye;
        oxr::protocol::FoveationPreset fp = oxr::protocol::FoveationPreset::Off;
        if (preset == "light") fp = oxr::protocol::FoveationPreset::Light;
        else if (preset == "medium") fp = oxr::protocol::FoveationPreset::Medium;
        else if (preset == "high") fp = oxr::protocol::FoveationPreset::High;
        oxr::protocol::FoveationPresetParameters params = oxr::protocol::ParametersForPreset(fp);
        if (preset == "medium_plus")
        {
            fp = oxr::protocol::FoveationPreset::Medium;
            params = oxr::protocol::MediumPlusParameters();
        }
        if (fovParams[2] > 0.0f)
        {
            if (fp == oxr::protocol::FoveationPreset::Off) fp = oxr::protocol::FoveationPreset::Medium;
            params = {fovParams[0], fovParams[1], 0.0f, 0.0f, fovParams[2], fovParams[3]};
        }
        const auto layout = oxr::protocol::CalculateFoveationLayout(eye, eyeH, fp, params);
        const uint32_t encW = layout.optimizedEyeWidth * 2;
        const uint32_t encH = layout.optimizedEyeHeight;

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> renderQueue = [device newCommandQueue];
        id<MTLCommandQueue> encodeQueue = [device newCommandQueue];
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kPatternSource) options:nil error:&error];
        id<MTLComputePipelineState> pso =
            [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"pattern"] error:&error];
        if (pso == nil) { std::fprintf(stderr, "pipeline failed\n"); return 1; }

        // A small ring of eye textures, like a swapchain.
        constexpr int kRing = 3;
        id<MTLTexture> eyes[kRing][2];
        MTLTextureDescriptor* d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:eye height:eyeH mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        d.storageMode = MTLStorageModePrivate;
        for (auto& pair : eyes) { pair[0] = [device newTextureWithDescriptor:d]; pair[1] = [device newTextureWithDescriptor:d]; }

        VideoEncoder encoder;
        VideoEncoder::FoveationSettings fs = {};
        fs.enabled = fp != oxr::protocol::FoveationPreset::Off;
        fs.targetEyeWidth = layout.targetEyeWidth;
        fs.targetEyeHeight = layout.targetEyeHeight;
        fs.eyeWidthRatio = layout.eyeWidthRatio;
        fs.eyeHeightRatio = layout.eyeHeightRatio;
        fs.centerSizeX = layout.parameters.centerSizeX;
        fs.centerSizeY = layout.parameters.centerSizeY;
        fs.centerShiftX = layout.parameters.centerShiftX;
        fs.centerShiftY = layout.parameters.centerShiftY;
        fs.edgeRatioX = layout.parameters.edgeRatioX;
        fs.edgeRatioY = layout.parameters.edgeRatioY;
        encoder.SetFoveationSettings(fs);
        encoder.SetTenBitEncoding(tenBit);
        const auto codec = codecName == "h264" ? oxr::protocol::VideoCodec::H264 : oxr::protocol::VideoCodec::H265;
        if (!encoder.Initialize(encW, encH, fps, mbps, GraphicsContext::Metal((__bridge void*)device, (__bridge void*)encodeQueue), codec))
        {
            std::fprintf(stderr, "Initialize failed\n");
            return 1;
        }

        std::mutex m;
        std::vector<double> cb, total, submit, gpu;
        std::map<int64_t, size_t> bytesByTs;
        std::vector<size_t> frameBytes;
        uint32_t dropped = 0, done = 0;
        const uint32_t warmup = std::min<uint32_t>(30, frames / 10);

        const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
        // Timestamps always advance at the session rate, so rate control budgets
        // bits per frame as live; --submit-hz only spaces the submissions out.
        const auto submitPeriod = std::chrono::nanoseconds(1'000'000'000LL / (submitHz ? submitHz : fps));
        auto start = std::chrono::steady_clock::now();
        auto next = start;
        float t = 0.0f;
        for (uint32_t f = 0; f < frames; ++f)
        {
            std::this_thread::sleep_until(next);
            next += submitPeriod;
            id<MTLTexture> l = eyes[f % kRing][0], r = eyes[f % kRing][1];
            id<MTLCommandBuffer> cmd = [renderQueue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
            [ce setComputePipelineState:pso];
            for (int e = 0; e < 2; ++e)
            {
                float te = t + e * 0.01f;
                [ce setTexture:(e ? r : l) atIndex:0];
                [ce setBytes:&te length:sizeof(te) atIndex:0];
                [ce setBytes:&patternMode length:sizeof(patternMode) atIndex:1];
                [ce dispatchThreads:MTLSizeMake(eye, eyeH, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            }
            [ce endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted]; // the app's frame is finished before xrEndFrame hands it over
            t += 1.0f / fps;

            FrameSource src = {};
            for (int e = 0; e < 2; ++e)
            {
                FrameImageSource& s = e ? src.right : src.left;
                s.image = Retained(e ? r : l);
                s.sourceWidth = s.imageWidth = eye;
                s.sourceHeight = s.imageHeight = eyeH;
            }
            const int64_t ts = (int64_t)f * period.count();
            const bool measured = f >= warmup;
            encoder.EncodeStereo(std::move(src), ts,
                [&, ts](const uint8_t*, size_t size, bool, int64_t) {
                    std::lock_guard<std::mutex> lock(m);
                    bytesByTs[ts] += size;
                },
                [&, measured](const VideoEncoder::FrameMetrics& mt) {
                    std::lock_guard<std::mutex> lock(m);
                    ++done;
                    if (mt.frameDropped) { ++dropped; return; }
                    if (!measured) return;
                    cb.push_back(mt.callbackLatencyMs);
                    total.push_back(mt.totalLatencyMs);
                    submit.push_back(mt.encodeSubmitMs);
                    gpu.push_back(mt.gpuCopyMs);
                    auto it = bytesByTs.find(mt.timestampNs);
                    if (it != bytesByTs.end()) frameBytes.push_back(it->second);
                });
        }
        encoder.FlushPendingFrames();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard<std::mutex> lock(m);
        std::vector<double> kb;
        for (size_t b : frameBytes) kb.push_back(b / 1024.0);
        std::printf("RESULT %ux%u %s fps=%u hz=%u mbps=%u n=%zu cb min=%.2f p10=%.2f p50=%.2f p95=%.2f max=%.2f | total p50=%.2f p95=%.2f | submit p50=%.2f | gpu p50=%.2f | fps_out=%.1f drops=%u inflight_end=%u | KB/frame p50=%.1f (%.1f Mbps) | hw=%s helper=%s\n",
                    encW, encH, codecName.c_str(), fps, submitHz ? submitHz : fps, mbps, cb.size(), Pct(cb, 0.0), Pct(cb, 0.1), Pct(cb, 0.5), Pct(cb, 0.95), Pct(cb, 1.0),
                    Pct(total, 0.5), Pct(total, 0.95), Pct(submit, 0.5), Pct(gpu, 0.5), done / wall, dropped,
                    encoder.GetInFlightFrameCount(), Pct(kb, 0.5), Pct(kb, 0.5) * 1024 * 8 * fps / 1e6,
                    encoder.InProcessSessionUsesHardware() ? "yes" : "no", encoder.EncoderHelperPid() > 0 ? "yes" : "no");
        encoder.Shutdown();
    }
    return 0;
}
