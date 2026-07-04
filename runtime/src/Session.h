// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include "GraphicsTypes.h"
#include <memory>
#include <vector>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

class Instance;
class Swapchain;
class Space;
class InputManager;
class IStreamingBackend;

class Session
{
public:
    // Metal session
    Session(Instance* instance, void* metalDevice, void* metalCommandQueue = nullptr);

    // Vulkan session (metalDevice for Renderer, Vulkan handles for swapchains)
    Session(Instance* instance, const GraphicsContext& graphicsContext);
    ~Session();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    Instance* GetInstance() const
    {
        return instance_;
    }

    void* GetMetalDevice() const
    {
        return graphicsContext_.metalDevice;
    }

    // Session lifecycle
    XrResult BeginSession(const XrSessionBeginInfo* beginInfo);
    XrResult EndSession();
    XrResult RequestExitSession();

    // Frame loop
    XrResult WaitFrame(const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState);
    XrResult BeginFrame(const XrFrameBeginInfo* frameBeginInfo);
    XrResult EndFrame(const XrFrameEndInfo* frameEndInfo);

    // Views
    XrResult LocateViews(const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState,
                          uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views);

    // Swapchain management
    XrResult CreateSwapchain(const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
    XrResult DestroySwapchain(Swapchain* swapchain);

    // Space management
    XrResult CreateReferenceSpace(const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space);
    XrResult CreateActionSpace(XrAction action, XrPath subactionPath, const XrPosef& poseInSpace, XrSpace* space);
    XrResult DestroySpace(Space* space);

    // Input manager access
    InputManager& GetInputManager()
    {
        return *inputManager_;
    }
    const InputManager& GetInputManager() const
    {
        return *inputManager_;
    }

    XrSessionState GetState() const
    {
        return state_;
    }

    bool IsRunning() const
    {
        return running_;
    }

    bool IsExiting() const
    {
        return exitRequested_;
    }

    // Forward controller haptic feedback to the streaming backend.
    // hand: 0 = left, 1 = right.
    void ApplyHapticFeedback(int hand, float amplitude, int64_t durationNs, float frequencyHz);

    XrTime GetCurrentTime() const;
    // XR_KHR_convert_timespec_time helpers. CLOCK_MONOTONIC is captured at the
    // same instant startTime_ is set (monoStartNs_), so XrTime == mono_ns -
    // monoStartNs_ and the mapping is a pure offset consistent with GetCurrentTime().
    XrTime TimespecToXrTime(const struct timespec& ts) const;
    void XrTimeToTimespec(XrTime time, struct timespec& ts) const;
    void BeginDebugUtilsLabelRegion(const XrDebugUtilsLabelEXT& labelInfo);
    void EndDebugUtilsLabelRegion();
    void InsertDebugUtilsLabel(const XrDebugUtilsLabelEXT& labelInfo);
    void GetDebugUtilsLabels(std::vector<XrDebugUtilsLabelEXT>& labels, std::vector<std::string>& labelNames) const;
    // forProcessExit: called from the dylib destructor / process teardown, where the
    // streaming backend must not join external runtimes (see StopForProcessExit()).
    void Shutdown(bool forProcessExit = false);

private:
    struct DebugUtilsLabelState
    {
        std::string labelName;
    };

    void TransitionState(XrSessionState newState);
    // Emit XrEventDataInteractionProfileChanged when the active controller interaction
    // profile changes (e.g. a streaming client connects and the profile resolves from
    // none/simple to oculus/touch). Unity's Input System relies on this event to switch
    // from its KHR Simple Controller fallback device to the real controller device.
    void MaybeEmitInteractionProfileChanged();
    void AdvanceSessionStateAfterFrameSubmission();
    bool IsFrameLoopRunningState() const;
    bool OwnsSwapchain(const Swapchain* swapchain) const;
    XrResult ValidateSwapchainSubImage(const XrSwapchainSubImage& subImage) const;
    XrResult ValidateProjectionLayer(const XrCompositionLayerProjection& layer,
                                     FrameSource& frameSource) const;
    XrResult ValidateQuadLayer(const XrCompositionLayerQuad& layer) const;

    uint64_t handle_ = 0;
    Instance* instance_;
    GraphicsContext graphicsContext_ = {};

    XrSessionState state_ = XR_SESSION_STATE_IDLE;
    bool running_ = false;
    bool exitRequested_ = false;
    bool frameBegun_ = false;
    uint32_t waitedFrameCount_ = 0;
    mutable std::mutex frameStateMutex_;
    mutable std::mutex debugUtilsMutex_;

    std::unique_ptr<InputManager> inputManager_;
    std::unique_ptr<IStreamingBackend> streamingServer_;
    std::vector<std::unique_ptr<Swapchain>> swapchains_;
    std::vector<std::unique_ptr<Space>> spaces_;

    std::chrono::steady_clock::time_point startTime_;
    // CLOCK_MONOTONIC nanoseconds sampled at the same instant as startTime_.
    int64_t monoStartNs_ = 0;
    // Last interaction-profile signature (left|right) we emitted an event for.
    std::string lastNotifiedInteractionProfile_;
    bool interactionProfileNotified_ = false;
    std::chrono::steady_clock::time_point lastFrameTime_;
    // Absolute deadline for the next frame so pacing does not accumulate sleep drift.
    std::chrono::steady_clock::time_point nextFrameDeadline_{};
    // Refresh rate the pacing grid was built for; a change re-anchors the grid once.
    uint32_t pacedRefreshHz_ = 0;
    // Whether the previous WaitFrame used backend (client vsync) pacing. A false->true
    // transition means the client just (re)connected: phase-lock the grid once.
    bool backendPacedLastFrame_ = false;

    std::vector<DebugUtilsLabelState> debugUtilsLabelRegions_;
    std::optional<DebugUtilsLabelState> debugUtilsInsertedLabel_;

    // Streaming state
    bool streamingStarted_ = false;
    void StartStreamingIfNeeded();
    void CheckStreamingConnection();
};
