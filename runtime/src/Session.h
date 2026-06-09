// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>

#include "GraphicsTypes.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class Instance;
class Swapchain;
class Space;
class InputManager;
class StreamingServer;

class Session
{
public:
    Session(Instance* instance, void* metalDevice, void* metalCommandQueue = nullptr);

#ifdef XR_USE_GRAPHICS_API_VULKAN
    Session(Instance* instance, void* metalDevice, const VulkanGraphicsContext& vulkanContext);
#endif
    Session(Instance* instance, const GraphicsContext& graphicsContext);

#if defined(_WIN32)
    Session(Instance* instance, const D3D11GraphicsContext& d3d11Context);
    Session(Instance* instance, const D3D12GraphicsContext& d3d12Context);
#endif

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

    XrResult BeginSession(const XrSessionBeginInfo* beginInfo);
    XrResult EndSession();
    XrResult RequestExitSession();

    XrResult WaitFrame(const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState);
    XrResult BeginFrame(const XrFrameBeginInfo* frameBeginInfo);
    XrResult EndFrame(const XrFrameEndInfo* frameEndInfo);

    XrResult LocateViews(const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState,
                         uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views);

    XrResult CreateSwapchain(const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
    XrResult DestroySwapchain(Swapchain* swapchain);

    XrResult CreateReferenceSpace(const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space);
    XrResult CreateActionSpace(XrAction action, XrPath subactionPath,
                               const XrPosef& poseInSpace, XrSpace* space);
    XrResult DestroySpace(Space* space);

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

    XrTime GetCurrentTime() const;
    void BeginDebugUtilsLabelRegion(const XrDebugUtilsLabelEXT& labelInfo);
    void EndDebugUtilsLabelRegion();
    void InsertDebugUtilsLabel(const XrDebugUtilsLabelEXT& labelInfo);
    void GetDebugUtilsLabels(std::vector<XrDebugUtilsLabelEXT>& labels,
                             std::vector<std::string>& labelNames) const;
    void Shutdown();

private:
    struct DebugUtilsLabelState
    {
        std::string labelName;
    };

    void TransitionState(XrSessionState newState);
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
    std::unique_ptr<StreamingServer> streamingServer_;
    std::vector<std::unique_ptr<Swapchain>> swapchains_;
    std::vector<std::unique_ptr<Space>> spaces_;

    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastFrameTime_;

    std::vector<DebugUtilsLabelState> debugUtilsLabelRegions_;
    std::optional<DebugUtilsLabelState> debugUtilsInsertedLabel_;

    bool streamingStarted_ = false;
    void StartStreamingIfNeeded();
    void CheckStreamingConnection();
};
