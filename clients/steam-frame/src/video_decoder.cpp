// SPDX-License-Identifier: BSL-1.0
#include "video_decoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define LOG_ERR(fmt, ...) fprintf(stderr, "[ERROR] VideoDecoder: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stderr, "[INFO] VideoDecoder: " fmt "\n", ##__VA_ARGS__)

VideoDecoder::~VideoDecoder() { Close(); }

// ---- shared helpers --------------------------------------------------------

// Convert the current decoded frame_ to tightly-packed RGBA into `out`.
static bool frameToRGBA(SwsContext*& sws, AVFrame* frame, std::vector<uint8_t>& out)
{
    sws = sws_getCachedContext(sws, frame->width, frame->height,
                               (AVPixelFormat)frame->format,
                               frame->width, frame->height, AV_PIX_FMT_RGBA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    out.resize((size_t)frame->width * frame->height * 4);
    uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {frame->width * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    return true;
}

// ---- file mode -------------------------------------------------------------

bool VideoDecoder::openInternal()
{
    if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) != 0) {
        LOG_ERR("cannot open %s", path_.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) { LOG_ERR("no stream info"); return false; }
    videoStream_ = -1;
    for (unsigned i = 0; i < fmt_->nb_streams; i++)
        if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStream_ = (int)i; break; }
    if (videoStream_ < 0) { LOG_ERR("no video stream"); return false; }

    AVCodecParameters* par = fmt_->streams[videoStream_]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { LOG_ERR("no decoder for codec %d", par->codec_id); return false; }
    codec_ = avcodec_alloc_context3(dec);
    if (!codec_ || avcodec_parameters_to_context(codec_, par) < 0) { LOG_ERR("ctx init"); return false; }
    if (avcodec_open2(codec_, dec, nullptr) < 0) { LOG_ERR("avcodec_open2"); return false; }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && packet_;
}

bool VideoDecoder::Open(const std::string& path)
{
    streamMode_ = false;
    path_ = path;
    if (!openInternal()) { closeInternal(); return false; }
    LOG_INF("opened %s (%s)", path.c_str(), avcodec_get_name(codec_->codec_id));
    return true;
}

bool VideoDecoder::NextFrameRGBA(std::vector<uint8_t>& out, int& width, int& height)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        while (av_read_frame(fmt_, packet_) >= 0) {
            if (packet_->stream_index != videoStream_) { av_packet_unref(packet_); continue; }
            int sret = avcodec_send_packet(codec_, packet_);
            av_packet_unref(packet_);
            if (sret < 0) continue;
            int rret = avcodec_receive_frame(codec_, frame_);
            if (rret == AVERROR(EAGAIN)) continue;
            if (rret < 0) return false;
            width_ = width = frame_->width;
            height_ = height = frame_->height;
            if (!frameToRGBA(sws_, frame_, out)) { LOG_ERR("sws"); return false; }
            return true;
        }
        closeInternal();
        if (!openInternal()) return false; // loop at EOF
    }
    return false;
}

// ---- stream mode -----------------------------------------------------------

bool VideoDecoder::openCodecOnly(int avCodecId)
{
    const AVCodec* dec = avcodec_find_decoder((AVCodecID)avCodecId);
    if (!dec) { LOG_ERR("no decoder for codec id %d", avCodecId); return false; }
    codec_ = avcodec_alloc_context3(dec);
    if (!codec_) { LOG_ERR("ctx alloc"); return false; }
    if (avcodec_open2(codec_, dec, nullptr) < 0) { LOG_ERR("avcodec_open2 (stream)"); return false; }
    parser_ = av_parser_init(avCodecId);
    if (!parser_) { LOG_ERR("parser init"); return false; }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && packet_;
}

bool VideoDecoder::OpenStream(Codec codec)
{
    streamMode_ = true;
    int id = (codec == Codec::H264) ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    if (!openCodecOnly(id)) { closeInternal(); return false; }
    LOG_INF("stream decoder open (%s)", avcodec_get_name((AVCodecID)id));
    return true;
}

void VideoDecoder::convertToLatest()
{
    std::vector<uint8_t> rgba;
    if (!frameToRGBA(sws_, frame_, rgba)) return;
    std::lock_guard<std::mutex> lk(latestMutex_);
    latest_.swap(rgba);
    latestW_ = frame_->width;
    latestH_ = frame_->height;
    latestFresh_ = true;
}

void VideoDecoder::SubmitNal(const uint8_t* data, size_t size)
{
    if (!codec_ || !parser_ || size == 0) return;
    const uint8_t* p = data;
    size_t remaining = size;
    while (remaining > 0) {
        uint8_t* outData = nullptr;
        int outSize = 0;
        int used = av_parser_parse2(parser_, codec_, &outData, &outSize,
                                    p, (int)remaining, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) return;
        p += used;
        remaining -= (size_t)used;
        if (outSize == 0) continue;

        packet_->data = outData;
        packet_->size = outSize;
        auto t0 = std::chrono::steady_clock::now();
        if (avcodec_send_packet(codec_, packet_) < 0) {
            std::lock_guard<std::mutex> lk(latestMutex_);
            needKeyframe_ = true;
            continue;
        }
        for (;;) {
            int rret = avcodec_receive_frame(codec_, frame_);
            if (rret == AVERROR(EAGAIN) || rret == AVERROR_EOF) break;
            if (rret < 0) {  // decode error — a reference frame was likely lost
                std::lock_guard<std::mutex> lk(latestMutex_);
                needKeyframe_ = true;
                break;
            }
            width_ = frame_->width;
            height_ = frame_->height;
            convertToLatest();
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(latestMutex_);
            lastDecodeMs_ = ms;
        }
    }
}

bool VideoDecoder::TakeLatestRGBA(std::vector<uint8_t>& out, int& width, int& height)
{
    std::lock_guard<std::mutex> lk(latestMutex_);
    if (!latestFresh_ || latest_.empty()) return false;
    out = latest_;
    width = latestW_;
    height = latestH_;
    latestFresh_ = false;
    return true;
}

// ---- teardown --------------------------------------------------------------

void VideoDecoder::closeInternal()
{
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (parser_) { av_parser_close(parser_); parser_ = nullptr; }
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (codec_) avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
    videoStream_ = -1;
}

void VideoDecoder::Close() { closeInternal(); }

double VideoDecoder::LastDecodeMs() const
{
    return lastDecodeMs_;
}

bool VideoDecoder::TakeNeedKeyframe()
{
    std::lock_guard<std::mutex> lk(latestMutex_);
    bool v = needKeyframe_;
    needKeyframe_ = false;
    return v;
}
