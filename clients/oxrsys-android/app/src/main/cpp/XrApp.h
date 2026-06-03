// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <memory>
#include <mutex>
#include <vector>

#include <openxr/openxr.h>

#include "NetworkReceiver.h"
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
    bool InitializeFoveation();
    bool InitializeDisplayRefreshRate();
    void ShutdownFoveation();
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
    void SendClientConnect(const char* serverIp);
    void SendLatencyReport();
    void RequestKeyframe(uint32_t reasonFlags, uint32_t detail);
    float GetCurrentRefreshRateHz() const;
    void OnNalUnitReceived(const uint8_t* data, size_t size,
                           int64_t timestampNs, int64_t receiveTimeNs);

    void RenderFrame(XrTime predictedDisplayTime);
    void BlitVideoToSwapchain(int eye);
    void ClearSwapchain(int eye, float r, float g, float b);
    XrPosef BuildEyePoseFromRenderPose(const NetworkReceiver::RenderPose& renderPose,
                                       int eye) const;
    void SendTracking(XrTime predictedDisplayTime);

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

    // Controller actions
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction gripPoseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction gripAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction aButtonAction_ = XR_NULL_HANDLE;
    XrAction bButtonAction_ = XR_NULL_HANDLE;
    XrAction menuAction_ = XR_NULL_HANDLE;
    XrSpace gripSpaces_[2] = {};     // [0]=left, [1]=right
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
    GLuint fbo_ = 0;               // Framebuffer for blit-to-swapchain

    // Networking
    std::unique_ptr<NetworkReceiver> networkReceiver_;
    std::unique_ptr<TrackingSender> trackingSender_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    int controlSocket_ = -1;
    int controlTcpSocket_ = -1;
    TransportMode transportMode_ = TransportMode::WifiUdp;

    std::atomic<ConnectionState> connectionState_{ConnectionState::Disconnected};
    std::atomic<bool> needsReconnect_{false};
    std::chrono::steady_clock::time_point lastUsbAdbRetryTime_;
    uint32_t usbAdbRetryAttempts_ = 0;
    char serverIp_[64] = {};
    uint16_t serverVideoPort_ = 0;
    uint16_t serverTrackingPort_ = 0;
    std::chrono::steady_clock::time_point connectionTime_;

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
    int64_t predictedDisplayPeriodNs_ = 11111111;
    int64_t lastFrameReceiveTimeNs_ = 0;
    int64_t lastFrameSubmitTimeNs_ = 0;
    int64_t lastFrameAcquireTimeNs_ = 0;
    int64_t lastReportedAcquireTimeNs_ = 0;
    uint32_t skippedDecodedFrames_ = 0;
    NetworkReceiver::RenderPose currentRenderPose_;
    bool hasCurrentRenderPose_ = false;
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
};

} // namespace oxr
