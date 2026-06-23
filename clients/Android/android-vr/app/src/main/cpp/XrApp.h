// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openxr/openxr.h>

#include "NetworkReceiver.h"
#include "QuestPassthroughAlphaPolicy.h"
#include "QuestShellInteraction.h"
#include "TrackingSender.h"
#include "VideoDecoder.h"

struct android_app;

namespace oxr
{

/**
 * Main OpenXR application for the streaming client.
 *
 * Manages the OpenXR session lifecycle on the headset:
 * - Creates XrInstance with EGL/OpenGL ES, XrSession, XrSwapchains
 * - Discovers the macOS server via UDP broadcast
 * - Receives H.265 video frames, decodes, and blits to swapchains
 * - Sends head/controller tracking data back to the server
 */
class XrApp
{
public:
    XrApp() = default;
    ~XrApp() = default;

    // Non-copyable
    XrApp(const XrApp&) = delete;
    XrApp& operator=(const XrApp&) = delete;

    bool Initialize(struct android_app* app);
    void RunFrame();
    void Shutdown();

    bool IsRunning() const { return running_; }
    bool IsSessionActive() const { return sessionRunning_; }

private:
    enum class TransportMode
    {
        WifiUdp,
        UsbAdbTcp,
    };

    enum class ConnectionState
    {
        Disconnected,
        Discovering,
        Connecting,
        Connected,
    };

    bool CreateInstance(struct android_app* app);
    bool InitEgl();
    bool CreateSession();
    bool CreateSwapchains();
    bool InitializeHandTracking();
    bool InitializePassthrough();
    bool InitializeFoveation();
    bool InitializeDisplayRefreshRate(float preferredRefreshRateHz);
    void ApplyClientFoveationPreset(protocol::ClientFoveationPreset preset);
    void ShutdownFoveation();
    void ShutdownPassthrough();
    void ShutdownHandTracking();
    void HandleSessionStateChange(XrSessionState newState);

    void StartNetworking();
    void StopNetworking();
    void ResetConnection(const char* reason);
    void OnConnectionLost(const char* reason);
    bool IsConnected() const { return connectionState_.load() == ConnectionState::Connected; }
    bool HasServerConnection() const
    {
        ConnectionState state = connectionState_.load();
        return state == ConnectionState::Connecting || state == ConnectionState::Connected;
    }

    void OnServerFound(const protocol::ServerAnnounce& server, const char* serverIp);
    void ConfigureServerConnection(const protocol::ServerAnnounce& server, const char* serverIp,
                                   TransportMode transportMode);
    bool TryStartUsbAdbTransport(bool logUnavailable = true);
    void RetryUsbAdbTransportIfNeeded();
    bool OpenControlSocket(const char* serverIp);
    bool OpenUsbControlSocket();
    void CloseControlSocket();
    bool OpenUsbSpatialSocket(uint16_t spatialPort);
    void CloseSpatialSocket();
    void SendClientConnect(const char* serverIp);
    void StartControlReceiver();
    void StopControlReceiver();
    void ControlReceiveThreadMain();
    void HandleControlPayload(const uint8_t* data, size_t size);
    void HandleStreamConfigUpdate(const protocol::StreamConfigUpdate& update);
    void StartStreamConfigWorker();
    void StopStreamConfigWorker();
    void StreamConfigWorkerMain();
    void ApplyCompletedStreamConfigUpdate();
    void SendStreamConfigAck(const protocol::StreamConfigUpdate& update, uint8_t status);
    void SendLatencyReport();
    void RequestKeyframe(uint32_t reasonFlags, uint32_t detail);
    float GetCurrentRefreshRateHz() const;
    void OnNalUnitReceived(const uint8_t* data, size_t size,
                           int64_t timestampNs, int64_t receiveTimeNs, uint8_t flags);

