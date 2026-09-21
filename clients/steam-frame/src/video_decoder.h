// Frame VR client — video decode layer
// SPDX-License-Identifier: MPL-2.0
//
// FFmpeg HEVC/H264 -> RGBA, portable across Mac (dev) and Linux (Frame).
// Two feed modes behind one narrow seam:
//   - File mode (V1):   Open(path) + NextFrameRGBA()          [pull, one thread]
//   - Stream mode (V2):  OpenStream(codec) + SubmitNal()      [push, net thread]
//                        + TakeLatestRGBA()                    [render thread]
// The guts (software avcodec now) can be swapped for Vulkan Video / VAAPI
// hardware decode later without touching either seam.

#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class VideoDecoder {
public:
    enum class Codec { H265, H264 };

    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // ---- File mode ----
    bool Open(const std::string& path);
    bool NextFrameRGBA(std::vector<uint8_t>& out, int& width, int& height);

    // ---- Stream mode ----
    bool OpenStream(Codec codec);
    // Called from the network thread with a complete encoded access unit.
    void SubmitNal(const uint8_t* data, size_t size);
    // Called from the render thread; true if a newer decoded frame was taken.
    bool TakeLatestRGBA(std::vector<uint8_t>& out, int& width, int& height);

    void Close();
    int Width() const { return width_; }
    int Height() const { return height_; }
    double LastDecodeMs() const;  // wall-clock of the last decode+convert (stream mode)

    // True (and cleared) if a decode error occurred since the last call — the
    // caller should ask the server for a keyframe (IDR) to resync the decoder.
    bool TakeNeedKeyframe();

private:
    bool openInternal();     // file
    bool openCodecOnly(int avCodecId);
    void closeInternal();
    void convertToLatest();  // frame_ -> latest_ (RGBA), stream mode

    std::string path_;
    int width_ = 0, height_ = 0;
    int videoStream_ = -1;
    bool streamMode_ = false;

    struct AVFormatContext* fmt_ = nullptr;
    struct AVCodecContext* codec_ = nullptr;
    struct AVCodecParserContext* parser_ = nullptr;
    struct SwsContext* sws_ = nullptr;
    struct AVFrame* frame_ = nullptr;
    struct AVPacket* packet_ = nullptr;

    // Stream mode: latest decoded frame handed net thread -> render thread.
    std::mutex latestMutex_;
    std::vector<uint8_t> latest_;
    int latestW_ = 0, latestH_ = 0;
    bool latestFresh_ = false;
    double lastDecodeMs_ = 0.0;
    bool needKeyframe_ = false;
};
