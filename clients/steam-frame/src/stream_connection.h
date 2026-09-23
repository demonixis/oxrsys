// Frame VR client — oxrsys stream connection (V2)
// SPDX-License-Identifier: BSL-1.0
//
// Wraps the ported oxrsys NetworkReceiver + control channel: discover the
// server, start receiving video (feeding the decoder), then send ClientConnect.
// Also owns the upstream TrackingSender (used from V3 on).

#pragma once
#include <cstdint>
#include <string>

class VideoDecoder;
namespace oxr {
class NetworkReceiver;
class TrackingSender;
namespace protocol { struct TrackingPacket; }
}

class StreamConnection {
public:
    StreamConnection();
    ~StreamConnection();
    StreamConnection(const StreamConnection&) = delete;
    StreamConnection& operator=(const StreamConnection&) = delete;

    // Blocks up to timeoutMs for a server broadcast, then wires the pipeline:
    // decoder must already be OpenStream()'d. Returns true once ClientConnect
    // is sent. On success, the decoder receives NAL units on a background thread.
    bool Connect(VideoDecoder* decoder, int timeoutMs = 10000);

    // Send a fully-built tracking packet upstream (V4: head + controllers + hands).
    void SendTrackingPacket(const oxr::protocol::TrackingPacket& pkt);

    // The head pose the server rendered the latest frame for (for async
    // timewarp: submit the layer with THIS pose so the compositor reprojects
    // to the current pose). Returns false until a render-pose packet arrives.
    // Non-const: also refreshes the foveation centre to the value the server warped this
    // frame with, so FoveationParams() and the pose describe the same frame.
    bool LatestRenderPose(float outPos[3], float outOri[4]);
    // Pose and foveation centre matched to the frame with this presentation timestamp; the
    // per-frame centre is only ever applied through this path. False when no match is held
    // (startup, metadata loss) - the caller keeps the previous frame's state.
    bool RenderPoseForFrame(int64_t presentationTimeUs, float outPos[3], float outOri[4]);

    // Report client-side latencies over the control channel (V6). oxrsys's
    // adaptive bitrate controller consumes these to tune the encode.
    void SendLatencyReport(float decodeMs, float compositorMs, float totalMs,
                           uint32_t reprojectedFrames);

    // Ask the server for a keyframe (IDR) to resync after a decode error / loss.
    void SendKeyframeRequest(uint32_t reasonFlags, uint32_t detail);

    bool Connected() const { return connected_; }
    const std::string& ServerIp() const { return serverIp_; }
    uint32_t EncodedWidth() const { return encodedW_; }
    uint32_t EncodedHeight() const { return encodedH_; }

    // Foveated-encoding parameters the fragment shader needs to un-warp the
    // frame. enabled=false ⇒ plain SBS sampling. Derived from the server's
    // announced preset + params via the shared Foveation.h layout math.
    struct Foveation {
        bool enabled = false;
        float centerSize[2] = {1, 1};
        float centerShift[2] = {0, 0};
        float edgeRatio[2] = {1, 1};
        float eyeSizeRatio[2] = {1, 1};
        // Pre-foveation per-eye dims, needed to re-align the gaze centre each frame.
        uint32_t targetEyeWidth = 0;
        uint32_t targetEyeHeight = 0;
    };
    const Foveation& FoveationParams() const { return foveation_; }

    void Disconnect();

private:
    oxr::NetworkReceiver* net_ = nullptr;
    oxr::TrackingSender* tracker_ = nullptr;
    int controlSocket_ = -1;
    std::string serverIp_;
    uint32_t encodedW_ = 0, encodedH_ = 0;
    uint16_t trackingPort_ = 0;
    bool connected_ = false;
    Foveation foveation_;
};
