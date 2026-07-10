// SPDX-License-Identifier: MPL-2.0

#ifdef OXRSYS_HAS_ALVR

#include "AlvrStreamingBackend.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "AlvrNalFraming.h"
#include "AlvrSessionConfig.h"
#include "CodecSelect.h"
#include "Config.h"
#include "TrackingReceiver.h"
#include "VideoEncoder.h"
#include "alvr_server_core.h"

namespace fs = std::filesystem;

AlvrStreamingBackend::AlvrStreamingBackend()
    : trackingReceiver_(std::make_unique<TrackingReceiver>())
{
    // The receiver is intentionally not Start()ed: poses arrive via
    // InjectPacket() from the ALVR event loop, not from a UDP socket.
    frameQueue_.SetReleaseFrameCallback([](StreamingFrame& frame) { frame = {}; });
}

AlvrStreamingBackend::~AlvrStreamingBackend()
{
    Stop();
}

void AlvrStreamingBackend::EnsureSessionJson(const std::string& configDir)
{
    const fs::path sessionPath = fs::path(configDir) / "session.json";
    if (fs::exists(sessionPath))
    {
        return;
    }
    std::ofstream out(sessionPath);
    out << oxrsys::alvr::MinimalSessionJson();
    spdlog::info("OXRSys/ALVR: wrote initial session.json at {}", sessionPath.string());
}

