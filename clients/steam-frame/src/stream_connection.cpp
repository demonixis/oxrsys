// SPDX-License-Identifier: BSL-1.0
#include "stream_connection.h"
#include "NetworkReceiver.h"
#include "TrackingSender.h"
#include "video_decoder.h"

#include <oxrsys/protocol/Protocol.h>
#include <oxrsys/protocol/Foveation.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#define LOG_INF(fmt, ...) fprintf(stderr, "[INFO] StreamConnection: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) fprintf(stderr, "[ERROR] StreamConnection: " fmt "\n", ##__VA_ARGS__)

using namespace oxr;

StreamConnection::StreamConnection()
    : net_(new NetworkReceiver()), tracker_(new TrackingSender()) {}

StreamConnection::~StreamConnection() { Disconnect(); delete net_; delete tracker_; }

bool StreamConnection::Connect(VideoDecoder* decoder, int timeoutMs)
{
    std::atomic<bool> found{false};
    protocol::ServerAnnounce srv{};
    char ip[64] = {};

    if (!net_->StartDiscovery([&](const protocol::ServerAnnounce& s, const char* serverIp) {
            if (found.exchange(true)) return;
            srv = s;
            strncpy(ip, serverIp, sizeof(ip) - 1);
        })) {
        LOG_ERR("StartDiscovery failed");
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!found && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    net_->StopDiscovery();
    if (!found) { LOG_ERR("no server found in %d ms", timeoutMs); return false; }

    serverIp_ = ip;
    encodedW_ = srv.encodedWidth ? srv.encodedWidth : srv.renderWidth;
    encodedH_ = srv.encodedHeight ? srv.encodedHeight : srv.renderHeight;
    trackingPort_ = srv.trackingPort;

    // Foveated encoding: use the server's announced (aligned) params for the
    // warp, and derive the eye-size ratio from the shared layout math on the
    // per-eye target dims (both sides run the same Foveation.h, so they agree).
    if (srv.foveatedEncodingPreset != protocol::FoveationPreset::Off &&
        srv.renderWidth >= 2 && srv.renderHeight > 0) {
        protocol::FoveationLayout layout = protocol::CalculateFoveationLayout(
            srv.renderWidth / 2, srv.renderHeight, srv.foveatedEncodingPreset);
        foveation_.enabled = true;
        foveation_.centerSize[0] = srv.foveationCenterSizeX;
        foveation_.centerSize[1] = srv.foveationCenterSizeY;
        foveation_.centerShift[0] = srv.foveationCenterShiftX;
        foveation_.centerShift[1] = srv.foveationCenterShiftY;
        foveation_.edgeRatio[0] = srv.foveationEdgeRatioX;
        foveation_.edgeRatio[1] = srv.foveationEdgeRatioY;
        foveation_.eyeSizeRatio[0] = layout.eyeWidthRatio;
        foveation_.eyeSizeRatio[1] = layout.eyeHeightRatio;
        foveation_.targetEyeWidth = srv.renderWidth / 2;
        foveation_.targetEyeHeight = srv.renderHeight;
        LOG_INF("foveated encoding ON: center=%.2f,%.2f edge=%.2f,%.2f eyeRatio=%.3f,%.3f",
                foveation_.centerSize[0], foveation_.centerSize[1],
                foveation_.edgeRatio[0], foveation_.edgeRatio[1],
                foveation_.eyeSizeRatio[0], foveation_.eyeSizeRatio[1]);
    }
    LOG_INF("server '%s' at %s  video=%u tracking=%u  encoded=%ux%u refresh=%uHz",
            srv.serverName, ip, srv.videoPort, srv.trackingPort,
            encodedW_, encodedH_, srv.refreshRateHz);

    // Start receiving BEFORE ClientConnect — the server encodes immediately on
    // connect, and the first keyframe must not arrive before we're listening.
    // Match the server's negotiated FEC group layout; recovering with a different layout
    // than the sender used XORs the wrong packets together.
    net_->SetFecInterleaved(
        (srv.serverFeatures & protocol::SERVER_FEATURE_FEC_INTERLEAVED) != 0);
    bool ok = net_->StartReceiving(ip, srv.videoPort,
        [decoder](const uint8_t* data, size_t size, int64_t timestampNs, int64_t, uint8_t,
                  uint8_t) {
            // Microseconds to match RenderPose.presentationTimeUs, so the decoded frame can
            // be paired with its own render pose and foveation centre.
            decoder->SubmitNal(data, size, timestampNs / 1000);
        },
        [](const char* reason) { LOG_ERR("connection lost: %s", reason); });
    if (!ok) { LOG_ERR("StartReceiving failed"); return false; }

    // Control socket (UDP) for ClientConnect + NACKs.
    controlSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (controlSocket_ < 0) { LOG_ERR("control socket failed"); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(protocol::CONTROL_PORT);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (::connect(controlSocket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERR("control connect failed");
        close(controlSocket_);
        controlSocket_ = -1;
        return false;
    }
    net_->SetControlSocket(controlSocket_, ip);
    tracker_->Connect(ip, srv.trackingPort);

    protocol::ClientConnect cc{};
    cc.type = protocol::MessageType::ClientConnect;
    cc.preferredCodec = (uint32_t)protocol::VideoCodec::H265;
    cc.maxBitrateMbps = protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG;
    cc.refreshRateHz = srv.refreshRateHz ? srv.refreshRateHz : 90;
    // FOVEATION_CENTER commits us to un-warping every frame with the centre carried in the
    // stream (LatestRenderPose below); without it the server keeps the centre static and the
    // gaze we report drives nothing.
    cc.clientCapabilities = protocol::CLIENT_CAPABILITY_FOVEATED_ENCODING |
                            protocol::CLIENT_CAPABILITY_FOVEATION_CENTER |
                            protocol::CLIENT_CAPABILITY_FEC_INTERLEAVED;
    strncpy(cc.deviceName, "FrameClient", sizeof(cc.deviceName) - 1);
    if (send(controlSocket_, &cc, sizeof(cc), 0) < 0) { LOG_ERR("ClientConnect send failed"); return false; }

    LOG_INF("connected — ClientConnect sent, receiving video");
    connected_ = true;
    return true;
}

void StreamConnection::SendTrackingPacket(const protocol::TrackingPacket& pkt)
{
    if (!connected_) return;
    tracker_->Send(pkt);
}

bool StreamConnection::LatestRenderPose(float outPos[3], float outOri[4])
{
    if (!connected_ || !net_) return false;
    NetworkReceiver::RenderPose rp = net_->GetLatestRenderPose();
    if (!rp.valid) return false;
    memcpy(outPos, rp.position, sizeof(float) * 3);
    memcpy(outOri, rp.orientation, sizeof(float) * 4);
    return true;
}

bool StreamConnection::RenderPoseForFrame(int64_t presentationTimeUs, float outPos[3],
                                          float outOri[4])
{
    if (!connected_ || !net_) return false;
    NetworkReceiver::RenderPose rp = {};
    static uint32_t hits = 0, misses = 0;
    if (!net_->TakeRenderPoseForPresentationTimeUs(presentationTimeUs, &rp) || !rp.valid) {
        misses++;
        if (misses <= 10 || misses % 60 == 0)
            LOG_INF("pose match MISS for pts=%lld (hits=%u misses=%u)",
                    (long long)presentationTimeUs, hits, misses);
        return false;
    }
    hits++;
    if (hits <= 10 || hits % 120 == 0)
        LOG_INF("pose match hit pts=%lld centre=%s(%d,%d) (hits=%u misses=%u)",
                (long long)presentationTimeUs, rp.hasFoveationCenter ? "" : "NONE",
                (int)rp.foveationCenterX, (int)rp.foveationCenterY, hits, misses);
    memcpy(outPos, rp.position, sizeof(float) * 3);
    memcpy(outOri, rp.orientation, sizeof(float) * 4);

    // Apply THIS frame's gaze-driven foveation centre. The centre is per-frame data: decode
    // runs several frames deep, so the most recently received centre leads the frame on
    // screen, and un-warping with it stretches the periphery whenever the centre moves.
    // DecodeCenterShift is the same call the server's encoder made on these bytes, so both
    // sides land on identical parameters.
    if (foveation_.enabled && rp.hasFoveationCenter) {
        foveation_.centerShift[0] = protocol::DecodeCenterShift(
            rp.foveationCenterX,
            foveation_.centerSize[0],
            static_cast<float>(foveation_.targetEyeWidth),
            foveation_.edgeRatio[0]);
        foveation_.centerShift[1] = protocol::DecodeCenterShift(
            rp.foveationCenterY,
            foveation_.centerSize[1],
            static_cast<float>(foveation_.targetEyeHeight),
            foveation_.edgeRatio[1]);
    }
    return true;
}

void StreamConnection::SendLatencyReport(float decodeMs, float compositorMs, float totalMs,
                                         uint32_t reprojectedFrames)
{
    if (!connected_ || controlSocket_ < 0) return;
    protocol::LatencyReport r{};
    r.type = protocol::ControlType::LatencyReport;
    r.receiveToDecoderSubmitMs = 0.0f; // best-effort; decode+compositor dominate
    r.decodeLatencyMs = decodeMs;
    r.compositorLatencyMs = compositorMs;
    r.totalClientLatencyMs = totalMs;
    r.reprojectedFrames = reprojectedFrames;
    send(controlSocket_, &r, sizeof(r), 0);
}

void StreamConnection::SendKeyframeRequest(uint32_t reasonFlags, uint32_t detail)
{
    if (!connected_ || controlSocket_ < 0) return;
    protocol::RequestKeyframe r{};
    r.type = protocol::ControlType::RequestKeyframe;
    r.reasonFlags = reasonFlags;
    r.detail = detail;
    send(controlSocket_, &r, sizeof(r), 0);
}

void StreamConnection::Disconnect()
{
    connected_ = false;
    if (net_) net_->Stop();
    if (tracker_) tracker_->Disconnect();
    if (controlSocket_ >= 0) { close(controlSocket_); controlSocket_ = -1; }
}