    bool RenderFrame(XrTime predictedDisplayTime);
    void BlitVideoToSwapchain(int eye);
    bool CreateShellResources();
    void DestroyShellResources();
    void ReleaseShellForStream(bool keepPassthrough);
    void RenderShellToSwapchain(int eye, bool transparentBackground);
    void ClearSwapchain(int eye, float r, float g, float b);
    XrPosef BuildEyePoseFromRenderPose(const NetworkReceiver::RenderPose& renderPose,
                                       int eye) const;
    XrPosef BuildCurrentHeadPose() const;
    bool ResolveRenderPoseForFrame(int64_t presentationTimeUs,
                                   NetworkReceiver::RenderPose* renderPose,
                                   bool* usedFallback);
    void UpdateReprojectionWarp(bool reusingFrame);
    protocol::TrackingPacket BuildTrackingPacket(XrTime predictedDisplayTime);
    void SendTracking(const protocol::TrackingPacket& packet);
    void UpdateShellPose();
    void UpdateShellInteractions();
    bool SetShellPassthroughActive(bool active);
    bool CanUseShellPassthrough() const;
    quest_passthrough::AlphaKeyDecision EvaluatePassthroughAlphaKey() const;
    const char* ShellStatusText() const;

    bool SetupActions();

    // OpenXR
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;  // For velocity-based tracking
    bool handTrackingExtensionAvailable_ = false;
    bool handTrackingSupported_ = false;
    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
    PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_ = nullptr;
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
    XrHandTrackerEXT handTrackers_[2] = {};
    bool foveationAvailable_ = false;
    bool passthroughExtensionAvailable_ = false;
    bool passthroughSupported_ = false;
    bool foveationConfigurationAvailable_ = false;
    bool swapchainUpdateAvailable_ = false;
    bool displayRefreshRateAvailable_ = false;
    PFN_xrCreateFoveationProfileFB xrCreateFoveationProfileFB_ = nullptr;
    PFN_xrDestroyFoveationProfileFB xrDestroyFoveationProfileFB_ = nullptr;
    PFN_xrUpdateSwapchainFB xrUpdateSwapchainFB_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr;
    XrFoveationProfileFB foveationProfile_ = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB_ = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB_ = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
    PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB_ = nullptr;
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;
    bool passthroughRunning_ = false;
    bool passthroughLayerRunning_ = false;

    // Controller actions
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction gripPoseAction_ = XR_NULL_HANDLE;
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction gripAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction aButtonAction_ = XR_NULL_HANDLE;
    XrAction bButtonAction_ = XR_NULL_HANDLE;
    XrAction menuAction_ = XR_NULL_HANDLE;
    XrSpace gripSpaces_[2] = {};     // [0]=left, [1]=right
    XrSpace aimSpaces_[2] = {};      // [0]=left, [1]=right
    XrPath handPaths_[2] = {};       // /user/hand/left, /user/hand/right

    XrSwapchain swapchains_[2] = {};
    std::vector<uint32_t> swapchainImages_[2]; // GL texture names per eye
    uint32_t swapchainWidth_ = 0;
    uint32_t swapchainHeight_ = 0;

    XrViewConfigurationView viewConfigs_[2] = {};
    XrView views_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    char headsetSystemName_[XR_MAX_SYSTEM_NAME_SIZE] = "Android XR Headset";

    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool running_ = false;

    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLConfig eglConfig_ = nullptr;

    // EGL extension function pointers (for AHardwareBuffer → EGLImage → GL texture)
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID_ = nullptr;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

    // Video rendering (GL resources)
    GLuint videoTexture_ = 0;       // GL_TEXTURE_EXTERNAL_OES for decoded video
    GLuint blitProgram_ = 0;
    GLuint blitVao_ = 0;
    GLuint blitVbo_ = 0;
    GLuint shellProgram_ = 0;
    GLuint shellVao_ = 0;
    GLuint shellVbo_ = 0;
    GLuint fbo_ = 0;               // Framebuffer for blit-to-swapchain
    GLint blitTextureUniform_ = -1;
    GLint blitEyeSourceMinUniform_ = -1;
    GLint blitEyeSourceMaxUniform_ = -1;
    GLint blitLogicalTexelSizeUniform_ = -1;
    GLint blitFoveatedEncodingEnabledUniform_ = -1;
    GLint blitClientUpscalingEnabledUniform_ = -1;
    GLint blitUpscaleEdgeThresholdUniform_ = -1;
    GLint blitUpscaleSharpnessUniform_ = -1;
    GLint blitFoveationCenterSizeUniform_ = -1;
    GLint blitFoveationCenterShiftUniform_ = -1;
    GLint blitFoveationEdgeRatioUniform_ = -1;
    GLint blitFoveationEyeSizeRatioUniform_ = -1;
    GLint blitReprojectionWarpEnabledUniform_ = -1;
    GLint blitReprojectionWarpOffsetUniform_ = -1;
    GLint blitPassthroughAlphaEnabledUniform_ = -1;
    GLint shellMvpUniform_ = -1;

