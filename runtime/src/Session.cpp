// SPDX-License-Identifier: MPL-2.0

#include "Session.h"
#include "CompositionLayerAlpha.h"
#include "Instance.h"
#include "Runtime.h"
#include "Swapchain.h"
#include "Space.h"
#include "InputManager.h"
#include "StreamingServer.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>
#include <utility>

namespace
{

using Clock = std::chrono::steady_clock;

struct SessionMetricSummary
{
    double average = 0.0;
    double p95 = 0.0;
    size_t count = 0;
};

SessionMetricSummary SummarizeSessionSamples(std::vector<double>& samples)
{
    SessionMetricSummary summary = {};
    if (samples.empty())
    {
        return summary;
    }

    std::sort(samples.begin(), samples.end());
    summary.count = samples.size();
    summary.average = std::accumulate(samples.begin(), samples.end(), 0.0) / summary.count;
    size_t p95Index = static_cast<size_t>(0.95 * (samples.size() - 1));
    summary.p95 = samples[p95Index];
    return summary;
}

bool IsSupportedEnvironmentBlendMode(XrEnvironmentBlendMode blendMode, const Instance* instance)
{
    if (blendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
    {
        return true;
    }
    return blendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND &&
           instance != nullptr &&
           instance->SupportsPassthroughBlendMode();
}

constexpr uint32_t kMaxSupportedCompositionLayers = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;

bool IsFiniteQuaternion(const XrQuaternionf& orientation)
{
    return std::isfinite(orientation.x) && std::isfinite(orientation.y) &&
           std::isfinite(orientation.z) && std::isfinite(orientation.w);
}

bool IsValidPose(const XrPosef& pose)
{
    if (!IsFiniteQuaternion(pose.orientation) ||
        !std::isfinite(pose.position.x) ||
        !std::isfinite(pose.position.y) ||
        !std::isfinite(pose.position.z))
    {
        return false;
    }

    const float magnitudeSquared =
        pose.orientation.x * pose.orientation.x +
        pose.orientation.y * pose.orientation.y +
        pose.orientation.z * pose.orientation.z +
        pose.orientation.w * pose.orientation.w;
    if (!(magnitudeSquared > 0.0f) || !std::isfinite(magnitudeSquared))
    {
        return false;
    }

    const float magnitude = std::sqrt(magnitudeSquared);
    return std::fabs(magnitude - 1.0f) <= 0.01f;
}

bool IsFiniteFov(const XrFovf& fov)
{
    return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
           std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown);
}

} // namespace

Session::Session(Instance* instance, void* metalDevice, void* metalCommandQueue)
    : instance_(instance), graphicsContext_(GraphicsContext::Metal(metalDevice, metalCommandQueue))
{
    inputManager_ = std::make_unique<InputManager>();

    startTime_ = std::chrono::steady_clock::now();
    lastFrameTime_ = startTime_;

    Runtime::Get().RegisterHandle(handle_, this);
    instance_->SetSession(this);

    TransitionState(XR_SESSION_STATE_IDLE);
    TransitionState(XR_SESSION_STATE_READY);

    spdlog::info("OXRSys: Metal session created");
}

Session::Session(Instance* instance, const GraphicsContext& graphicsContext)
    : instance_(instance), graphicsContext_(graphicsContext)
{
    inputManager_ = std::make_unique<InputManager>();

    startTime_ = std::chrono::steady_clock::now();
    lastFrameTime_ = startTime_;

    Runtime::Get().RegisterHandle(handle_, this);
    instance_->SetSession(this);

    TransitionState(XR_SESSION_STATE_IDLE);
    TransitionState(XR_SESSION_STATE_READY);

    const char* apiName = "unknown";
    switch (graphicsContext_.api)
    {
        case GraphicsApi::Metal:
            apiName = "Metal";
            break;
        case GraphicsApi::Vulkan:
            apiName = "Vulkan";
            break;
        case GraphicsApi::OpenGL:
            apiName = "OpenGL";
            break;
        case GraphicsApi::D3D11:
            apiName = "D3D11";
            break;
        case GraphicsApi::D3D12:
            apiName = "D3D12";
            break;
    }
    spdlog::info("OXRSys: {} session created", apiName);
}

