// SPDX-License-Identifier: MPL-2.0

#include "VideoEncoder.h"
#include "Config.h"
#include "Swapchain.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

AVCodecContext* CodecContext(void* ptr)
{
    return static_cast<AVCodecContext*>(ptr);
}

AVFrame* Frame(void* ptr)
{
    return static_cast<AVFrame*>(ptr);
}

AVPacket* Packet(void* ptr)
{
    return static_cast<AVPacket*>(ptr);
}

struct FfmpegReadbackState
{
    SwsContext* eyeScaleContext = nullptr;
    SwsContext* rgbaToYuvContext = nullptr;
    std::vector<uint8_t> stereoRgba;
};

FfmpegReadbackState* ReadbackState(void* ptr)
{
    return static_cast<FfmpegReadbackState*>(ptr);
}

void DestroyReadbackState(FfmpegReadbackState* state)
{
    if (state == nullptr)
    {
        return;
    }
    if (state->eyeScaleContext != nullptr)
    {
        sws_freeContext(state->eyeScaleContext);
    }
    if (state->rgbaToYuvContext != nullptr)
    {
        sws_freeContext(state->rgbaToYuvContext);
    }
    delete state;
}

AVCodecID AvCodecId(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return AV_CODEC_ID_H264;
        case oxr::protocol::VideoCodec::H265:
            return AV_CODEC_ID_HEVC;
        case oxr::protocol::VideoCodec::AV1:
        default:
            return AV_CODEC_ID_NONE;
    }
}

const char* CodecDisplayName(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return "H.264";
        case oxr::protocol::VideoCodec::AV1:
            return "AV1";
        case oxr::protocol::VideoCodec::H265:
        default:
            return "H.265";
    }
}

const char* FfmpegErrorString(int error)
{
    static thread_local char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

std::optional<AVPixelFormat> AvPixelFormatForVulkanFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return AV_PIX_FMT_RGBA;
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return AV_PIX_FMT_BGRA;
        default:
            return std::nullopt;
    }
}

#ifdef XR_USE_GRAPHICS_API_OPENGL
std::optional<AVPixelFormat> AvPixelFormatForOpenGLReadFormat(uint64_t format)
{
    switch (static_cast<GLenum>(format))
    {
        case GL_RGBA:
            return AV_PIX_FMT_RGBA;
        case GL_BGRA:
            return AV_PIX_FMT_BGRA;
        default:
            return std::nullopt;
    }
}
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
std::optional<AVPixelFormat> AvPixelFormatForDxgiFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return AV_PIX_FMT_RGBA;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return AV_PIX_FMT_BGRA;
        default:
            return std::nullopt;
    }
}
#endif

void FillBlackYuv420Frame(AVFrame* frame)
{
    if (frame == nullptr)
    {
        return;
    }

    for (int y = 0; y < frame->height; ++y)
    {
        std::memset(frame->data[0] + y * frame->linesize[0], 16, static_cast<size_t>(frame->width));
    }
    for (int y = 0; y < frame->height / 2; ++y)
    {
        std::memset(frame->data[1] + y * frame->linesize[1], 128, static_cast<size_t>(frame->width / 2));
        std::memset(frame->data[2] + y * frame->linesize[2], 128, static_cast<size_t>(frame->width / 2));
    }
}

struct SourceRect
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::optional<SourceRect> ResolveSourceRect(const FrameImageSource& source,
                                            uint32_t imageWidth,
                                            uint32_t imageHeight)
{
    SourceRect rect = {};
    if (source.HasSourceRect())
    {
        rect.x = source.sourceX;
        rect.y = source.sourceY;
        rect.width = source.sourceWidth;
        rect.height = source.sourceHeight;
    }
    else
    {
        rect.width = imageWidth;
        rect.height = imageHeight;
    }

    const uint64_t maxX = static_cast<uint64_t>(rect.x) + rect.width;
    const uint64_t maxY = static_cast<uint64_t>(rect.y) + rect.height;
    if (rect.width == 0 || rect.height == 0 ||
        maxX > imageWidth || maxY > imageHeight)
    {
        return std::nullopt;
    }
    return rect;
}