    // Networking
    std::unique_ptr<NetworkReceiver> networkReceiver_;
    std::unique_ptr<TrackingSender> trackingSender_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::mutex videoDecoderMutex_;
    int controlSocket_ = -1;
    int controlTcpSocket_ = -1;
    int spatialTcpSocket_ = -1;
    std::thread controlReceiveThread_;
    std::atomic<bool> controlReceiveRunning_{false};
    TransportMode transportMode_ = TransportMode::WifiUdp;

    std::atomic<ConnectionState> connectionState_{ConnectionState::Disconnected};
    std::atomic<bool> needsReconnect_{false};
    std::atomic<bool> shellPendingNetworkReset_{false};
    std::chrono::steady_clock::time_point lastUsbAdbRetryTime_;
    uint32_t usbAdbRetryAttempts_ = 0;
    char serverIp_[64] = {};
    uint16_t serverVideoPort_ = 0;
    uint16_t serverTrackingPort_ = 0;
    uint16_t serverSpatialPort_ = 0;
    std::chrono::steady_clock::time_point connectionTime_;
    std::string shellStatusText_ = "Searching for runtime";

    // Aspect-ratio-correct blit viewport (computed in OnServerFound)
    uint32_t blitWidth_ = 0;
    uint32_t blitHeight_ = 0;
    int32_t blitOffsetX_ = 0;
    int32_t blitOffsetY_ = 0;
    float macEyeAspect_ = 0.0f;

