// SPDX-License-Identifier: MPL-2.0

#include "Session.h"
#include "CompositionLayerAlpha.h"
#include "CompositionOverlay.h"
#include "Config.h"
#include "Instance.h"
#include "Runtime.h"
#include "Swapchain.h"
#include "SwapchainCreateValidation.h"
#include "Space.h"
#include "InputManager.h"
#include "RuntimePlatform.h"
#include "StreamingServer.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <numeric>
#include <thread>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;

// CLOCK_MONOTONIC in nanoseconds — the clock XR_KHR_convert_timespec_time
// converts against.
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

    startTime_ = std::chrono::steady_clock::now();
    // Test seam: holds the frame-driven READY -> SYNCHRONIZED transition after its branch is chosen,
    // so the RequestExitSession race test is deterministic. Unset in normal use.
    if (const char* delay = std::getenv("OXRSYS_TEST_ENDFRAME_ADVANCE_DELAY_US"))
    {
        endFrameAdvanceDelayUs_ = std::atoi(delay);
    }
    monoStartNs_ = MonotonicNowNs();
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
    // Test seam: holds the frame-driven READY -> SYNCHRONIZED transition after its branch is chosen,
    // so the RequestExitSession race test is deterministic. Unset in normal use.
    if (const char* delay = std::getenv("OXRSYS_TEST_ENDFRAME_ADVANCE_DELAY_US"))
    {
        endFrameAdvanceDelayUs_ = std::atoi(delay);
    }
    monoStartNs_ = MonotonicNowNs();
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
    }
    spdlog::info("OXRSys: {} session created", apiName);
}