bool ReadVulkanFrameSource(const Swapchain::VulkanFrameSource& source,
                           std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForVulkanFormat(source.format);
    if (!format.has_value() || source.stagingBuffer == VK_NULL_HANDLE ||
        source.stagingMemory == VK_NULL_HANDLE || source.mapMemory == nullptr ||
        source.unmapMemory == nullptr || source.width == 0 || source.height == 0)
    {
        return false;
    }

    const size_t readbackSize = static_cast<size_t>(source.width) * source.height * 4u;
    if (source.stagingSize < readbackSize)
    {
        return false;
    }

    if (source.fenceSubmitted && source.fence != VK_NULL_HANDLE && source.waitForFences != nullptr &&
        source.waitForFences(
            reinterpret_cast<VkDevice>(source.context.device),
            1, &source.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
    {
        return false;
    }

    void* mapped = nullptr;
    VkDevice device = reinterpret_cast<VkDevice>(source.context.device);
    if (source.mapMemory(device, source.stagingMemory, 0, readbackSize, 0, &mapped) != VK_SUCCESS)
    {
        return false;
    }
    if (source.invalidateMappedMemoryRanges != nullptr)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = source.stagingMemory;
        range.offset = 0;
        range.size = readbackSize;
        source.invalidateMappedMemoryRanges(device, 1, &range);
    }
    output.resize(readbackSize);
    std::memcpy(output.data(), mapped, readbackSize);
    source.unmapMemory(device, source.stagingMemory);
    return true;
}

#ifdef XR_USE_GRAPHICS_API_OPENGL
bool ReadOpenGLFrameSource(const Swapchain::OpenGLFrameSource& source,
                           std::vector<uint8_t>& output)
{
    if (source.width == 0 || source.height == 0 || source.pixels.empty())
    {
        return false;
    }
    output = source.pixels;
    return true;
}
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
bool ReadD3D11FrameSource(const Swapchain::D3D11FrameSource& source,
                          std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForDxgiFormat(source.format);
    if (!format.has_value() || source.immediateContext == nullptr ||
        source.stagingTexture == nullptr || source.width == 0 || source.height == 0)
    {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT result = source.immediateContext->Map(
        source.stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(source.width) * 4u;
    output.resize(rowBytes * source.height);
    const auto* srcRows = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < source.height; ++y)
    {
        std::memcpy(output.data() + rowBytes * y,
                    srcRows + static_cast<size_t>(mapped.RowPitch) * y,
                    rowBytes);
    }

    source.immediateContext->Unmap(source.stagingTexture, 0);
    return true;
}
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
bool ReadD3D12FrameSource(const Swapchain::D3D12FrameSource& source,
                          std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForDxgiFormat(source.format);
    if (!format.has_value() || source.readbackBuffer == nullptr ||
        source.width == 0 || source.height == 0 || source.totalBytes == 0)
    {
        return false;
    }

    if (source.fenceSubmitted && source.fence != nullptr &&
        source.fence->GetCompletedValue() < source.fenceValue)
    {
        if (source.fenceEvent != nullptr &&
            SUCCEEDED(source.fence->SetEventOnCompletion(source.fenceValue, source.fenceEvent)))
        {
            WaitForSingleObject(source.fenceEvent, INFINITE);
        }
        else
        {
            while (source.fence->GetCompletedValue() < source.fenceValue)
            {
                Sleep(1);
            }
        }
    }

    D3D12_RANGE readRange = {
        static_cast<SIZE_T>(source.footprint.Offset),
        static_cast<SIZE_T>(source.footprint.Offset + source.totalBytes),
    };
    void* mapped = nullptr;
    if (FAILED(source.readbackBuffer->Map(0, &readRange, &mapped)) || mapped == nullptr)
    {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(source.width) * 4u;
    output.resize(rowBytes * source.height);
    const auto* srcRows = static_cast<const uint8_t*>(mapped) + source.footprint.Offset;
    for (uint32_t y = 0; y < source.height; ++y)
    {
        std::memcpy(output.data() + rowBytes * y,
                    srcRows + static_cast<size_t>(source.footprint.Footprint.RowPitch) * y,
                    rowBytes);
    }

    D3D12_RANGE writtenRange = {0, 0};
    source.readbackBuffer->Unmap(0, &writtenRange);
    return true;
}
#endif

bool ScaleEyeToStereoRgba(FfmpegReadbackState& state,
                          AVPixelFormat sourceFormat,
                          uint32_t sourceWidth,
                          uint32_t sourceHeight,
                          const std::vector<uint8_t>& readback,
                          const SourceRect& rect,
                          uint32_t outputX,
                          uint32_t outputEyeWidth,
                          uint32_t outputHeight,
                          uint32_t outputStereoWidth)
{
    state.stereoRgba.resize(static_cast<size_t>(outputStereoWidth) * outputHeight * 4u);
    uint8_t* dstData[4] = {
        state.stereoRgba.data() + static_cast<size_t>(outputX) * 4u,
        nullptr, nullptr, nullptr,
    };
    int dstLinesize[4] = {
        static_cast<int>(outputStereoWidth * 4u),
        0, 0, 0,
    };
    const size_t sourceOffset =
        (static_cast<size_t>(rect.y) * sourceWidth + rect.x) * 4u;
    if (sourceOffset >= readback.size())
    {
        return false;
    }
    const uint8_t* srcData[4] = {readback.data() + sourceOffset, nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(sourceWidth * 4u), 0, 0, 0};

    state.eyeScaleContext = sws_getCachedContext(
        state.eyeScaleContext,
        static_cast<int>(rect.width),
        static_cast<int>(rect.height),
        sourceFormat,
        static_cast<int>(outputEyeWidth),
        static_cast<int>(outputHeight),
        AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (state.eyeScaleContext == nullptr)
    {
        return false;
    }

    return sws_scale(state.eyeScaleContext,
                     srcData,
                     srcLinesize,
                     0,
                     static_cast<int>(rect.height),
                     dstData,
                     dstLinesize) > 0;
}

bool ConvertStereoRgbaToYuv(FfmpegReadbackState& state,
                            AVFrame* frame,
                            uint32_t width,
                            uint32_t height)
{
    state.rgbaToYuvContext = sws_getCachedContext(
        state.rgbaToYuvContext,
        static_cast<int>(width),
        static_cast<int>(height),
        AV_PIX_FMT_RGBA,
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (state.rgbaToYuvContext == nullptr)
    {
        return false;
    }

    const uint8_t* srcData[4] = {state.stereoRgba.data(), nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(width * 4u), 0, 0, 0};
    return sws_scale(state.rgbaToYuvContext,
                     srcData,
                     srcLinesize,
                     0,
                     static_cast<int>(height),
                     frame->data,
                     frame->linesize) > 0;
}

bool CopyBackendFrameToAvFrame(FfmpegReadbackState& state,
                               const FrameSource& frameSource,
                               bool stereo,
                               AVFrame* frame,
                               const GraphicsContext& graphicsContext,
                               uint32_t outputWidth,
                               uint32_t outputHeight)
{
    if (graphicsContext.api == GraphicsApi::Vulkan)
    {
        auto* leftSource = static_cast<Swapchain::VulkanFrameSource*>(frameSource.left.GetImage());
        auto* rightSource = stereo
            ? static_cast<Swapchain::VulkanFrameSource*>(frameSource.right.GetImage())
            : nullptr;
        if (leftSource == nullptr || (stereo && rightSource == nullptr))
        {
            return false;
        }

        const auto leftFormat = AvPixelFormatForVulkanFormat(leftSource->format);
        const auto rightFormat = stereo
            ? AvPixelFormatForVulkanFormat(rightSource->format)
            : leftFormat;
        if (!leftFormat.has_value() || !rightFormat.has_value())
        {
            return false;
        }

        std::vector<uint8_t> leftReadback;
        std::vector<uint8_t> rightReadback;
        if (!ReadVulkanFrameSource(*leftSource, leftReadback) ||
            (stereo && !ReadVulkanFrameSource(*rightSource, rightReadback)))
        {
            return false;
        }
        if (!stereo)
        {
            rightReadback = leftReadback;
        }

        const uint32_t eyeWidth = stereo ? outputWidth / 2u : outputWidth;
        auto leftRect = ResolveSourceRect(frameSource.left, leftSource->width, leftSource->height);
        auto rightRect = stereo
            ? ResolveSourceRect(frameSource.right, rightSource->width, rightSource->height)
            : leftRect;
        if (!leftRect.has_value() || !rightRect.has_value())
        {
            return false;
        }

        state.stereoRgba.assign(static_cast<size_t>(outputWidth) * outputHeight * 4u, 0);
        if (!ScaleEyeToStereoRgba(state, *leftFormat, leftSource->width, leftSource->height,
                                  leftReadback, *leftRect, 0, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        if (stereo &&
            !ScaleEyeToStereoRgba(state, *rightFormat, rightSource->width, rightSource->height,
                                  rightReadback, *rightRect, eyeWidth, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        return ConvertStereoRgbaToYuv(state, frame, outputWidth, outputHeight);
    }

    if (graphicsContext.api == GraphicsApi::OpenGL)
    {
#ifdef XR_USE_GRAPHICS_API_OPENGL
        auto* leftSource = static_cast<Swapchain::OpenGLFrameSource*>(frameSource.left.GetImage());
        auto* rightSource = stereo
            ? static_cast<Swapchain::OpenGLFrameSource*>(frameSource.right.GetImage())
            : nullptr;
        if (leftSource == nullptr || (stereo && rightSource == nullptr))
        {
            return false;
        }
        const auto leftFormat = AvPixelFormatForOpenGLReadFormat(leftSource->format);
        const auto rightFormat = stereo
            ? AvPixelFormatForOpenGLReadFormat(rightSource->format)
            : leftFormat;
        if (!leftFormat.has_value() || !rightFormat.has_value())
        {
            return false;
        }

        std::vector<uint8_t> leftReadback;
        std::vector<uint8_t> rightReadback;
        if (!ReadOpenGLFrameSource(*leftSource, leftReadback) ||
            (stereo && !ReadOpenGLFrameSource(*rightSource, rightReadback)))
        {
            return false;
        }
        if (!stereo)
        {
            rightReadback = leftReadback;
        }

        const uint32_t eyeWidth = stereo ? outputWidth / 2u : outputWidth;
        auto leftRect = ResolveSourceRect(frameSource.left, leftSource->width, leftSource->height);
        auto rightRect = stereo
            ? ResolveSourceRect(frameSource.right, rightSource->width, rightSource->height)
            : leftRect;
        if (!leftRect.has_value() || !rightRect.has_value())
        {
            return false;
        }

        state.stereoRgba.assign(static_cast<size_t>(outputWidth) * outputHeight * 4u, 0);
        if (!ScaleEyeToStereoRgba(state, *leftFormat, leftSource->width, leftSource->height,
                                  leftReadback, *leftRect, 0, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        if (stereo &&
            !ScaleEyeToStereoRgba(state, *rightFormat, rightSource->width, rightSource->height,
                                  rightReadback, *rightRect, eyeWidth, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        return ConvertStereoRgbaToYuv(state, frame, outputWidth, outputHeight);
#else
        return false;
#endif
    }

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsContext.api == GraphicsApi::D3D11)
    {
        auto* leftSource = static_cast<Swapchain::D3D11FrameSource*>(frameSource.left.GetImage());
        auto* rightSource = stereo
            ? static_cast<Swapchain::D3D11FrameSource*>(frameSource.right.GetImage())
            : nullptr;
        if (leftSource == nullptr || (stereo && rightSource == nullptr))
        {
            return false;
        }
        const auto leftFormat = AvPixelFormatForDxgiFormat(leftSource->format);
        const auto rightFormat = stereo
            ? AvPixelFormatForDxgiFormat(rightSource->format)
            : leftFormat;
        if (!leftFormat.has_value() || !rightFormat.has_value())
        {
            return false;
        }

        std::vector<uint8_t> leftReadback;
        std::vector<uint8_t> rightReadback;
        if (!ReadD3D11FrameSource(*leftSource, leftReadback) ||
            (stereo && !ReadD3D11FrameSource(*rightSource, rightReadback)))
        {
            return false;
        }
        if (!stereo)
        {
            rightReadback = leftReadback;
        }

        const uint32_t eyeWidth = stereo ? outputWidth / 2u : outputWidth;
        auto leftRect = ResolveSourceRect(frameSource.left, leftSource->width, leftSource->height);
        auto rightRect = stereo
            ? ResolveSourceRect(frameSource.right, rightSource->width, rightSource->height)
            : leftRect;
        if (!leftRect.has_value() || !rightRect.has_value())
        {
            return false;
        }

        state.stereoRgba.assign(static_cast<size_t>(outputWidth) * outputHeight * 4u, 0);
        if (!ScaleEyeToStereoRgba(state, *leftFormat, leftSource->width, leftSource->height,
                                  leftReadback, *leftRect, 0, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        if (stereo &&
            !ScaleEyeToStereoRgba(state, *rightFormat, rightSource->width, rightSource->height,
                                  rightReadback, *rightRect, eyeWidth, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        return ConvertStereoRgbaToYuv(state, frame, outputWidth, outputHeight);
    }
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsContext.api == GraphicsApi::D3D12)
    {
        auto* leftSource = static_cast<Swapchain::D3D12FrameSource*>(frameSource.left.GetImage());
        auto* rightSource = stereo
            ? static_cast<Swapchain::D3D12FrameSource*>(frameSource.right.GetImage())
            : nullptr;
        if (leftSource == nullptr || (stereo && rightSource == nullptr))
        {
            return false;
        }
        const auto leftFormat = AvPixelFormatForDxgiFormat(leftSource->format);
        const auto rightFormat = stereo
            ? AvPixelFormatForDxgiFormat(rightSource->format)
            : leftFormat;
        if (!leftFormat.has_value() || !rightFormat.has_value())
        {
            return false;
        }

        std::vector<uint8_t> leftReadback;
        std::vector<uint8_t> rightReadback;
        if (!ReadD3D12FrameSource(*leftSource, leftReadback) ||
            (stereo && !ReadD3D12FrameSource(*rightSource, rightReadback)))
        {
            return false;
        }
        if (!stereo)
        {
            rightReadback = leftReadback;
        }

        const uint32_t eyeWidth = stereo ? outputWidth / 2u : outputWidth;
        auto leftRect = ResolveSourceRect(frameSource.left, leftSource->width, leftSource->height);
        auto rightRect = stereo
            ? ResolveSourceRect(frameSource.right, rightSource->width, rightSource->height)
            : leftRect;
        if (!leftRect.has_value() || !rightRect.has_value())
        {
            return false;
        }

        state.stereoRgba.assign(static_cast<size_t>(outputWidth) * outputHeight * 4u, 0);
        if (!ScaleEyeToStereoRgba(state, *leftFormat, leftSource->width, leftSource->height,
                                  leftReadback, *leftRect, 0, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        if (stereo &&
            !ScaleEyeToStereoRgba(state, *rightFormat, rightSource->width, rightSource->height,
                                  rightReadback, *rightRect, eyeWidth, eyeWidth, outputHeight, outputWidth))
        {
            return false;
        }
        return ConvertStereoRgbaToYuv(state, frame, outputWidth, outputHeight);
    }
#endif

    return false;
}

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                              uint32_t bitrateMbps, const GraphicsContext& graphicsContext,
                              oxr::protocol::VideoCodec codec)
{
    Shutdown();

    const AVCodecID codecId = AvCodecId(codec);
    if (codecId == AV_CODEC_ID_NONE)
    {
        spdlog::error("FFmpegVideoEncoder: {} is not implemented", CodecDisplayName(codec));
        return false;
    }

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    codec_ = codec;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    auto readbackState = std::unique_ptr<FfmpegReadbackState, decltype(&DestroyReadbackState)>(
        new FfmpegReadbackState(), DestroyReadbackState);

    const AVCodec* avCodec = avcodec_find_encoder(codecId);
    if (avCodec == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: no {} encoder available", CodecDisplayName(codec_));
        return false;
    }

    AVCodecContext* context = avcodec_alloc_context3(avCodec);
    if (context == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate codec context");
        return false;
    }

    context->width = static_cast<int>(width_);
    context->height = static_cast<int>(height_);
    context->time_base = AVRational{1, static_cast<int>(fps_)};
    context->framerate = AVRational{static_cast<int>(fps_), 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->bit_rate = static_cast<int64_t>(bitrateMbps_) * 1000 * 1000;
    context->gop_size = static_cast<int>(std::max(Config::Get().GetValues().keyframeIntervalSec * fps_, 1u));
    context->max_b_frames = 0;

    const std::string encoderPreset = Config::Get().GetValues().encoderPreset;
    const char* ffmpegPreset = "superfast";
    if (encoderPreset == "speed")
    {
        ffmpegPreset = "ultrafast";
    }
    else if (encoderPreset == "quality")
    {
        ffmpegPreset = "veryfast";
    }
    av_opt_set(context->priv_data, "preset", ffmpegPreset, 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);

    int openResult = avcodec_open2(context, avCodec, nullptr);
    if (openResult < 0)
    {
        spdlog::error("FFmpegVideoEncoder: failed to open {} encoder {}: {}",
                      CodecDisplayName(codec_), avCodec->name, FfmpegErrorString(openResult));
        avcodec_free_context(&context);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (frame == nullptr || packet == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate frame or packet");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate frame buffer");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    ffmpeg_.codecContext = context;
    ffmpeg_.frame = frame;
    ffmpeg_.packet = packet;
    ffmpeg_.readbackState = readbackState.release();

    const char* apiName = graphicsContext_.api == GraphicsApi::Vulkan
        ? "Vulkan"
        : (graphicsContext_.api == GraphicsApi::OpenGL
            ? "OpenGL"
            : (graphicsContext_.api == GraphicsApi::D3D11
                ? "D3D11"
                : (graphicsContext_.api == GraphicsApi::D3D12 ? "D3D12" : "fallback")));
    spdlog::info("FFmpegVideoEncoder: initialized {} encoder {} {}x{} @ {}Hz {}Mbps preset={} ({}) backend={}",
                 CodecDisplayName(codec_), avCodec->name, width_, height_, fps_, bitrateMbps_,
                 encoderPreset, ffmpegPreset, apiName);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    AVCodecContext* context = CodecContext(ffmpeg_.codecContext);
    AVFrame* frame = Frame(ffmpeg_.frame);
    AVPacket* packet = Packet(ffmpeg_.packet);
    auto* readbackState = ReadbackState(ffmpeg_.readbackState);

    if (context != nullptr)
    {
        avcodec_free_context(&context);
    }
    if (frame != nullptr)
    {
        av_frame_free(&frame);
    }
    if (packet != nullptr)
    {
        av_packet_free(&packet);
    }
    DestroyReadbackState(readbackState);

    ffmpeg_.codecContext = nullptr;
    ffmpeg_.frame = nullptr;
    ffmpeg_.packet = nullptr;
    ffmpeg_.readbackState = nullptr;
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool stereo,
                                  int64_t timestampNs, OnNalUnitCallback callback,
                                  OnFrameEncodedCallback frameCallback)
{
    AVCodecContext* context = CodecContext(ffmpeg_.codecContext);
    AVFrame* frame = Frame(ffmpeg_.frame);
    AVPacket* packet = Packet(ffmpeg_.packet);
    auto* readbackState = ReadbackState(ffmpeg_.readbackState);
    if (context == nullptr || frame == nullptr || packet == nullptr || readbackState == nullptr)
    {
        return false;
    }

    auto encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);

    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;

    auto dropFrame = [&](const char* reason) {
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
        if (reason != nullptr && metrics.frameNumber <= 5)
        {
            spdlog::warn("FFmpegVideoEncoder: dropping frame {}: {}", metrics.frameNumber, reason);
        }
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    };

    if (av_frame_make_writable(frame) < 0)
    {
        return dropFrame("frame not writable");
    }

    auto copyStart = Clock::now();
    if (frameSource.left.IsValid() && (!stereo || frameSource.right.IsValid()))
    {
        if (!CopyBackendFrameToAvFrame(
                *readbackState, frameSource, stereo, frame, graphicsContext_, width_, height_))
        {
            return dropFrame("backend readback/conversion failed");
        }
    }
    else if (graphicsContext_.api == GraphicsApi::Vulkan ||
             graphicsContext_.api == GraphicsApi::OpenGL ||
             graphicsContext_.api == GraphicsApi::D3D11 ||
             graphicsContext_.api == GraphicsApi::D3D12)
    {
        return dropFrame("backend frame source unavailable");
    }
    else
    {
        // Keep FFmpeg useful for encoder-only pipeline validation when a non-readback
        // backend is selected.
        FillBlackYuv420Frame(frame);
    }
    metrics.gpuCopyMs = ToMilliseconds(Clock::now() - copyStart);

    frame->pts = static_cast<int64_t>(frameCount_);
    if (forceKeyframe_.exchange(false))
    {
        frame->pict_type = AV_PICTURE_TYPE_I;
        metrics.keyframe = true;
    }
    else
    {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    auto submitStart = Clock::now();
    int sendResult = avcodec_send_frame(context, frame);
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
    if (sendResult < 0)
    {
        return dropFrame(FfmpegErrorString(sendResult));
    }

    bool emittedPacket = false;
    for (;;)
    {
        int receiveResult = avcodec_receive_packet(context, packet);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF)
        {
            break;
        }
        if (receiveResult < 0)
        {
            metrics.frameDropped = true;
            spdlog::warn("FFmpegVideoEncoder: avcodec_receive_packet failed: {}",
                         FfmpegErrorString(receiveResult));
            break;
        }

        bool packetIsKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        metrics.keyframe = metrics.keyframe || packetIsKeyframe;
        if (callback && packet->data != nullptr && packet->size > 0)
        {
            callback(packet->data, static_cast<size_t>(packet->size), packetIsKeyframe, timestampNs);
        }
        emittedPacket = true;
        av_packet_unref(packet);
    }

    frameCount_++;
    metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
    inFlightFrameCount_.fetch_sub(1);

    if (!emittedPacket)
    {
        droppedFrameCount_.fetch_add(1);
        metrics.frameDropped = true;
    }

    if (frameCallback)
    {
        frameCallback(metrics);
    }

    return emittedPacket;
}

void VideoEncoder::ForceKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
    if (AVCodecContext* context = CodecContext(ffmpeg_.codecContext))
    {
        context->bit_rate = static_cast<int64_t>(bitrateMbps_) * 1000 * 1000;
    }
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    outSlotIndex = 0;
    return false;
}

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/)
{
}

void VideoEncoder::DestroySlots()
{
}