    // Decoded frame state
    uint32_t videoWidth_ = 0;
    uint32_t videoHeight_ = 0;
    bool hasVideoTexture_ = false;  // True once we've bound at least one decoded frame
    std::chrono::steady_clock::time_point lastVideoFrameTime_;  // Detect stream loss
    float videoContentUMin_ = 0.0f;
    float videoContentUMax_ = 1.0f;
    float videoContentVMin_ = 0.0f;
    float videoContentVMax_ = 1.0f;
    uint32_t clientRefreshRateHz_ = 90;
    protocol::ClientFoveationPreset clientFoveationPreset_ =
        protocol::ClientFoveationPreset::Off;
    protocol::ClientReprojectionMode clientReprojectionMode_ =
        protocol::ClientReprojectionMode::Pose;
    bool serverFoveatedEncodingEnabled_ = false;
    bool clientUpscalingEnabled_ = false;
    bool serverMixedRealityPassthroughEnabled_ = false;
    bool hasObservedProtocolAlphaFrame_ = false;
    bool loggedTransparentClearFallback_ = false;
    float foveationCenterSizeX_ = 1.0f;
    float foveationCenterSizeY_ = 1.0f;
    float foveationCenterShiftX_ = 0.0f;
    float foveationCenterShiftY_ = 0.0f;
    float foveationEdgeRatioX_ = 1.0f;
    float foveationEdgeRatioY_ = 1.0f;
    float foveationEyeWidthRatio_ = 1.0f;
    float foveationEyeHeightRatio_ = 1.0f;
    float decodedTexelWidth_ = 1.0f;
    float decodedTexelHeight_ = 1.0f;
    std::mutex pendingStreamConfigMutex_;
    std::condition_variable pendingStreamConfigCv_;
    protocol::StreamConfigUpdate pendingStreamConfigUpdate_;
    bool hasPendingStreamConfigUpdate_ = false;
    std::mutex completedStreamConfigMutex_;
    protocol::StreamConfigUpdate completedStreamConfigUpdate_;
    bool hasCompletedStreamConfigUpdate_ = false;
    bool completedStreamConfigAccepted_ = false;
    std::thread streamConfigWorkerThread_;
    std::atomic<bool> streamConfigWorkerRunning_{false};
    std::atomic<uint32_t> streamConfigWorkerSequence_{0};
    std::atomic<uint32_t> streamConfigSequence_{0};
    int64_t predictedDisplayPeriodNs_ = 11111111;
    int64_t lastFrameReceiveTimeNs_ = 0;
    int64_t lastFrameSubmitTimeNs_ = 0;
    int64_t lastFrameAcquireTimeNs_ = 0;
    int64_t lastReportedAcquireTimeNs_ = 0;
    uint32_t skippedDecodedFrames_ = 0;
    struct PresentedVideoFrame
    {
        bool valid = false;
        GLuint texture = 0;
        int64_t presentationTimeUs = 0;
        int64_t localReceiveTimeNs = 0;
        int64_t localSubmitTimeNs = 0;
        int64_t localAcquireTimeNs = 0;
        NetworkReceiver::RenderPose renderPose = {};
        bool hasRenderPose = false;
        bool alphaBlend = false;
        XrPosef headsetPoseAtPresentation = {
            {0.0f, 0.0f, 0.0f, 1.0f},
            {0.0f, 0.0f, 0.0f}
        };
        uint32_t consecutiveReuses = 0;
    };
    PresentedVideoFrame presentedVideoFrame_;
    NetworkReceiver::RenderPose currentRenderPose_;
    bool hasCurrentRenderPose_ = false;
    bool reprojectionWarpEnabled_ = false;
    float reprojectionWarpOffsetX_ = 0.0f;
    float reprojectionWarpOffsetY_ = 0.0f;
    uint32_t reprojectedFramesSinceLastReport_ = 0;
    uint32_t renderPoseFallbacksSinceLastReport_ = 0;
    uint32_t staleFrameReusesSinceLastReport_ = 0;
    uint32_t renderPoseHitCount_ = 0;
    uint32_t renderPoseMissCount_ = 0;
    std::chrono::steady_clock::time_point lastRenderPoseLogTime_;
    std::chrono::steady_clock::time_point lastLatencyReportTime_;
    std::chrono::steady_clock::time_point lastKeyframeRequestTime_;
    uint32_t lastObservedDroppedFrames_ = 0;

    struct LatencySamples
    {
        std::vector<double> receiveToSubmitMs;
        std::vector<double> submitToDecodeMs;
        std::vector<double> decodeToCompositorMs;
        std::vector<double> totalClientMs;
        std::vector<double> frameAgeMs;
    } latencySamples_;

    // Diagnostic counters
    uint32_t nalUnitsReceived_ = 0;
    uint32_t decodedFrameCount_ = 0;

    struct ShellControllerInput
    {
        bool active = false;
        bool aimActive = false;
        XrPosef gripPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        XrPosef aimPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        float triggerValue = 0.0f;
        quest_shell::Ray ray = {};
        quest_shell::ControllerClickState clickState = {};
    };
    struct ShellHandInput
    {
        bool active = false;
        quest_shell::Vec3 indexTip = {};
        quest_shell::Ray ray = {};
        float pinchValue = 0.0f;
        quest_shell::ControllerClickState pinchClickState = {};
        std::array<bool, XR_HAND_JOINT_COUNT_EXT> jointActive = {};
        std::array<quest_shell::Vec3, XR_HAND_JOINT_COUNT_EXT> joints = {};
    };
    ShellControllerInput shellControllers_[2];
    ShellHandInput shellHands_[2];
    quest_shell::PanelLayout shellPanelLayout_;
    bool shellPanelInitialized_ = false;
    bool shellPassthroughMode_ = false;
    bool appSpaceIsStage_ = false;
    float shellFloorY_ = 0.0f;
    quest_shell::ButtonId shellHoveredButton_ = quest_shell::ButtonId::None;
};

} // namespace oxr