Session::~Session()
{
    if (Shutdown() != XR_SUCCESS)
    {
        // xrDestroySession retains the Session on a bounded drain timeout.
        // Freeing it here would invalidate callbacks and app-device resources.
        std::terminate();
    }
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
    {
        std::scoped_lock lock(pacingThreadMutex_);
        RestorePacingThreadLocked();
    }

    // Stop streaming so it can be restarted on next BeginSession
    streamingSnapshotDemand_->store(false, std::memory_order_release);
    if (streamingServer_)
    {
        if (!streamingServer_->Stop())
        {
            return XR_ERROR_RUNTIME_FAILURE;
        }
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

    std::scoped_lock transitionLock(stateTransitionMutex_);
    exitRequested_ = true;

    // Exit normally advances one step per submitted frame. A session that never left READY has
    // no frame loop to do that (OpenComposite requests exit and only pumps events), so step
    // straight to STOPPING here instead of leaving the app to time out.
    {
        std::scoped_lock lock(frameStateMutex_);
        if (state_ == XR_SESSION_STATE_READY && !frameBegun_ && waitedFrameCount_ == 0)
        {
            TransitionState(XR_SESSION_STATE_SYNCHRONIZED);
            TransitionState(XR_SESSION_STATE_STOPPING);
        }
    }
    return XR_SUCCESS;
}

XrResult Session::Shutdown()
{
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    teardownStarted_.store(true, std::memory_order_release);
    running_ = false;
    exitRequested_ = true;
    state_ = XR_SESSION_STATE_IDLE;

    {
        std::scoped_lock lock(frameStateMutex_);
        frameBegun_ = false;
        waitedFrameCount_ = 0;
    }
    {
        std::scoped_lock lock(pacingThreadMutex_);
        RestorePacingThreadLocked();
    }

    if (inputManager_)
    {
        inputManager_->SetTrackingReceiver(nullptr);
    }

    streamingSnapshotDemand_->store(false, std::memory_order_release);
    if (streamingServer_)
    {
        if (!streamingServer_->Stop())
        {
            // Keep the server, encoder, swapchains and public session handle
            // alive. xrDestroySession/Instance can retry after callbacks exit.
            return XR_ERROR_RUNTIME_FAILURE;
        }
        streamingServer_.reset();
        streamingStarted_ = false;
    }

    std::lock_guard<std::mutex> swapchainsLock(swapchainsMutex_);
    for (const auto& swapchain : swapchains_)
    {
        const XrResult result = swapchain->PrepareForDestroy();
        if (result != XR_SUCCESS)
        {
            return result;
        }
    }
    spaces_.clear();
    swapchains_.clear();
    return XR_SUCCESS;
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

// pacing_thread = "realtime": the sleep_until below runs on the app's render thread, and at default
// priority macOS lets that wake land up to several ms after the deadline, which shows up as
// frame-interval jitter. The policy belongs to the waiting thread: it is set on that thread's wait,
// set again when the frame period changes, and the thread gets its previous policy back when
// realtime pacing is switched off, another thread takes over the wait, or the session ends.
void Session::UpdatePacingThread(std::chrono::nanoseconds framePeriod)
{
    const bool realtime = Config::Get().GetValues().pacingThread == "realtime";
    const std::thread::id caller = std::this_thread::get_id();
    std::scoped_lock lock(pacingThreadMutex_);
    switch (oxrsys::pacing_thread::Decide(pacingThread_, realtime, caller, framePeriod))
    {
        case oxrsys::pacing_thread::Action::Keep:
            return;
        case oxrsys::pacing_thread::Action::Restore:
            RestorePacingThreadLocked();
            return;
        case oxrsys::pacing_thread::Action::Move:
            RestorePacingThreadLocked();
            [[fallthrough]];
        case oxrsys::pacing_thread::Action::Promote:
            pacingThreadSaved_ = oxrsys::runtime_platform::SaveCurrentThreadPolicy();
            [[fallthrough]];
        case oxrsys::pacing_thread::Action::Retune:
            break;
    }
    const bool ok = oxrsys::runtime_platform::SetCurrentThreadRealtime(framePeriod);
    pacingThread_ = {caller, framePeriod};
    spdlog::info("OXRSys: Session::WaitFrame pacing_thread=realtime period={}us applied={}",
                 framePeriod.count() / 1000, ok);
}

void Session::RestorePacingThreadLocked()
{
    if (pacingThread_.thread == std::thread::id{})
    {
        return;
    }
    const bool ok = oxrsys::runtime_platform::RestoreThreadPolicy(pacingThreadSaved_);
    pacingThread_ = {};
    spdlog::info("OXRSys: Session pacing_thread previous policy restored={}", ok);
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

    streamingSnapshotDemand_->store(
        streamingServer_ != nullptr && streamingServer_->IsClientConnected(),
        std::memory_order_release);

    uint32_t targetRefreshHz = 90;
    if (streamingServer_)
    {
        targetRefreshHz = std::max(streamingServer_->GetTargetRefreshRateHz(), 1u);
    }

    // Frame pacing: self-correcting absolute-deadline grid at the negotiated display
    // period. The previous scheme slept for (period - elapsed) and then re-anchored
    // to the wake time, so sleep overshoot was never corrected: periods were always
    // >= the target and the phase drifted continuously against the panel (the app
    // free-ran slightly under refresh, producing a slow beat and frame-duplication
    // judder even at a good average framerate). Here each frame targets a fixed
    // absolute deadline advanced by exactly one period, so overshoot on one frame is
    // absorbed by the next and the cadence locks to the panel period with low
    // variance. targetFrameTime comes from the client-reported (negotiated) refresh.
    auto targetFrameTime = std::chrono::nanoseconds(1000000000ll / targetRefreshHz);
    UpdatePacingThread(targetFrameTime);
    auto now = std::chrono::steady_clock::now();

    if (nextFrameDeadline_.time_since_epoch().count() == 0 || pacingPeriod_ != targetFrameTime)
    {
        // First frame, or the negotiated refresh changed: (re)seed the grid.
        pacingPeriod_ = targetFrameTime;
        nextFrameDeadline_ = now + targetFrameTime;
    }
    else
    {
        if (now < nextFrameDeadline_)
        {
            std::this_thread::sleep_until(nextFrameDeadline_);
            now = std::chrono::steady_clock::now();
        }
        nextFrameDeadline_ += targetFrameTime;

        // If we fell far behind (a hitch, a stall, or the app pausing), snap the grid
        // back to the present rather than firing a burst of catch-up frames.
        if (now - nextFrameDeadline_ > 2 * targetFrameTime)
        {
            nextFrameDeadline_ = now + targetFrameTime;
        }
    }

    // Frame-pacing diagnostic: actual inter-WaitFrame interval statistics prove the
    // cadence locks to the negotiated period (e.g. ~13.9ms @72Hz, ~11.1ms @90Hz)
    // with low variance instead of free-running / drifting.
    {
        static std::vector<double> intervalSamplesMs;
        static auto lastPacingLog = Clock::now();
        auto intervalMs = std::chrono::duration<double, std::milli>(now - lastFrameTime_).count();
        if (lastFrameTime_ != startTime_)
        {
            intervalSamplesMs.push_back(intervalMs);
        }
        if (now - lastPacingLog >= std::chrono::seconds(1) && !intervalSamplesMs.empty())
        {
            double sum = std::accumulate(intervalSamplesMs.begin(), intervalSamplesMs.end(), 0.0);
            double mean = sum / intervalSamplesMs.size();
            double variance = 0.0;
            for (double s : intervalSamplesMs) { variance += (s - mean) * (s - mean); }
            variance /= intervalSamplesMs.size();
            double stddev = std::sqrt(variance);
            auto mm = std::minmax_element(intervalSamplesMs.begin(), intervalSamplesMs.end());
            spdlog::info("OXRSys: Session::WaitFrame pacing target={}Hz ({:.2f}ms) "
                         "actual mean={:.2f}ms stddev={:.2f}ms min/max={:.2f}/{:.2f}ms (n={})",
                         targetRefreshHz,
                         std::chrono::duration<double, std::milli>(targetFrameTime).count(),
                         mean, stddev, *mm.first, *mm.second, intervalSamplesMs.size());
            intervalSamplesMs.clear();
            lastPacingLog = now;
        }
    }

    auto dt = std::chrono::duration<float>(now - lastFrameTime_).count();
    lastFrameTime_ = now;

    // Update input
    inputManager_->Update(dt);
    XrExtent2Df bounds = {};
    GetStageBounds(&bounds);

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
    bool hasProjectionLayer = false;
    uint32_t projectionLayerIndex = 0;

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
                if (hasProjectionLayer)
                {
                    static std::atomic_bool loggedUnsupportedProjections{false};
                    if (!loggedUnsupportedProjections.exchange(true))
                    {
                        spdlog::warn("OXRSys: multiple projection layer composition is unavailable");
                    }
                    return XR_ERROR_LAYER_LIMIT_EXCEEDED;
                }
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
                hasProjectionLayer = true;
                projectionLayerIndex = i;
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
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            {
                if (!instance_->IsExtensionEnabled(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
                {
                    return XR_ERROR_LAYER_INVALID;
                }
                XrResult result = ValidateCylinderLayer(
                    *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer));
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

    if (!hasProjectionLayer && frameEndInfo->layerCount > 0)
    {
        static std::atomic_bool loggedUnsupportedOverlayOnly{false};
        if (!loggedUnsupportedOverlayOnly.exchange(true))
        {
            spdlog::warn("OXRSys: quad and cylinder layers are composited only over a projection layer");
        }
        return XR_ERROR_LAYER_INVALID;
    }

    if (hasProjectionLayer && frameSource.IsStereoValid())
    {
        AppendOverlayLayers(*frameEndInfo, projectionLayerIndex, frameSource);
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
    std::lock_guard<std::mutex> lock(swapchainsMutex_);
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
        // Honor the per-view sub-rectangle: UE packs both eyes into one swapchain side-by-side.
        imageSource.sourceX = static_cast<uint32_t>(view.subImage.imageRect.offset.x);
        imageSource.sourceY = static_cast<uint32_t>(view.subImage.imageRect.offset.y);
        imageSource.sourceWidth = static_cast<uint32_t>(view.subImage.imageRect.extent.width);
        imageSource.sourceHeight = static_cast<uint32_t>(view.subImage.imageRect.extent.height);
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

XrResult Session::ValidateCylinderLayer(const XrCompositionLayerCylinderKHR& layer) const
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
    if (std::isnan(layer.radius) || layer.radius < 0.0f ||
        !std::isfinite(layer.centralAngle) || layer.centralAngle < 0.0f ||
        layer.centralAngle >= 2.0f * static_cast<float>(M_PI) ||
        !std::isfinite(layer.aspectRatio) || layer.aspectRatio <= 0.0f)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    return ValidateSwapchainSubImage(layer.subImage);
}

void Session::AppendOverlayLayers(const XrFrameEndInfo& frameEndInfo, uint32_t projectionLayerIndex,
                                  FrameSource& frameSource) const
{
    const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(
        frameEndInfo.layers[projectionLayerIndex]);
    auto* projectionSpace = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(projection.space));
    if (projectionSpace == nullptr)
    {
        return;
    }

    static std::atomic_bool loggedUnderlay{false};
    static std::atomic_bool loggedFirstOverlay{false};

    for (uint32_t i = 0; i < frameEndInfo.layerCount; ++i)
    {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo.layers[i];
        const bool isQuad = layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD;
        const bool isCylinder = layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
        if (!isQuad && !isCylinder)
        {
            continue;
        }
        if (i < projectionLayerIndex)
        {
            // Behind the projection layer. With an opaque projection it is hidden,
            // which is what the spec asks; under a source-alpha projection it would
            // show through, and we do not composite underlays yet.
            if (!loggedUnderlay.exchange(true))
            {
                spdlog::info("OXRSys: a quad/cylinder layer submitted before the projection layer is not drawn (underlays are not composited)");
            }
            continue;
        }

        FrameOverlayLayer overlay = {};
        XrSpace layerSpaceHandle = XR_NULL_HANDLE;
        XrPosef layerPose = {};
        XrEyeVisibility eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        XrSwapchainSubImage subImage = {};
        bool atInfinity = false;
        if (isQuad)
        {
            const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
            if (quad.size.width <= 0.0f || quad.size.height <= 0.0f)
            {
                continue;
            }
            overlay.shape = FrameOverlayLayer::Shape::Quad;
            overlay.width = quad.size.width;
            overlay.height = quad.size.height;
            overlay.sourceAlpha = (quad.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            overlay.unpremultipliedAlpha = (quad.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
            layerSpaceHandle = quad.space;
            layerPose = quad.pose;
            eyeVisibility = quad.eyeVisibility;
            subImage = quad.subImage;
        }
        else
        {
            const auto& cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
            if (!oxrsys::runtime::IsDrawableCylinder(cylinder.radius, cylinder.centralAngle, cylinder.aspectRatio))
            {
                continue;
            }
            atInfinity = oxrsys::runtime::IsInfiniteCylinder(cylinder.radius);
            overlay.shape = FrameOverlayLayer::Shape::Cylinder;
            overlay.radius = atInfinity ? 1.0f : cylinder.radius;
            overlay.centralAngle = cylinder.centralAngle;
            overlay.height = oxrsys::runtime::CylinderHeight(overlay.radius, cylinder.centralAngle, cylinder.aspectRatio);
            overlay.sourceAlpha = (cylinder.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            overlay.unpremultipliedAlpha = (cylinder.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
            layerSpaceHandle = cylinder.space;
            layerPose = cylinder.pose;
            eyeVisibility = cylinder.eyeVisibility;
            subImage = cylinder.subImage;
        }

        auto* layerSpace = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(layerSpaceHandle));
        if (layerSpace == nullptr)
        {
            continue;
        }
        XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
        if (layerSpace->LocateSpace(projectionSpace, frameEndInfo.displayTime, &location) != XR_SUCCESS ||
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0)
        {
            continue;
        }

        auto* swapchain = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(subImage.swapchain));
        if (swapchain == nullptr)
        {
            continue;
        }
        overlay.image = swapchain->GetLastReleasedFrameImageSource(subImage.imageArrayIndex);
        if (!overlay.image.IsValid())
        {
            continue;
        }
        overlay.image.sourceX = static_cast<uint32_t>(subImage.imageRect.offset.x);
        overlay.image.sourceY = static_cast<uint32_t>(subImage.imageRect.offset.y);
        overlay.image.sourceWidth = static_cast<uint32_t>(subImage.imageRect.extent.width);
        overlay.image.sourceHeight = static_cast<uint32_t>(subImage.imageRect.extent.height);

        switch (eyeVisibility)
        {
            case XR_EYE_VISIBILITY_LEFT: overlay.eyeMask = 1u; break;
            case XR_EYE_VISIBILITY_RIGHT: overlay.eyeMask = 2u; break;
            default: overlay.eyeMask = 3u; break;
        }
        for (uint32_t eye = 0; eye < 2; ++eye)
        {
            const XrCompositionLayerProjectionView& view = projection.views[eye];
            const glm::mat4 clip = atInfinity
                ? oxrsys::runtime::OverlayClipFromLayerAtInfinity(view.pose, view.fov, location.pose, layerPose)
                : oxrsys::runtime::OverlayClipFromLayer(view.pose, view.fov, location.pose, layerPose);
            std::memcpy(overlay.clipFromLayer[eye], &clip[0][0], sizeof(overlay.clipFromLayer[eye]));
        }

        if (!loggedFirstOverlay.exchange(true))
        {
            spdlog::info("OXRSys: compositing {} layer into the eye images ({}x{} m, image {}x{}, alpha={})",
                         isQuad ? "quad" : "cylinder",
                         isQuad ? overlay.width : overlay.radius * overlay.centralAngle, overlay.height,
                         overlay.image.sourceWidth, overlay.image.sourceHeight, overlay.sourceAlpha);
        }
        frameSource.overlays.push_back(std::move(overlay));
    }
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

    const XrSpaceLocation location = baseSpace->LocateWorldPose(inputManager_->GetHeadPose());
    viewState->type = XR_TYPE_VIEW_STATE;
    viewState->viewStateFlags = location.locationFlags != 0
        ? XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
              XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT
        : 0;

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

    // GetEyeViews returns the eye poses in absolute (STAGE-floor) world space.
    // Express them relative to the requested base reference space so the game
    // renders at the correct height for its chosen origin. Previously baseSpace
    // was ignored here, so views were always STAGE-absolute regardless of whether
    // the game asked for LOCAL/LOCAL_FLOOR/STAGE, and the STAGE floor-calibration
    // offset never reached the rendered image. For a STAGE base with the default
    // zero offset the transform is identity, so STAGE behaviour is unchanged.
    inputManager_->GetEyeViews(views, 2);
    for (uint32_t i = 0; i < 2; ++i)
    {
        views[i].pose = baseSpace->LocateWorldPose(views[i].pose).pose;
    }

    // Remember the exact head pose this frame is being rendered for, so the streamed frame can be
    // tagged with it at submission instead of a later re-prediction.
    lastRenderHeadPose_ = inputManager_->GetHeadPose();
    lastRenderHasPose_ = true;

    return XR_SUCCESS;
}

void Session::DelayFrameTransitionForTest() const
{
    if (endFrameAdvanceDelayUs_ > 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(endFrameAdvanceDelayUs_));
    }
}

void Session::AdvanceSessionStateAfterFrameSubmission()
{
    std::scoped_lock transitionLock(stateTransitionMutex_);
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
                DelayFrameTransitionForTest();
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
            DelayFrameTransitionForTest();
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
    const auto reject = [&](XrResult result) {
        spdlog::warn("Session: swapchain rejected (XrResult {}) format={} {}x{} array={} faces={} "
                     "samples={} mips={} usage=0x{:x} create=0x{:x}",
                     static_cast<int>(result), createInfo->format, createInfo->width,
                     createInfo->height, createInfo->arraySize, createInfo->faceCount,
                     createInfo->sampleCount, createInfo->mipCount,
                     static_cast<uint64_t>(createInfo->usageFlags),
                     static_cast<uint64_t>(createInfo->createFlags));
        *swapchain = XR_NULL_HANDLE;
        return result;
    };
    if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO ||
        createInfo->width == 0 ||
        createInfo->height == 0 ||
        createInfo->faceCount != 1 ||
        createInfo->arraySize == 0 ||
        createInfo->sampleCount != 1)
    {
        return reject(XR_ERROR_VALIDATION_FAILURE);
    }
    if (createInfo->mipCount != 1)
    {
        return reject(XR_ERROR_FEATURE_UNSUPPORTED);
    }

    if (teardownStarted_.load(std::memory_order_acquire))
    {
        *swapchain = XR_NULL_HANDLE;
        return XR_ERROR_SESSION_LOST;
    }

    const XrResult supportResult =
        oxrsys::swapchain::ValidateCreateInfo(graphicsContext_.api, *createInfo);
    if (supportResult != XR_SUCCESS)
    {
        return reject(supportResult);
    }

    *swapchain = XR_NULL_HANDLE;
    auto sc = std::make_unique<Swapchain>(graphicsContext_, createInfo);
    XrResult initializationResult = sc->InitializationResult();
    if (initializationResult != XR_SUCCESS)
    {
        return initializationResult;
    }
    sc->SetStreamingSnapshotDemand(streamingSnapshotDemand_);
    *swapchain = reinterpret_cast<XrSwapchain>(sc->GetHandle());
    {
        std::lock_guard<std::mutex> lock(swapchainsMutex_);
        if (teardownStarted_.load(std::memory_order_acquire))
        {
            *swapchain = XR_NULL_HANDLE;
            return XR_ERROR_SESSION_LOST;
        }
        swapchains_.push_back(std::move(sc));
    }
    return XR_SUCCESS;
}

XrResult Session::DestroySwapchain(Swapchain* swapchain)
{
    std::lock_guard<std::mutex> lock(swapchainsMutex_);
    for (auto it = swapchains_.begin(); it != swapchains_.end(); ++it)
    {
        if (it->get() == swapchain)
        {
            const XrResult result = (*it)->PrepareForDestroy();
            if (result != XR_SUCCESS)
            {
                return result;
            }
            swapchains_.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

bool Session::HasTrackingConnection() const
{
    return streamingServer_ != nullptr && streamingServer_->IsClientConnected();
}

XrResult Session::GetStageBounds(XrExtent2Df* bounds)
{
    std::lock_guard<std::mutex> lock(stageBoundsMutex_);
    *bounds = {};
    const bool available = HasTrackingConnection() && inputManager_->GetStageBounds(*bounds);
    if (available != stageBoundsAvailable_ || bounds->width != stageBounds_.width ||
        bounds->height != stageBounds_.height)
    {
        XrEventDataBuffer event = {};
        auto* change = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
        change->type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
        change->session = reinterpret_cast<XrSession>(handle_);
        change->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        change->changeTime = GetCurrentTime();
        change->poseValid = XR_FALSE;
        change->poseInPreviousSpace.orientation.w = 1.0f;
        instance_->PushEvent(event);
        stageBounds_ = *bounds;
        stageBoundsAvailable_ = available;
    }
    return available ? XR_SUCCESS : XR_SPACE_BOUNDS_UNAVAILABLE;
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

    const char* spaceName =
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? "VIEW" :
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" :
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? "LOCAL_FLOOR" :
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "UNKNOWN";
    spdlog::info("Session: game created reference space {} (offsetInSpace y={:.3f}), "
                 "stage_height_offset_m={:.3f}",
                 spaceName, createInfo->poseInReferenceSpace.position.y,
                 Config::Get().GetValues().stageHeightOffsetM);

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

    // Per-eye render resolution for the configured render_device — this must match the
    // recommendedImageRect the app renders into (see RenderBaseEyeResolution), otherwise the
    // encoder is sized off a stale default and the stream stays at that resolution.
    uint32_t width = 0;
    uint32_t height = 0;
    RenderBaseEyeResolution(width, height);
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
        streamingSnapshotDemand_->store(false, std::memory_order_release);
        return;
    }

    streamingSnapshotDemand_->store(
        streamingServer_->IsClientConnected(), std::memory_order_release);

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