bool AlvrStreamingBackend::ReadSessionJson(std::string& out) const
{
    std::ifstream in(sessionJsonPath_);
    if (!in)
    {
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

void AlvrStreamingBackend::SyncSessionSettings()
{
    std::string json;
    if (!ReadSessionJson(json))
    {
        return;
    }

    const uint32_t bitrateMbps = std::max(Config::Get().GetValues().bitrateMbps, 1u);
    const std::string updated = oxrsys::alvr::ApplySessionSettings(json, bitrateMbps);

    if (updated == json)
    {
        return; // already in sync
    }
    std::ofstream out(sessionJsonPath_, std::ios::trunc);
    if (!out)
    {
        spdlog::warn("OXRSys/ALVR: cannot rewrite {} to sync toml settings", sessionJsonPath_);
        return;
    }
    out << updated;
    spdlog::info(
        "OXRSys/ALVR: synced session.json from toml (ConstantMbps={}, max_buffering_frames=1.5)",
        bitrateMbps);
}

bool AlvrStreamingBackend::Start(uint32_t renderWidth, uint32_t renderHeight,
                                 uint32_t refreshRateHz)
{
    if (running_.load())
    {
        return true;
    }

    (void)renderWidth;
    (void)renderHeight;
    targetRefreshRateHz_.store(refreshRateHz);

    Config& config = Config::Get();
    const fs::path alvrDir = fs::path(config.appSupportDir) / "alvr";
    std::error_code ec;
    fs::create_directories(alvrDir, ec);
    if (ec)
    {
        spdlog::error("OXRSys/ALVR: cannot create {}: {}", alvrDir.string(), ec.message());
        return false;
    }

    EnsureSessionJson(alvrDir.string());

    const std::string configDir = alvrDir.string();
    const std::string sessionLogPath = (alvrDir / "session_log.txt").string();
    const std::string crashLogPath = (alvrDir / "crash_log.txt").string();

    sessionJsonPath_ = (alvrDir / "session.json").string();
    // Must precede alvr_initialize: server_core loads session.json once and
    // owns it afterwards.
    SyncSessionSettings();

    alvr_initialize_environment(configDir.c_str(), configDir.c_str());
    alvr_initialize_logging(sessionLogPath.c_str(), crashLogPath.c_str());

    const AlvrTargetConfig target = alvr_initialize();
    streamWidth_.store(target.stream_width);
    streamHeight_.store(target.stream_height);
    spdlog::info("OXRSys/ALVR: initialized (game_render={}x{} stream={}x{}, logs at {})",
                 target.game_render_width, target.game_render_height, target.stream_width,
                 target.stream_height, sessionLogPath);

    alvr_start_connection();

    InitInputIds();
    // ALVR's get_device_motion already extrapolates to the sample timestamp;
    // oxrsys-side prediction would double-predict.
    trackingReceiver_->SetPredictionHorizonMs(0.0f);

    running_.store(true);
    frameQueue_.Start();
    eventThread_ = std::thread(&AlvrStreamingBackend::EventThread, this);
    encodeThread_ = std::thread(&AlvrStreamingBackend::EncodeThread, this);
    return true;
}

void AlvrStreamingBackend::Stop()
{
    StopInternal(/*skipAlvrShutdown=*/false);
}

void AlvrStreamingBackend::StopForProcessExit()
{
    StopInternal(/*skipAlvrShutdown=*/true);
}

void AlvrStreamingBackend::StopInternal(bool skipAlvrShutdown)
{
    if (!running_.exchange(false))
    {
        return;
    }
    frameQueue_.Stop();
    if (encodeThread_.joinable())
    {
        encodeThread_.join();
    }
    if (eventThread_.joinable())
    {
        eventThread_.join();
    }
    if (encoder_)
    {
        // Flushes in-flight VideoToolbox frames; our submit callbacks may
        // still run during this call, so it precedes alvr_shutdown().
        encoder_->Shutdown();
        encoder_.reset();
    }
    connected_.store(false);
    if (skipAlvrShutdown)
    {
        // Process exit / dylib unload: alvr_shutdown() drops ServerCoreContext
        // (joins threads, waits for client disconnect, drops a tokio runtime),
        // which hangs or crashes at this point. Leave it to die with the
        // process; running_ is already false so a later Stop() is a no-op.
        spdlog::info("OXRSys/ALVR: threads stopped (alvr_shutdown skipped for process exit)");
        return;
    }
    // Tears down ServerCoreContext (tokio runtime + sockets). Must happen
    // after our own threads stopped touching alvr_* functions.
    alvr_shutdown();
    spdlog::info("OXRSys/ALVR: shut down");
}

void AlvrStreamingBackend::SendFrame(FrameSource frameSource,
                                     const float* /*renderHeadOrientation*/,
                                     const float* /*renderHeadPosition*/)
{
    StreamingFrame frame = {};
    // ALVR requires the video timestamp to equal a tracking sample timestamp
    // (its time domain) so the client can match the frame to the pose it was
    // rendered from. Session supplies the exact sample the app's poses came
    // from; fall back to latest-at-enqueue before the first sample lands.
    frame.timestampNs = frameSource.trackingSampleTimestampNs != 0
                            ? frameSource.trackingSampleTimestampNs
                            : static_cast<int64_t>(latestTrackingTimestampNs_.load());
    frame.source = std::move(frameSource);
    frame.valid = true;
    // ALVR latency decomposition: "present" marks the game handing the frame
    // to the runtime; it must precede "composed" (encode-thread pickup).
    alvr_report_present(static_cast<uint64_t>(frame.timestampNs), 0);
    frameQueue_.PushLatest(std::move(frame));
}

void AlvrStreamingBackend::ApplyHaptics(int hand, float amplitude, float durationSeconds,
                                        float frequencyHz)
{
    if (!connected_.load())
    {
        return;
    }
    alvr_send_haptics(hand == 0 ? handLeftId_ : handRightId_, durationSeconds, frequencyHz,
                      amplitude);
}

bool AlvrStreamingBackend::GetFramePacing(int64_t& outSleepNs)
{
    if (!connected_.load())
    {
        return false;
    }
    uint64_t untilVsyncNs = 0;
    if (!alvr_duration_until_next_vsync(&untilVsyncNs))
    {
        return false;
    }
    outSleepNs = static_cast<int64_t>(untilVsyncNs);
    return true;
}

void AlvrStreamingBackend::RefreshNegotiatedConfig()
{
    std::string json;
    if (!ReadSessionJson(json))
    {
        return;
    }

    // openvr_config holds the values server_core negotiated with the client
    // during the handshake.
    const oxrsys::alvr::NegotiatedConfig config = oxrsys::alvr::ParseNegotiatedConfig(json);
    if (config.width == 0 || config.height == 0)
    {
        return;
    }

    const bool changed = config.width != streamWidth_.load() ||
                         config.height != streamHeight_.load() ||
                         (config.fps != 0 && config.fps != targetRefreshRateHz_.load());
    streamWidth_.store(config.width);
    streamHeight_.store(config.height);
    if (config.fps != 0)
    {
        targetRefreshRateHz_.store(config.fps);
    }
    if (changed)
    {
        encoderResetPending_.store(true);
        spdlog::info("OXRSys/ALVR: negotiated stream {}x{} per eye @{}Hz (encoder reset queued)",
                     config.width, config.height, config.fps);
    }
}

bool AlvrStreamingBackend::EnsureEncoder()
{
    if (encoderResetPending_.exchange(false) && encoder_)
    {
        encoder_->Shutdown();
        encoder_.reset();
        submittedConfigNals_.clear();
    }
    if (encoder_ && encoder_->IsInitialized())
    {
        return true;
    }
    const uint32_t eyeWidth = streamWidth_.load();
    const uint32_t eyeHeight = streamHeight_.load();
    if (eyeWidth == 0 || eyeHeight == 0)
    {
        return false;
    }

    const oxr::protocol::VideoCodec codec = oxrsys::PreferredVideoCodec();
    encoderUsesH264_ = (codec == oxr::protocol::VideoCodec::H264);
    encoder_ = std::make_shared<VideoEncoder>();
    const uint32_t totalWidth = eyeWidth * 2; // side-by-side stereo
    const uint32_t bitrateMbps = Config::Get().GetValues().bitrateMbps;
    if (!encoder_->Initialize(totalWidth, eyeHeight, targetRefreshRateHz_.load(),
                              bitrateMbps, graphicsContext_, codec))
    {
        spdlog::error("OXRSys/ALVR: encoder init failed ({}x{} @{}Hz)", totalWidth,
                      eyeHeight, targetRefreshRateHz_.load());
        encoder_.reset();
        return false;
    }
    spdlog::info("OXRSys/ALVR: encoder ready {}x{} @{}Hz {}Mbps ({})", totalWidth, eyeHeight,
                 targetRefreshRateHz_.load(), bitrateMbps, encoderUsesH264_ ? "H.264" : "HEVC");
    return true;
}

double AlvrStreamingBackend::SubmitEncodedFrame(PendingEncodedFrame& frame)
{
    if (frame.data.empty())
    {
        return 0.0;
    }

    if (!frame.config.empty() && frame.config != submittedConfigNals_)
    {
        alvr_set_video_config_nals(encoderUsesH264_ ? ALVR_CODEC_H264 : ALVR_CODEC_HEVC,
                                   frame.config.data(), static_cast<int32_t>(frame.config.size()));
        submittedConfigNals_ = frame.config;
        spdlog::info("OXRSys/ALVR: sent codec config ({} bytes)", frame.config.size());
    }

    const auto sendStart = std::chrono::steady_clock::now();
    alvr_send_video_nal(frame.timestampNs, frame.data.data(),
                        static_cast<int32_t>(frame.data.size()), frame.isIdr);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sendStart)
        .count();
}

void AlvrStreamingBackend::EncodeThread()
{
    while (running_.load())
    {
        StreamingFrame frame = {};
        if (!frameQueue_.WaitPop(running_, frame))
        {
            continue;
        }
        if (!connected_.load() || !EnsureEncoder())
        {
            continue; // dropping the frame releases its graphics resources
        }

        if (keyframeRequested_.exchange(false))
        {
            encoder_->ForceKeyframe();
        }

        AlvrDynamicEncoderParams params = {};
        if (alvr_get_dynamic_encoder_params(&params))
        {
            const uint32_t mbps = std::max(1u, static_cast<uint32_t>(params.bitrate_bps / 1e6f));
            if (mbps != encoder_->GetBitrateMbps())
            {
                encoder_->SetBitrate(mbps);
            }
        }

        auto pending = std::make_shared<PendingEncodedFrame>();
        pending->timestampNs = static_cast<uint64_t>(frame.timestampNs);

        alvr_report_composed(pending->timestampNs, 0);

        const bool usesH264 = encoderUsesH264_;
        encoder_->EncodeStereo(
            std::move(frame.source), frame.timestampNs,
            [pending, usesH264](const uint8_t* data, size_t size, bool isKeyframe,
                                int64_t /*pts*/)
            { oxrsys::alvr::AppendEncodedNal(*pending, data, size, isKeyframe, usesH264); },
            [this, pending](const VideoEncoder::FrameMetrics& metrics)
            {
                double sendMs = 0.0;
                if (!metrics.frameDropped)
                {
                    sendMs = SubmitEncodedFrame(*pending);
                }
                RecordFrameMetrics(metrics, pending->data.size(), sendMs);
            });
    }
}

std::string AlvrStreamingBackend::GetClientName() const
{
    // Contains "Quest" so InputManager resolves the oculus/touch profile.
    return connected_.load() ? "Quest (ALVR)" : std::string();
}

void AlvrStreamingBackend::InitInputIds()
{
    headId_ = alvr_path_to_id("/user/head");
    handLeftId_ = alvr_path_to_id("/user/hand/left");
    handRightId_ = alvr_path_to_id("/user/hand/right");

    const auto add = [this](const char* path, ButtonKind kind)
    { buttonIds_.emplace(alvr_path_to_id(path), kind); };
    add("/user/hand/left/input/x/click", ButtonKind::LeftX);
    add("/user/hand/left/input/y/click", ButtonKind::LeftY);
    add("/user/hand/left/input/menu/click", ButtonKind::LeftMenu);
    add("/user/hand/left/input/thumbstick/click", ButtonKind::LeftThumbClick);
    add("/user/hand/left/input/thumbstick/x", ButtonKind::LeftThumbX);
    add("/user/hand/left/input/thumbstick/y", ButtonKind::LeftThumbY);
    add("/user/hand/left/input/trigger/click", ButtonKind::LeftTriggerClick);
    add("/user/hand/left/input/trigger/value", ButtonKind::LeftTriggerValue);
    add("/user/hand/left/input/squeeze/click", ButtonKind::LeftSqueezeClick);
    add("/user/hand/left/input/squeeze/value", ButtonKind::LeftSqueezeValue);
    add("/user/hand/right/input/a/click", ButtonKind::RightA);
    add("/user/hand/right/input/b/click", ButtonKind::RightB);
    add("/user/hand/right/input/system/click", ButtonKind::RightSystem);
    add("/user/hand/right/input/thumbstick/click", ButtonKind::RightThumbClick);
    add("/user/hand/right/input/thumbstick/x", ButtonKind::RightThumbX);
    add("/user/hand/right/input/thumbstick/y", ButtonKind::RightThumbY);
    add("/user/hand/right/input/trigger/click", ButtonKind::RightTriggerClick);
    add("/user/hand/right/input/trigger/value", ButtonKind::RightTriggerValue);
    add("/user/hand/right/input/squeeze/click", ButtonKind::RightSqueezeClick);
    add("/user/hand/right/input/squeeze/value", ButtonKind::RightSqueezeValue);
}

void AlvrStreamingBackend::DrainButtons()
{
    const uint64_t count = alvr_get_buttons(nullptr);
    if (count == 0)
    {
        return;
    }
    std::vector<AlvrButtonEntry> entries(count);
    const uint64_t written = alvr_get_buttons(entries.data());

    const auto setButton = [this](uint32_t flag, bool down)
    {
        if (down)
        {
            inputState_.buttons |= flag;
        }
        else
        {
            inputState_.buttons &= ~flag;
        }
    };

    for (uint64_t i = 0; i < written; ++i)
    {
        const auto it = buttonIds_.find(entries[i].id);
        if (it == buttonIds_.end())
        {
            // Log unmapped client button path ids, once per id per process
            // (the set is a function-local static that never resets), to catch
            // controller paths we haven't bound.
            static std::unordered_set<uint64_t> unmatchedLogged;
            if (unmatchedLogged.insert(entries[i].id).second)
            {
                spdlog::info("OXRSys/ALVR: unmapped button path id {:#x} (value bin={} f={})",
                             entries[i].id, entries[i].value.scalar, entries[i].value.float_);
            }
            continue;
        }
        const bool binary = entries[i].value.scalar;
        const float scalar = entries[i].value.float_;
        switch (it->second)
        {
            case ButtonKind::LeftX: setButton(oxr::protocol::BUTTON_X, binary); break;
            case ButtonKind::LeftY: setButton(oxr::protocol::BUTTON_Y, binary); break;
            case ButtonKind::LeftMenu:
                setButton(oxr::protocol::BUTTON_MENU, binary);
                break;
            case ButtonKind::LeftThumbClick:
                setButton(oxr::protocol::BUTTON_LEFT_THUMBSTICK, binary);
                break;
            case ButtonKind::LeftThumbX: inputState_.leftThumb[0] = scalar; break;
            case ButtonKind::LeftThumbY: inputState_.leftThumb[1] = scalar; break;
            case ButtonKind::LeftTriggerClick:
                setButton(oxr::protocol::BUTTON_LEFT_TRIGGER, binary);
                break;
            case ButtonKind::LeftTriggerValue:
                inputState_.leftTrigger = scalar;
                setButton(oxr::protocol::BUTTON_LEFT_TRIGGER, scalar > 0.5f);
                break;
            case ButtonKind::LeftSqueezeClick:
                setButton(oxr::protocol::BUTTON_LEFT_GRIP, binary);
                break;
            case ButtonKind::LeftSqueezeValue:
                inputState_.leftGrip = scalar;
                setButton(oxr::protocol::BUTTON_LEFT_GRIP, scalar > 0.5f);
                break;
            case ButtonKind::RightA: setButton(oxr::protocol::BUTTON_A, binary); break;
            case ButtonKind::RightB: setButton(oxr::protocol::BUTTON_B, binary); break;
            case ButtonKind::RightSystem: break; // reserved by the system
            case ButtonKind::RightThumbClick:
                setButton(oxr::protocol::BUTTON_RIGHT_THUMBSTICK, binary);
                break;
            case ButtonKind::RightThumbX: inputState_.rightThumb[0] = scalar; break;
            case ButtonKind::RightThumbY: inputState_.rightThumb[1] = scalar; break;
            case ButtonKind::RightTriggerClick:
                setButton(oxr::protocol::BUTTON_RIGHT_TRIGGER, binary);
                break;
            case ButtonKind::RightTriggerValue:
                inputState_.rightTrigger = scalar;
                setButton(oxr::protocol::BUTTON_RIGHT_TRIGGER, scalar > 0.5f);
                break;
            case ButtonKind::RightSqueezeClick:
                setButton(oxr::protocol::BUTTON_RIGHT_GRIP, binary);
                break;
            case ButtonKind::RightSqueezeValue:
                inputState_.rightGrip = scalar;
                setButton(oxr::protocol::BUTTON_RIGHT_GRIP, scalar > 0.5f);
                break;
        }
    }
}

void AlvrStreamingBackend::InjectTrackingSample(uint64_t sampleTimestampNs)
{
    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = static_cast<int64_t>(sampleTimestampNs);

    AlvrDeviceMotion motion = {};
    if (!alvr_get_device_motion(headId_, sampleTimestampNs, &motion))
    {
        return; // no sample yet; skip rather than inject a zero head pose
    }
    memcpy(packet.headPosition, motion.pose.position, sizeof(packet.headPosition));
    packet.headOrientation[0] = motion.pose.orientation.x;
    packet.headOrientation[1] = motion.pose.orientation.y;
    packet.headOrientation[2] = motion.pose.orientation.z;
    packet.headOrientation[3] = motion.pose.orientation.w;
    memcpy(packet.headLinearVelocity, motion.linear_velocity, sizeof(packet.headLinearVelocity));
    memcpy(packet.headAngularVelocity, motion.angular_velocity,
           sizeof(packet.headAngularVelocity));

    if (alvr_get_device_motion(handLeftId_, sampleTimestampNs, &motion))
    {
        packet.trackingFlags |= oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
        memcpy(packet.leftControllerPos, motion.pose.position, sizeof(packet.leftControllerPos));
        packet.leftControllerRot[0] = motion.pose.orientation.x;
        packet.leftControllerRot[1] = motion.pose.orientation.y;
        packet.leftControllerRot[2] = motion.pose.orientation.z;
        packet.leftControllerRot[3] = motion.pose.orientation.w;
    }
    if (alvr_get_device_motion(handRightId_, sampleTimestampNs, &motion))
    {
        packet.trackingFlags |= oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
        memcpy(packet.rightControllerPos, motion.pose.position,
               sizeof(packet.rightControllerPos));
        packet.rightControllerRot[0] = motion.pose.orientation.x;
        packet.rightControllerRot[1] = motion.pose.orientation.y;
        packet.rightControllerRot[2] = motion.pose.orientation.z;
        packet.rightControllerRot[3] = motion.pose.orientation.w;
    }

    packet.buttonState = inputState_.buttons;
    packet.leftTrigger = inputState_.leftTrigger;
    packet.rightTrigger = inputState_.rightTrigger;
    packet.leftGrip = inputState_.leftGrip;
    packet.rightGrip = inputState_.rightGrip;
    packet.leftThumbstick[0] = inputState_.leftThumb[0];
    packet.leftThumbstick[1] = inputState_.leftThumb[1];
    packet.rightThumbstick[0] = inputState_.rightThumb[0];
    packet.rightThumbstick[1] = inputState_.rightThumb[1];
    packet.ipd = inputState_.ipd;
    memcpy(packet.eyeFov, inputState_.eyeFov, sizeof(packet.eyeFov));
    // Aim poses left zero: the runtime falls back to the grip pose.

    trackingReceiver_->InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
}

namespace
{
double Percentile(std::vector<double>& values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }
    const size_t idx = std::min(values.size() - 1,
                                static_cast<size_t>(fraction * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(idx), values.end());
    return values[idx];
}
} // namespace

void AlvrStreamingBackend::RecordFrameMetrics(const VideoEncoder::FrameMetrics& metrics,
                                              size_t nalBytes, double sendMs)
{
    std::lock_guard<std::mutex> lock(encodeStats_.mutex);
    auto& s = encodeStats_;
    const auto now = std::chrono::steady_clock::now();
    if (s.windowStart == std::chrono::steady_clock::time_point{})
    {
        s.windowStart = now;
    }
    s.frames++;
    if (metrics.frameDropped)
    {
        s.drops++;
    }
    else
    {
        if (metrics.keyframe)
        {
            s.keyframes++;
        }
        s.totalMs.push_back(metrics.totalLatencyMs);
        s.callbackMs.push_back(metrics.callbackLatencyMs);
        s.gpuCopyMs.push_back(metrics.gpuCopyMs);
        s.sendMs.push_back(sendMs);
        s.nalKb.push_back(static_cast<double>(nalBytes) / 1024.0);
    }
    if (now - s.windowStart < std::chrono::seconds(1))
    {
        return;
    }
    spdlog::info("OXRSys/ALVR: enc1s n={} drop={} key={} totalMs p50={:.1f} p95={:.1f} max={:.1f} "
                 "cbMs p95={:.1f} gpuMs p95={:.2f} sendMs p50={:.2f} p95={:.2f} max={:.1f} "
                 "nalKB p50={:.0f} max={:.0f}",
                 s.frames, s.drops, s.keyframes, Percentile(s.totalMs, 0.5),
                 Percentile(s.totalMs, 0.95),
                 s.totalMs.empty() ? 0.0 : *std::max_element(s.totalMs.begin(), s.totalMs.end()),
                 Percentile(s.callbackMs, 0.95), Percentile(s.gpuCopyMs, 0.95),
                 Percentile(s.sendMs, 0.5), Percentile(s.sendMs, 0.95),
                 s.sendMs.empty() ? 0.0 : *std::max_element(s.sendMs.begin(), s.sendMs.end()),
                 Percentile(s.nalKb, 0.5),
                 s.nalKb.empty() ? 0.0 : *std::max_element(s.nalKb.begin(), s.nalKb.end()));
    s.windowStart = now;
    s.frames = s.drops = s.keyframes = 0;
    s.totalMs.clear();
    s.callbackMs.clear();
    s.gpuCopyMs.clear();
    s.sendMs.clear();
    s.nalKb.clear();
}

void AlvrStreamingBackend::EventThread()
{
    constexpr uint64_t kPollTimeoutNs = 100ull * 1000 * 1000;
    // alvr_poll_event returns true without writing the event for variants the
    // C API swallows (GameRenderLatencyFeedback fires per frame once video
    // flows). Pre-fill with a sentinel so those polls are skipped.
    constexpr uint8_t kUnwrittenTag = 0xFF;

    // Staleness watchdog: tracking normally arrives at client refresh rate;
    // >500ms of silence while connected means the headset slept or the
    // server-side receive stalled. Only EventThread touches these.
    using SteadyClock = std::chrono::steady_clock;
    SteadyClock::time_point lastTrackingArrival{};
    bool trackingStaleWarned = false;

    while (running_.load())
    {
        if (connected_.load() && !trackingStaleWarned &&
            lastTrackingArrival != SteadyClock::time_point{} &&
            SteadyClock::now() - lastTrackingArrival > std::chrono::milliseconds(500))
        {
            spdlog::warn("OXRSys/ALVR: tracking stale >500ms (headset asleep or receive stall)");
            trackingStaleWarned = true;
        }

        AlvrEvent event{};
        event.tag = kUnwrittenTag;
        if (!alvr_poll_event(&event, kPollTimeoutNs) || event.tag == kUnwrittenTag)
        {
            continue;
        }

        switch (event.tag)
        {
            case ALVR_EVENT_CLIENT_CONNECTED:
                RefreshNegotiatedConfig();
                connected_.store(true);
                spdlog::info("OXRSys/ALVR: client connected");
                break;
            case ALVR_EVENT_CLIENT_DISCONNECTED:
                connected_.store(false);
                lastTrackingArrival = {};
                trackingStaleWarned = false;
                spdlog::info("OXRSys/ALVR: client disconnected");
                break;
            case ALVR_EVENT_TRACKING_UPDATED:
                if (trackingStaleWarned)
                {
                    spdlog::info(
                        "OXRSys/ALVR: tracking resumed after {:.1f}s stall",
                        std::chrono::duration<double>(SteadyClock::now() - lastTrackingArrival)
                            .count());
                    trackingStaleWarned = false;
                }
                lastTrackingArrival = SteadyClock::now();
                latestTrackingTimestampNs_.store(event.tracking_updated.sample_timestamp_ns);
                InjectTrackingSample(event.tracking_updated.sample_timestamp_ns);
                break;
            case ALVR_EVENT_BUTTONS_UPDATED:
                DrainButtons();
                break;
            case ALVR_EVENT_REQUEST_IDR:
                if (keyframeRequestLimiter_.Accept(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()))
                {
                    keyframeRequested_.store(true);
                }
                break;
            case ALVR_EVENT_VIEWS_CONFIG:
                inputState_.ipd = event.views_config.local_view_transform[1].position[0] -
                                  event.views_config.local_view_transform[0].position[0];
                inputState_.eyeFov[0] = event.views_config.fov[0].left;
                inputState_.eyeFov[1] = event.views_config.fov[0].right;
                inputState_.eyeFov[2] = event.views_config.fov[0].up;
                inputState_.eyeFov[3] = event.views_config.fov[0].down;
                spdlog::info("OXRSys/ALVR: views config (fov0 l={:.2f} r={:.2f}, ipd~{:.4f})",
                             event.views_config.fov[0].left, event.views_config.fov[0].right,
                             inputState_.ipd);
                break;
            case ALVR_EVENT_BATTERY:
                spdlog::debug("OXRSys/ALVR: battery {:.0f}%",
                              event.battery.info.gauge_value * 100.0f);
                break;
            case ALVR_EVENT_PLAYSPACE_SYNC:
                spdlog::info("OXRSys/ALVR: playspace {:.2f}x{:.2f}m",
                             event.playspace_sync.bounds[0], event.playspace_sync.bounds[1]);
                break;
            case ALVR_EVENT_CAPTURE_FRAME:
                break;
            case ALVR_EVENT_RESTART_PENDING:
                // Emitted when negotiation changed restart-flagged settings.
                // With no SteamVR to restart, the connection loop retries and
                // succeeds against the updated session (verified in Stage 0).
                spdlog::warn("OXRSys/ALVR: restart pending (ignored; reconnect handles it)");
                break;
            case ALVR_EVENT_SHUTDOWN_PENDING:
                spdlog::warn("OXRSys/ALVR: shutdown pending");
                connected_.store(false);
                break;
            default:
                break;
        }
    }
}

#endif // OXRSYS_HAS_ALVR
