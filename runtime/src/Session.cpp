// SPDX-License-Identifier: MPL-2.0

#include "Session.h"
#include "CompositionLayerAlpha.h"
#include "Instance.h"
#include "Runtime.h"
#include "Swapchain.h"
#include "Space.h"
#include "InputManager.h"
#include "InteractionProfileResolve.h"
#include "StreamingServer.h"
#ifdef OXRSYS_HAS_ALVR
#include "AlvrStreamingBackend.h"
#endif
#include "Config.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>
#include <utility>

#include <openxr/openxr_platform.h>

namespace
{

using Clock = std::chrono::steady_clock;

// CLOCK_MONOTONIC in nanoseconds. This is the clock wineopenxr samples when it
// translates a Win32 QPC value to a timespec for XR_KHR_convert_timespec_time.
int64_t MonotonicNowNs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

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
    inputManager_->SetSimpleControllerFallback(Config::Get().GetValues().simpleControllerFallback);

    startTime_ = std::chrono::steady_clock::now();
    monoStartNs_ = MonotonicNowNs();
    lastFrameTime_ = startTime_;
    nextFrameDeadline_ = {};

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
    inputManager_->SetSimpleControllerFallback(Config::Get().GetValues().simpleControllerFallback);

    startTime_ = std::chrono::steady_clock::now();
    monoStartNs_ = MonotonicNowNs();
    lastFrameTime_ = startTime_;
    nextFrameDeadline_ = {};

    Runtime::Get().RegisterHandle(handle_, this);
    instance_->SetSession(this);

    TransitionState(XR_SESSION_STATE_IDLE);
    TransitionState(XR_SESSION_STATE_READY);

