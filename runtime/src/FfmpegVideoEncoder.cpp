// SPDX-License-Identifier: MPL-2.0

#include "VideoEncoder.h"
#include "Config.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

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

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                              uint32_t bitrateMbps, void* /*graphicsDevice*/)
{
    Shutdown();

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (codec == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: no HEVC encoder available");
        return false;
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
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

    av_opt_set(context->priv_data, "preset", "ultrafast", 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(context, codec, nullptr) < 0)
    {
        spdlog::error("FFmpegVideoEncoder: failed to open HEVC encoder {}", codec->name);
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

    session_ = context;
    pixelBufferPool_ = frame;
    textureCache_ = packet;

    spdlog::info("FFmpegVideoEncoder: initialized HEVC encoder {}x{} @ {}Hz {}Mbps",
                 width_, height_, fps_, bitrateMbps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    AVCodecContext* context = CodecContext(session_);
    AVFrame* frame = Frame(pixelBufferPool_);
    AVPacket* packet = Packet(textureCache_);

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

    session_ = nullptr;
    pixelBufferPool_ = nullptr;
    textureCache_ = nullptr;
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(void* texture, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(texture, nullptr, false, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(void* leftTexture, void* rightTexture,
                                int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(leftTexture, rightTexture, true, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(void* /*leftTexture*/, void* /*rightTexture*/, bool /*stereo*/,
                                  int64_t timestampNs, OnNalUnitCallback callback,
                                  OnFrameEncodedCallback frameCallback)
{
    AVCodecContext* context = CodecContext(session_);
    AVFrame* frame = Frame(pixelBufferPool_);
    AVPacket* packet = Packet(textureCache_);
    if (context == nullptr || frame == nullptr || packet == nullptr)
    {
        return false;
    }

    auto encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);

    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;

    if (av_frame_make_writable(frame) < 0)
    {
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    }

    // TODO: replace this with a Vulkan readback path. The current Linux backend
    // proves the FFmpeg encode pipeline without blocking xrEndFrame().
    FillBlackYuv420Frame(frame);
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
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
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
            droppedFrameCount_.fetch_add(1);
            metrics.frameDropped = true;
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
    if (AVCodecContext* context = CodecContext(session_))
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