Session::~Session()
{
    Shutdown();
    instance_->RemoveEventsForSession(reinterpret_cast<XrSession>(handle_));
    instance_->SetSession(nullptr);
    Runtime::Get().RemoveHandle(handle_);
    spdlog::info("OXRSys: Session destroyed");
}

XrTime Session::GetCurrentTime() const
{
    return static_cast<XrTime>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startTime_)
            .count());
}

void Session::TransitionState(XrSessionState newState)
{
    state_ = newState;

    XrEventDataBuffer event{};
    auto* stateChanged = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
    stateChanged->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    stateChanged->next = nullptr;
    stateChanged->session = reinterpret_cast<XrSession>(handle_);
    stateChanged->state = newState;
    stateChanged->time = static_cast<XrTime>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startTime_)
            .count());

    instance_->PushEvent(event);
    spdlog::info("OXRSys: Session state -> {}", static_cast<int>(newState));
}

XrResult Session::BeginSession(const XrSessionBeginInfo* beginInfo)
{
    if (beginInfo == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (running_)
    {
        return XR_ERROR_SESSION_RUNNING;
    }
    if (state_ != XR_SESSION_STATE_READY)
    {
        return XR_ERROR_SESSION_NOT_READY;
    }
    if (!instance_->IsViewConfigurationTypeSupported(beginInfo->primaryViewConfigurationType))
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    exitRequested_ = false;
    {
        std::scoped_lock lock(frameStateMutex_);
        frameBegun_ = false;
        waitedFrameCount_ = 0;
    }

    running_ = true;

    // Start streaming server (broadcasts on LAN, waits for headset connection)
    StartStreamingIfNeeded();

    spdlog::info("OXRSys: Session begun");
    return XR_SUCCESS;
}

XrResult Session::EndSession()
{
    if (state_ != XR_SESSION_STATE_STOPPING)
    {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }

    running_ = false;

    // Stop streaming so it can be restarted on next BeginSession
    if (streamingServer_)
    {
        streamingServer_->Stop();
        streamingServer_.reset();
        streamingStarted_ = false;
        inputManager_->SetTrackingReceiver(nullptr);
        spdlog::info("OXRSys: Streaming server stopped for session end");
    }

    {
        std::scoped_lock lock(frameStateMutex_);
        frameBegun_ = false;
        waitedFrameCount_ = 0;
    }

    TransitionState(XR_SESSION_STATE_IDLE);
    TransitionState(XR_SESSION_STATE_EXITING);
    exitRequested_ = false;

    spdlog::info("OXRSys: Session ended");
    return XR_SUCCESS;
}

XrResult Session::RequestExitSession()
{
    if (!running_)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    exitRequested_ = true;
    return XR_SUCCESS;
}

void Session::Shutdown()
{
    running_ = false;
    exitRequested_ = true;
    state_ = XR_SESSION_STATE_IDLE;

    {
        std::scoped_lock lock(frameStateMutex_);
        frameBegun_ = false;
        waitedFrameCount_ = 0;
    }

    if (inputManager_)
    {
        inputManager_->SetTrackingReceiver(nullptr);
    }

    if (streamingServer_)
    {
        streamingServer_->Stop();
        streamingServer_.reset();
        streamingStarted_ = false;
    }

    spaces_.clear();
    swapchains_.clear();
}

void Session::BeginDebugUtilsLabelRegion(const XrDebugUtilsLabelEXT& labelInfo)
{
    std::scoped_lock lock(debugUtilsMutex_);

    debugUtilsInsertedLabel_.reset();
    DebugUtilsLabelState label = {};
    label.labelName = labelInfo.labelName;
    debugUtilsLabelRegions_.push_back(std::move(label));
}

void Session::EndDebugUtilsLabelRegion()
{
    std::scoped_lock lock(debugUtilsMutex_);

    if (!debugUtilsLabelRegions_.empty())
    {
        debugUtilsLabelRegions_.pop_back();
    }
    debugUtilsInsertedLabel_.reset();
}

void Session::InsertDebugUtilsLabel(const XrDebugUtilsLabelEXT& labelInfo)
{
    std::scoped_lock lock(debugUtilsMutex_);

    DebugUtilsLabelState label = {};
    label.labelName = labelInfo.labelName;
    debugUtilsInsertedLabel_ = std::move(label);
}

void Session::GetDebugUtilsLabels(std::vector<XrDebugUtilsLabelEXT>& labels, std::vector<std::string>& labelNames) const
{
    std::vector<DebugUtilsLabelState> activeLabels;
    {
        std::scoped_lock lock(debugUtilsMutex_);

        if (debugUtilsInsertedLabel_.has_value())
        {
            activeLabels.push_back(*debugUtilsInsertedLabel_);
        }
        for (auto it = debugUtilsLabelRegions_.rbegin(); it != debugUtilsLabelRegions_.rend(); ++it)
        {
            activeLabels.push_back(*it);
        }
    }

    labelNames.clear();
    labels.clear();
    labelNames.reserve(activeLabels.size());
    labels.reserve(activeLabels.size());

    for (const auto& activeLabel : activeLabels)
    {
        labelNames.push_back(activeLabel.labelName);

        XrDebugUtilsLabelEXT label = {XR_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.labelName = labelNames.back().c_str();
        labels.push_back(label);
    }
}

XrResult Session::WaitFrame(const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState)
{
    if (frameWaitInfo != nullptr && frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (frameState == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (frameState->type != XR_TYPE_FRAME_STATE)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    {
        std::scoped_lock lock(frameStateMutex_);
        if (!IsFrameLoopRunningState())
        {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
    }

    uint32_t targetRefreshHz = 90;
    if (streamingServer_)
    {
        targetRefreshHz = std::max(streamingServer_->GetTargetRefreshRateHz(), 1u);
    }

    // Throttle to the negotiated headset refresh rate when available.
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - lastFrameTime_;
    auto targetFrameTime = std::chrono::nanoseconds(1000000000ll / targetRefreshHz);

    if (elapsed < targetFrameTime)
    {
        std::this_thread::sleep_for(targetFrameTime - elapsed);
        now = std::chrono::steady_clock::now();
    }

    auto dt = std::chrono::duration<float>(now - lastFrameTime_).count();
    lastFrameTime_ = now;

    // Update input
    inputManager_->Update(dt);

    auto displayTime = std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTime_).count();

    frameState->type = XR_TYPE_FRAME_STATE;
    frameState->predictedDisplayTime = static_cast<XrTime>(displayTime);
    frameState->predictedDisplayPeriod = static_cast<XrDuration>(targetFrameTime.count());
    frameState->shouldRender = running_ && !exitRequested_ ? XR_TRUE : XR_FALSE;

    {
        std::scoped_lock lock(frameStateMutex_);
        if (!IsFrameLoopRunningState())
        {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        ++waitedFrameCount_;
    }

    return XR_SUCCESS;
}

XrResult Session::BeginFrame(const XrFrameBeginInfo* frameBeginInfo)
{
    if (frameBeginInfo != nullptr && frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(frameStateMutex_);
    if (!IsFrameLoopRunningState())
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (waitedFrameCount_ == 0)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    --waitedFrameCount_;
    if (frameBegun_)
    {
        return XR_FRAME_DISCARDED;
    }

    frameBegun_ = true;
    return XR_SUCCESS;
}

XrResult Session::EndFrame(const XrFrameEndInfo* frameEndInfo)
{
    {
        std::scoped_lock lock(frameStateMutex_);
        if (!IsFrameLoopRunningState())
        {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        if (!frameBegun_)
        {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
    }

    if (frameEndInfo == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (frameEndInfo->displayTime <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    if (!IsSupportedEnvironmentBlendMode(frameEndInfo->environmentBlendMode, instance_))
    {
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    }
    if (frameEndInfo->layerCount > kMaxSupportedCompositionLayers)
    {
        return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    }
    if (frameEndInfo->layerCount > 0 && frameEndInfo->layers == nullptr)
    {
        return XR_ERROR_LAYER_INVALID;
    }

    // Extract submitted eye sources for streaming. Missing streaming sources do
    // not invalidate the OpenXR frame; they only skip this frame's video encode.
    const bool passthroughEnabled = instance_ != nullptr &&
                                    instance_->SupportsPassthroughBlendMode();
    const bool environmentAlphaBlend =
        frameEndInfo->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    bool sourceAlphaProjectionLayer = false;

    FrameSource frameSource = {};

    for (uint32_t i = 0; i < frameEndInfo->layerCount; i++)
    {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
        if (layer == nullptr)
        {
            return XR_ERROR_LAYER_INVALID;
        }

        switch (layer->type)
        {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            {
                const auto& projectionLayer =
                    *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                if (oxrsys::runtime::IsSourceAlphaProjectionLayerForStreaming(
                        projectionLayer.layerFlags, passthroughEnabled))
                {
                    sourceAlphaProjectionLayer = true;
                }

                XrResult result = ValidateProjectionLayer(projectionLayer, frameSource);
                if (result != XR_SUCCESS)
                {
                    return result;
                }
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD:
            {
                XrResult result = ValidateQuadLayer(*reinterpret_cast<const XrCompositionLayerQuad*>(layer));
                if (result != XR_SUCCESS)
                {
                    return result;
                }
                break;
            }
            default:
                return XR_ERROR_LAYER_INVALID;
        }
    }

    frameSource.alphaBlend = oxrsys::runtime::IsAlphaFrameForStreaming(
        frameEndInfo->environmentBlendMode, sourceAlphaProjectionLayer);

    if (!environmentAlphaBlend && sourceAlphaProjectionLayer)
    {
        static std::atomic_bool loggedSourceAlphaProjection{false};
        if (!loggedSourceAlphaProjection.exchange(true))
        {
            spdlog::info("OXRSys: using projection layer source-alpha flags for passthrough streaming");
        }
    }

    // Send to connected headset client if streaming
    CheckStreamingConnection();
    if (streamingServer_ && streamingServer_->IsClientConnected())
    {
        auto sendStart = Clock::now();
        if (lastRenderHasPose_)
        {
            const float renderHeadOrientation[4] = {
                lastRenderHeadPose_.orientation.x, lastRenderHeadPose_.orientation.y,
                lastRenderHeadPose_.orientation.z, lastRenderHeadPose_.orientation.w};
            const float renderHeadPosition[3] = {
                lastRenderHeadPose_.position.x, lastRenderHeadPose_.position.y,
                lastRenderHeadPose_.position.z};
            streamingServer_->SendFrame(std::move(frameSource), renderHeadOrientation,
                                        renderHeadPosition);
        }
        else
        {
            streamingServer_->SendFrame(std::move(frameSource));
        }
        double enqueueMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            Clock::now() - sendStart).count();

        static std::vector<double> enqueueSamples;
        static auto lastLogTime = Clock::now();
        enqueueSamples.push_back(enqueueMs);
        if (Clock::now() - lastLogTime >= std::chrono::seconds(1))
        {
            SessionMetricSummary summary = SummarizeSessionSamples(enqueueSamples);
            spdlog::info("OXRSys: Session::EndFrame streaming enqueue avg/p95 = {:.3f}/{:.3f}ms (n={})",
                          summary.average, summary.p95, summary.count);
            enqueueSamples.clear();
            lastLogTime = Clock::now();
        }
    }

    {
        std::scoped_lock lock(frameStateMutex_);
        frameBegun_ = false;
    }

    AdvanceSessionStateAfterFrameSubmission();
    return XR_SUCCESS;
}

bool Session::OwnsSwapchain(const Swapchain* swapchain) const
{
    return std::any_of(swapchains_.begin(), swapchains_.end(),
                       [swapchain](const std::unique_ptr<Swapchain>& candidate)
                       {
                           return candidate.get() == swapchain;
                       });
}

XrResult Session::ValidateSwapchainSubImage(const XrSwapchainSubImage& subImage) const
{
    auto* swapchain = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(subImage.swapchain));
    if (swapchain == nullptr || !OwnsSwapchain(swapchain))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!swapchain->HasReleasedImage())
    {
        return XR_ERROR_LAYER_INVALID;
    }
    if (subImage.imageRect.offset.x < 0 || subImage.imageRect.offset.y < 0)
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }
    if (subImage.imageRect.extent.width <= 0 || subImage.imageRect.extent.height <= 0)
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }

    const int64_t imageRectMaxX =
        static_cast<int64_t>(subImage.imageRect.offset.x) + static_cast<int64_t>(subImage.imageRect.extent.width);
    const int64_t imageRectMaxY =
        static_cast<int64_t>(subImage.imageRect.offset.y) + static_cast<int64_t>(subImage.imageRect.extent.height);
    if (imageRectMaxX > static_cast<int64_t>(swapchain->GetWidth()) ||
        imageRectMaxY > static_cast<int64_t>(swapchain->GetHeight()))
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }
    if (subImage.imageArrayIndex >= swapchain->GetArraySize())
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    return XR_SUCCESS;
}

XrResult Session::ValidateProjectionLayer(const XrCompositionLayerProjection& layer,
                                          FrameSource& frameSource) const
{
    auto* space = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(layer.space));
    if (space == nullptr || space->GetSession() != this)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (layer.viewCount != 2 || layer.views == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    for (uint32_t viewIndex = 0; viewIndex < layer.viewCount; ++viewIndex)
    {
        const XrCompositionLayerProjectionView& view = layer.views[viewIndex];
        if (!IsValidPose(view.pose))
        {
            return XR_ERROR_POSE_INVALID;
        }
        if (!IsFiniteFov(view.fov))
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        XrResult subImageResult = ValidateSwapchainSubImage(view.subImage);
        if (subImageResult != XR_SUCCESS)
        {
            return subImageResult;
        }

        auto* swapchain = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(view.subImage.swapchain));
        FrameImageSource imageSource =
            swapchain->GetLastReleasedFrameImageSource(view.subImage.imageArrayIndex);
        if (viewIndex == 0)
        {
            frameSource.left = std::move(imageSource);
        }
        else
        {
            frameSource.right = std::move(imageSource);
        }
    }

    return XR_SUCCESS;
}

XrResult Session::ValidateQuadLayer(const XrCompositionLayerQuad& layer) const
{
    auto* space = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(layer.space));
    if (space == nullptr || space->GetSession() != this)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!IsValidPose(layer.pose))
    {
        return XR_ERROR_POSE_INVALID;
    }
    if (!std::isfinite(layer.size.width) || !std::isfinite(layer.size.height) ||
        layer.size.width < 0.0f || layer.size.height < 0.0f)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    return ValidateSwapchainSubImage(layer.subImage);
}

bool Session::IsFrameLoopRunningState() const
{
    return running_ && state_ != XR_SESSION_STATE_STOPPING;
}

XrResult Session::LocateViews(const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState,
                               uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views)
{
    if (viewLocateInfo == nullptr || viewState == nullptr || viewCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (viewLocateInfo->displayTime <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    auto* baseSpace = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(viewLocateInfo->space));
    if (baseSpace == nullptr || baseSpace->GetSession() != this)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    *viewCountOutput = 2;

    viewState->type = XR_TYPE_VIEW_STATE;
    viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;

    if (viewCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (views == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (viewCapacityInput < 2)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    inputManager_->GetEyeViews(views, 2);

    // Remember the exact head pose this frame is being rendered for, so the streamed frame can be
    // tagged with it at submission instead of a later re-prediction.
    lastRenderHeadPose_ = inputManager_->GetHeadPose();
    lastRenderHasPose_ = true;

    return XR_SUCCESS;
}

void Session::AdvanceSessionStateAfterFrameSubmission()
{
    if (!running_)
    {
        return;
    }

    if (exitRequested_)
    {
        switch (state_)
        {
            case XR_SESSION_STATE_FOCUSED:
                TransitionState(XR_SESSION_STATE_VISIBLE);
                break;

            case XR_SESSION_STATE_VISIBLE:
                TransitionState(XR_SESSION_STATE_SYNCHRONIZED);
                break;

            case XR_SESSION_STATE_READY:
                TransitionState(XR_SESSION_STATE_SYNCHRONIZED);
                break;

            case XR_SESSION_STATE_SYNCHRONIZED:
                TransitionState(XR_SESSION_STATE_STOPPING);
                break;

            default:
                break;
        }
        return;
    }

    switch (state_)
    {
        case XR_SESSION_STATE_READY:
            TransitionState(XR_SESSION_STATE_SYNCHRONIZED);
            break;

        case XR_SESSION_STATE_SYNCHRONIZED:
            TransitionState(XR_SESSION_STATE_VISIBLE);
            break;

        case XR_SESSION_STATE_VISIBLE:
            TransitionState(XR_SESSION_STATE_FOCUSED);
            break;

        default:
            break;
    }
}

XrResult Session::CreateSwapchain(const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain)
{
    if (createInfo == nullptr || swapchain == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO ||
        createInfo->width == 0 ||
        createInfo->height == 0 ||
        createInfo->faceCount != 1 ||
        createInfo->arraySize == 0 ||
        createInfo->sampleCount != 1)
    {
        *swapchain = XR_NULL_HANDLE;
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->mipCount != 1)
    {
        *swapchain = XR_NULL_HANDLE;
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    *swapchain = XR_NULL_HANDLE;
    auto sc = std::make_unique<Swapchain>(graphicsContext_, createInfo);
    XrResult initializationResult = sc->InitializationResult();
    if (initializationResult != XR_SUCCESS)
    {
        return initializationResult;
    }
    *swapchain = reinterpret_cast<XrSwapchain>(sc->GetHandle());
    swapchains_.push_back(std::move(sc));
    return XR_SUCCESS;
}

XrResult Session::DestroySwapchain(Swapchain* swapchain)
{
    for (auto it = swapchains_.begin(); it != swapchains_.end(); ++it)
    {
        if (it->get() == swapchain)
        {
            swapchains_.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

XrResult Session::CreateReferenceSpace(const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space)
{
    if (createInfo == nullptr || space == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!IsValidPose(createInfo->poseInReferenceSpace))
    {
        return XR_ERROR_POSE_INVALID;
    }

    switch (createInfo->referenceSpaceType)
    {
        case XR_REFERENCE_SPACE_TYPE_VIEW:
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
        case XR_REFERENCE_SPACE_TYPE_STAGE:
            break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
            if (!instance_->SupportsLocalFloor())
            {
                return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
            }
            break;
        default:
            return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }

    auto sp = std::make_unique<Space>(this, Space::Type::Reference,
                                       createInfo->referenceSpaceType, createInfo->poseInReferenceSpace);
    *space = reinterpret_cast<XrSpace>(sp->GetHandle());
    spaces_.push_back(std::move(sp));
    return XR_SUCCESS;
}

XrResult Session::CreateActionSpace(XrAction action, XrPath subactionPath, const XrPosef& poseInSpace,
                                     XrSpace* space)
{
    if (space == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto sp = std::make_unique<Space>(this, action, subactionPath, poseInSpace);
    *space = reinterpret_cast<XrSpace>(sp->GetHandle());
    spaces_.push_back(std::move(sp));
    return XR_SUCCESS;
}

XrResult Session::DestroySpace(Space* space)
{
    for (auto it = spaces_.begin(); it != spaces_.end(); ++it)
    {
        if (it->get() == space)
        {
            spaces_.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

void Session::StartStreamingIfNeeded()
{
    if (streamingStarted_)
    {
        return;
    }

    streamingServer_ = std::make_unique<StreamingServer>();
    streamingServer_->SetGraphicsContext(graphicsContext_);

    // Use default resolution until first swapchain is created
    // Will be updated when we know the actual render resolution
    uint32_t width = 1512;
    uint32_t height = 1680;
    uint32_t refreshHz = 90;

    if (streamingServer_->Start(width, height, refreshHz))
    {
        streamingStarted_ = true;
        spdlog::info("OXRSys: Streaming server started, waiting for headset connection...");
    }
    else
    {
        spdlog::warn("OXRSys: Failed to start streaming server (non-fatal, simulator mode only)");
        streamingServer_.reset();
    }
}

void Session::CheckStreamingConnection()
{
    if (!streamingServer_)
    {
        return;
    }

    // When a client connects, wire up the tracking receiver
    if (streamingServer_->IsClientConnected() && !inputManager_->IsStreaming())
    {
        std::string clientName = streamingServer_->GetClientName();
        inputManager_->SetTrackingReceiver(streamingServer_->GetTrackingReceiver());
        inputManager_->SetStreamingClientName(clientName);
        spdlog::info("OXRSys: Client connected ({}), receiving tracking",
                      clientName);
    }

    // When client disconnects, clear the tracking receiver
    if (!streamingServer_->IsClientConnected() && inputManager_->IsStreaming())
    {
        inputManager_->SetTrackingReceiver(nullptr);
        spdlog::info("OXRSys: Client disconnected");
    }
}