    spdlog::info("OXRSys: Vulkan session created");
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

XrTime Session::TimespecToXrTime(const struct timespec& ts) const
{
    const int64_t monoNs = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    // XrTime == steady_clock::now() - startTime_, and CLOCK_MONOTONIC advances in
    // lockstep with steady_clock (both real-time ns), so XrTime == monoNs - monoStartNs_.
    return static_cast<XrTime>(monoNs - monoStartNs_);
}

void Session::XrTimeToTimespec(XrTime time, struct timespec& ts) const
{
    const int64_t monoNs = static_cast<int64_t>(time) + monoStartNs_;
    ts.tv_sec = static_cast<time_t>(monoNs / 1000000000LL);
    ts.tv_nsec = static_cast<long>(monoNs % 1000000000LL);
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

void Session::MaybeEmitInteractionProfileChanged()
{
    if (!inputManager_)
    {
        return;
    }
    // Signature over both hands. When a streaming client connects the profile resolves
    // (e.g. from empty to oculus/touch), which must be signalled so the app (Unity's
    // Input System) re-queries xrGetCurrentInteractionProfile and binds the correct device.
    // Debounced: only a signature stable for kProfileChangeStableDelay is announced (see
    // the Session.h member comments for why transients must never reach the app).
    //
    // Key on the app-visible (instance-filtered) profile, exactly what
    // xrGetCurrentInteractionProfile returns — not the raw InputManager profile.
    // The two can diverge (e.g. the raw profile flips to ext/hand_interaction_ext
    // for an app that never enabled XR_EXT_hand_interaction, whose visible profile
    // stays oculus/touch); keying on the raw value would emit a spurious change
    // event the app cannot observe, the very Unity input churn the debounce exists
    // to suppress.
    std::string sig =
        SelectCurrentInteractionProfileForInstance(instance_, *inputManager_, InputManager::Hand::Left) + "|" +
        SelectCurrentInteractionProfileForInstance(instance_, *inputManager_, InputManager::Hand::Right);
    if (sig == lastNotifiedInteractionProfile_)
    {
        // Back to the announced state before the pending change stabilized (A->B->A):
        // drop the pending change so it can never fire late.
        pendingInteractionProfile_.clear();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (sig != pendingInteractionProfile_)
    {
        pendingInteractionProfile_ = sig;
        pendingInteractionProfileSince_ = now;
        spdlog::info("OXRSys: interaction profile pending '{}' (debouncing)", sig);
        return;
    }
    if (now - pendingInteractionProfileSince_ < kProfileChangeStableDelay)
    {
        return;
    }
    lastNotifiedInteractionProfile_ = sig;
    pendingInteractionProfile_.clear();

    XrEventDataBuffer event{};
    auto* ip = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&event);
    ip->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    ip->next = nullptr;
    ip->session = reinterpret_cast<XrSession>(handle_);
    instance_->PushEvent(event);
    spdlog::info("OXRSys: emitted XrEventDataInteractionProfileChanged (profiles='{}')", sig);
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
    // Reset focus emulation for the new run: a stale focusSuppressed_ would gate the
    // VISIBLE->FOCUSED ratchet forever and stall the restarted session at VISIBLE.
    focusSuppressed_ = false;
    streamingInputSeenActive_ = false;
    lastInputActiveTime_ = std::chrono::steady_clock::now();
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

void Session::Shutdown(bool forProcessExit)
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
        if (forProcessExit)
        {
            streamingServer_->StopForProcessExit();
        }
        else
        {
            streamingServer_->Stop();
        }
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

    // Signal interaction-profile changes (e.g. streaming client connecting after focus) so
    // the app rebinds to the correct controller device instead of the simple-controller fallback.
    MaybeEmitInteractionProfileChanged();

    uint32_t targetRefreshHz = 90;
    if (streamingServer_)
    {
        targetRefreshHz = std::max(streamingServer_->GetTargetRefreshRateHz(), 1u);
    }

    // Throttle to the negotiated headset refresh rate. Pace to an ABSOLUTE deadline grid
    // rather than sleeping (targetFrameTime - elapsed) relative to the actual wake time:
    // std::this_thread::sleep_for oversleeps (worse under Rosetta), and re-anchoring to the
    // late wake makes that oversleep accumulate into steady frame-rate drift (13.9ms ->
    // ~15.3ms => 65fps instead of 72). An absolute grid self-corrects; a short spin for the
    // final sub-millisecond removes most residual jitter.
    const auto targetFrameTime = std::chrono::nanoseconds(1000000000ll / targetRefreshHz);
    auto now = std::chrono::steady_clock::now();
    if (targetRefreshHz != pacedRefreshHz_)
    {
        // Refresh rate changed (e.g. ALVR negotiation): the grid's period/phase is
        // stale — clear the anchor so the branch below re-anchors once at the new rate.
        pacedRefreshHz_ = targetRefreshHz;
        nextFrameDeadline_ = {};
    }
    int64_t backendSleepNs = 0;
    if (streamingServer_ && streamingServer_->GetFramePacing(backendSleepNs) &&
        backendSleepNs > 0 &&
        backendSleepNs < 2 * targetFrameTime.count())
    {
        // Backend pacing (ALVR: duration until the client's next vsync) is used only
        // to ANCHOR the grid, not per frame: re-anchoring from `now` every frame folds
        // per-frame clock jitter into the produced rate (~73.8fps against the 72Hz
        // panel), slowly flooding the client's vsync queue -> periodic micro-stutter.
        // Between anchors, advance the same absolute grid as the fallback below.
        const auto backendDeadline = now + std::chrono::nanoseconds(backendSleepNs);
        if (nextFrameDeadline_.time_since_epoch().count() == 0 || !backendPacedLastFrame_)
        {
            // First backend-paced frame — either the very first WaitFrame, or the
            // previous frame ran on the local fallback grid (client just connected or
            // reconnected). One-shot phase-lock the grid to the client vsync estimate;
            // the arbitrary fallback phase would otherwise persist (up to ~1 frame of
            // standing latency) since the grid below only advances, never re-phases.
            nextFrameDeadline_ = backendDeadline;
        }
        else
        {
            nextFrameDeadline_ += targetFrameTime;
            if (now > nextFrameDeadline_ + targetFrameTime)
            {
                // Fell behind by more than a full frame (long stall) — resync to the
                // client vsync estimate instead of bursting zero-length catch-up frames.
                nextFrameDeadline_ = backendDeadline;
            }
        }
        backendPacedLastFrame_ = true;
    }
    else
    {
        backendPacedLastFrame_ = false;
        if (nextFrameDeadline_.time_since_epoch().count() == 0)
        {
            nextFrameDeadline_ = now; // first frame: anchor the grid here
        }
        nextFrameDeadline_ += targetFrameTime;
        if (now > nextFrameDeadline_ + targetFrameTime)
        {
            // Fell behind by more than a full frame (long stall) — resync instead of
            // bursting a catch-up sequence of zero-length frames.
            nextFrameDeadline_ = now + targetFrameTime;
        }
    }
    constexpr auto kSpinMargin = std::chrono::microseconds(1500);
    if (nextFrameDeadline_ - now > kSpinMargin)
    {
        std::this_thread::sleep_for((nextFrameDeadline_ - now) - kSpinMargin);
    }
    while (std::chrono::steady_clock::now() < nextFrameDeadline_)
    {
        std::this_thread::yield(); // brief spin for sub-ms precision
    }
    now = std::chrono::steady_clock::now();

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
        frameSource.trackingSampleTimestampNs = inputManager_->GetLastTrackingSampleTimestampNs();
        auto sendStart = Clock::now();
        streamingServer_->SendFrame(std::move(frameSource));
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

    // OpenComposite (OpenVR->OpenXR) submits Y-flipped rects to signal the OpenVR
    // texture origin (bottom-left) vs OpenXR (top-left): e.g. offset.y=height,
    // extent.height=-height. That is technically out-of-spec (negative extent), but
    // real runtimes tolerate it. Normalize to a min/max covered region and validate
    // that, so the flipped rect is accepted as long as it stays in bounds.
    const int64_t x0 = static_cast<int64_t>(subImage.imageRect.offset.x);
    const int64_t y0 = static_cast<int64_t>(subImage.imageRect.offset.y);
    const int64_t x1 = x0 + static_cast<int64_t>(subImage.imageRect.extent.width);
    const int64_t y1 = y0 + static_cast<int64_t>(subImage.imageRect.extent.height);
    const int64_t minX = std::min(x0, x1), maxX = std::max(x0, x1);
    const int64_t minY = std::min(y0, y1), maxY = std::max(y0, y1);
    if (subImage.imageRect.extent.width == 0 || subImage.imageRect.extent.height == 0)
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }
    if (minX < 0 || minY < 0 ||
        maxX > static_cast<int64_t>(swapchain->GetWidth()) ||
        maxY > static_cast<int64_t>(swapchain->GetHeight()))
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

    // Focus emulation: the Quest drops both controllers' active flags while the system
    // overlay is open (and everything on client disconnect); Unity maps the session
    // FOCUSED<->VISIBLE transitions to OnApplicationFocus, which is the system-side
    // pause path. Armed only once input has been seen active on the current connection
    // so the connect window (controller flags can lag connect by >0.5s) cannot drop
    // focus. VISIBLE (not SYNCHRONIZED) is also the disconnect state: the frame loop
    // is identical in both states here and SYNCHRONIZED would only add extra ratchet
    // transitions on resume.
    if (inputManager_ && inputManager_->IsStreaming() &&
        (inputManager_->IsInputDeviceActive(InputManager::Hand::Left) ||
         inputManager_->IsInputDeviceActive(InputManager::Hand::Right)))
    {
        streamingInputSeenActive_ = true;
        lastInputActiveTime_ = std::chrono::steady_clock::now();
        focusSuppressed_ = false; // ratchet below restores FOCUSED next frame
    }
    else if (streamingInputSeenActive_ && !focusSuppressed_ &&
             state_ == XR_SESSION_STATE_FOCUSED &&
             std::chrono::steady_clock::now() - lastInputActiveTime_ >= kFocusLossDelay)
    {
        focusSuppressed_ = true;
        TransitionState(XR_SESSION_STATE_VISIBLE);
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
            if (!focusSuppressed_)
            {
                TransitionState(XR_SESSION_STATE_FOCUSED);
            }
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

    auto sc = std::make_unique<Swapchain>(graphicsContext_, createInfo);
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

void Session::ApplyHapticFeedback(int hand, float amplitude, int64_t durationNs, float frequencyHz)
{
    if (!streamingServer_ || !streamingServer_->IsClientConnected())
    {
        return;
    }
    // XR_MIN_HAPTIC_DURATION (-1) and very short pulses map to a floor the
    // client motor can actually render.
    constexpr float kMinDurationS = 0.01f;
    float durationS = durationNs > 0 ? static_cast<float>(durationNs) * 1e-9f : kMinDurationS;
    durationS = std::max(durationS, kMinDurationS);
    // XR_FREQUENCY_UNSPECIFIED (0) passes through; ALVR applies its default.
    streamingServer_->ApplyHaptics(hand, std::clamp(amplitude, 0.0f, 1.0f), durationS,
                                   frequencyHz);
}

void Session::StartStreamingIfNeeded()
{
    if (streamingStarted_)
    {
        return;
    }

    const std::string protocol = Config::Get().GetValues().streamingProtocol;
#ifdef OXRSYS_HAS_ALVR
    if (protocol == "alvr")
    {
        streamingServer_ = std::make_unique<AlvrStreamingBackend>();
    }
#else
    if (protocol == "alvr")
    {
        spdlog::warn("OXRSys: protocol=\"alvr\" requested but this build lacks the ALVR "
                     "backend; falling back to the oxrsys protocol");
    }
#endif
    if (!streamingServer_)
    {
        streamingServer_ = std::make_unique<StreamingServer>();
    }
    streamingServer_->SetGraphicsContext(graphicsContext_);

    // Use default resolution until first swapchain is created
    // Will be updated when we know the actual render resolution
    uint32_t width = 1512;
    uint32_t height = 1680;
    uint32_t refreshHz = std::max(Config::Get().GetValues().refreshRateHz, 1u);

    if (streamingServer_->Start(width, height, refreshHz))
    {
        streamingStarted_ = true;
        spdlog::info("OXRSys: Streaming server started (protocol={}), waiting for headset connection...",
                     protocol);
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
