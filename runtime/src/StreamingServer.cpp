// SPDX-License-Identifier: MPL-2.0

#include "StreamingServer.h"
#include "Config.h"
#include "RuntimeSockets.h"
#include "RuntimeStatus.h"
#include "StreamingTransportPolicy.h"
#include "Swapchain.h"
#include "TrackingReceiver.h"
#include "VideoEncoder.h"
#include <oxrsys/protocol/Foveation.h>
#include <oxrsys/protocol/FecCodec.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <system_error>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace
{

using Clock = std::chrono::steady_clock;
using SocketHandle = oxrsys::runtime_socket::SocketHandle;

#if defined(MSG_DONTWAIT)
constexpr int kBestEffortSendFlags = MSG_DONTWAIT;
#else
constexpr int kBestEffortSendFlags = 0;
#endif

constexpr auto kTcpSendDeadline = std::chrono::milliseconds(100);
constexpr auto kTcpSendRetrySleep = std::chrono::milliseconds(1);
constexpr int64_t kStreamConfigAckTimeoutNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::milliseconds(500)).count();
constexpr uint32_t kStreamConfigMaxRetries = 2;

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

size_t BoundedStringLength(const char* value, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && value[length] != '\0')
    {
        length++;
    }
    return length;
}

oxr::protocol::FoveationPreset ParseFoveationPreset(const std::string& value)
{
    if (value == "light")
    {
        return oxr::protocol::FoveationPreset::Light;
    }
    if (value == "medium")
    {
        return oxr::protocol::FoveationPreset::Medium;
    }
    if (value == "high")
    {
        return oxr::protocol::FoveationPreset::High;
    }
    return oxr::protocol::FoveationPreset::Off;
}

oxr::protocol::ClientFoveationPreset ParseClientFoveationPreset(const std::string& value)
{
    if (value == "light")
    {
        return oxr::protocol::ClientFoveationPreset::Light;
    }
    if (value == "medium")
    {
        return oxr::protocol::ClientFoveationPreset::Medium;
    }
    if (value == "high")
    {
        return oxr::protocol::ClientFoveationPreset::High;
    }
    return oxr::protocol::ClientFoveationPreset::Off;
}

oxr::protocol::ClientReprojectionMode ParseClientReprojectionMode(const std::string& value)
{
    if (value == "off")
    {
        return oxr::protocol::ClientReprojectionMode::Off;
    }
    if (value == "pose_warp")
    {
        return oxr::protocol::ClientReprojectionMode::PoseWarp;
    }
    return oxr::protocol::ClientReprojectionMode::Pose;
}

const char* ClientReprojectionModeName(oxr::protocol::ClientReprojectionMode mode)
{
    switch (mode)
    {
        case oxr::protocol::ClientReprojectionMode::Off:
            return "off";
        case oxr::protocol::ClientReprojectionMode::PoseWarp:
            return "pose_warp";
        case oxr::protocol::ClientReprojectionMode::Pose:
        default:
            return "pose";
    }
}

bool HasClientFoveationOverride(const std::string& value)
{
    return value == "off" || value == "light" || value == "medium" || value == "high";
}

bool PlatformSupportsFoveatedEncoding()
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool HasClientCapability(const oxr::protocol::ClientConnect& clientConnect, uint32_t flag)
{
    return (clientConnect.clientCapabilities & flag) != 0;
}

const char* VideoCodecName(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return "h264";
        case oxr::protocol::VideoCodec::AV1:
            return "av1";
        case oxr::protocol::VideoCodec::H265:
        default:
            return "h265";
    }
}

uint32_t VideoCodecCapabilityFlag(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_H264;
        case oxr::protocol::VideoCodec::AV1:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_AV1;
        case oxr::protocol::VideoCodec::H265:
        default:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_H265;
    }
}

bool RuntimeSupportsVideoCodec(oxr::protocol::VideoCodec codec)
{
    return codec == oxr::protocol::VideoCodec::H265 ||
           codec == oxr::protocol::VideoCodec::H264;
}

bool ClientSupportsVideoCodec(const oxr::protocol::ClientConnect& clientConnect,
                              oxr::protocol::VideoCodec codec)
{
    if (clientConnect.supportedCodecs == 0)
    {
        return codec == oxr::protocol::VideoCodec::H265;
    }
    return (clientConnect.supportedCodecs & VideoCodecCapabilityFlag(codec)) != 0;
}

oxr::protocol::VideoCodec ParseConfiguredVideoCodec(const std::string& value)
{
    if (value == "h264")
    {
        return oxr::protocol::VideoCodec::H264;
    }
    return oxr::protocol::VideoCodec::H265;
}

oxr::protocol::VideoCodec SelectVideoCodec(const ConfigValues& config,
                                           const oxr::protocol::ClientConnect& clientConnect)
{
    if (config.videoCodec == "auto")
    {
        const auto preferred = static_cast<oxr::protocol::VideoCodec>(clientConnect.preferredCodec);
        if (RuntimeSupportsVideoCodec(preferred) && ClientSupportsVideoCodec(clientConnect, preferred))
        {
            return preferred;
        }
        if (ClientSupportsVideoCodec(clientConnect, oxr::protocol::VideoCodec::H265))
        {
            return oxr::protocol::VideoCodec::H265;
        }
        if (ClientSupportsVideoCodec(clientConnect, oxr::protocol::VideoCodec::H264))
        {
            return oxr::protocol::VideoCodec::H264;
        }
        return oxr::protocol::VideoCodec::H265;
    }

    const oxr::protocol::VideoCodec requested = ParseConfiguredVideoCodec(config.videoCodec);
    if (RuntimeSupportsVideoCodec(requested) && ClientSupportsVideoCodec(clientConnect, requested))
    {
        return requested;
    }
    if (ClientSupportsVideoCodec(clientConnect, oxr::protocol::VideoCodec::H265))
    {
        return oxr::protocol::VideoCodec::H265;
    }
    if (ClientSupportsVideoCodec(clientConnect, oxr::protocol::VideoCodec::H264))
    {
        return oxr::protocol::VideoCodec::H264;
    }
    return oxr::protocol::VideoCodec::H265;
}

bool IsGraphicsContextValid(const GraphicsContext& context);

struct StreamLayout
{
    float resolutionScale = 1.0f;
    uint32_t scaledWidth = 0;
    uint32_t scaledHeight = 0;
    uint32_t encodedWidth = 0;
    uint32_t encodedHeight = 0;
    uint32_t foveatedTargetEyeWidth = 0;
    uint32_t foveatedTargetEyeHeight = 0;
    bool foveatedEncodingActive = false;
    oxr::protocol::FoveationLayout foveationLayout = {};
    oxr::protocol::FoveationPreset foveationPreset = oxr::protocol::FoveationPreset::Off;
};

StreamLayout BuildStreamLayout(uint32_t renderWidth,
                               uint32_t renderHeight,
                               float requestedScale,
                               const ConfigValues& config,
                               const GraphicsContext& graphicsContext)
{
    StreamLayout layout = {};
    layout.resolutionScale = std::clamp(requestedScale, 0.25f, 1.0f);
    layout.scaledWidth = static_cast<uint32_t>(renderWidth * layout.resolutionScale);
    layout.scaledHeight = static_cast<uint32_t>(renderHeight * layout.resolutionScale);
    layout.scaledWidth = std::max((layout.scaledWidth + 15) & ~15u, 16u);
    layout.scaledHeight = std::max((layout.scaledHeight + 15) & ~15u, 16u);
    layout.encodedWidth = layout.scaledWidth * 2;
    layout.encodedHeight = layout.scaledHeight;
    layout.foveatedTargetEyeWidth = layout.scaledWidth;
    layout.foveatedTargetEyeHeight = layout.scaledHeight;

    layout.foveationPreset = ParseFoveationPreset(config.foveatedEncodingPreset);
    if (layout.foveationPreset != oxr::protocol::FoveationPreset::Off &&
        PlatformSupportsFoveatedEncoding() &&
        IsGraphicsContextValid(graphicsContext) &&
        VideoEncoder::SupportsFoveatedEncoding(graphicsContext))
    {
        const bool unscaledFoveationTarget =
            std::fabs(layout.resolutionScale - 1.0f) <= 0.001f;
        layout.foveatedTargetEyeWidth =
            unscaledFoveationTarget ? renderWidth : layout.scaledWidth;
        layout.foveatedTargetEyeHeight =
            unscaledFoveationTarget ? renderHeight : layout.scaledHeight;
        layout.foveationLayout = oxr::protocol::CalculateFoveationLayout(
            layout.foveatedTargetEyeWidth,
            layout.foveatedTargetEyeHeight,
            layout.foveationPreset);
        if (unscaledFoveationTarget &&
            oxr::protocol::IsFoveatedEncodingLayoutUsable(
                layout.foveationLayout, renderWidth, renderHeight))
        {
            layout.encodedWidth = layout.foveationLayout.optimizedEyeWidth * 2;
            layout.encodedHeight = layout.foveationLayout.optimizedEyeHeight;
            layout.foveatedEncodingActive = layout.encodedWidth < renderWidth * 2 ||
                                            layout.encodedHeight < renderHeight;
        }
        else
        {
            layout.foveatedTargetEyeWidth = layout.scaledWidth;
            layout.foveatedTargetEyeHeight = layout.scaledHeight;
            layout.foveationLayout = oxr::protocol::CalculateFoveationLayout(
                layout.foveatedTargetEyeWidth,
                layout.foveatedTargetEyeHeight,
                oxr::protocol::FoveationPreset::Off);
        }
    }
    else
    {
        layout.foveationPreset = oxr::protocol::FoveationPreset::Off;
        layout.foveationLayout = oxr::protocol::CalculateFoveationLayout(
            layout.foveatedTargetEyeWidth,
            layout.foveatedTargetEyeHeight,
            oxr::protocol::FoveationPreset::Off);
    }

    return layout;
}

VideoEncoder::FoveationSettings BuildEncoderFoveationSettings(
    bool enabled,
    const oxr::protocol::FoveationLayout& layout)
{
    VideoEncoder::FoveationSettings settings = {};
    settings.enabled = enabled;
    settings.targetEyeWidth = layout.targetEyeWidth;
    settings.targetEyeHeight = layout.targetEyeHeight;
    settings.eyeWidthRatio = layout.eyeWidthRatio;
    settings.eyeHeightRatio = layout.eyeHeightRatio;
    settings.centerSizeX = layout.parameters.centerSizeX;
    settings.centerSizeY = layout.parameters.centerSizeY;
    settings.centerShiftX = layout.parameters.centerShiftX;
    settings.centerShiftY = layout.parameters.centerShiftY;
    settings.edgeRatioX = layout.parameters.edgeRatioX;
    settings.edgeRatioY = layout.parameters.edgeRatioY;
    return settings;
}

bool IsGraphicsContextValid(const GraphicsContext& context)
{
    switch (context.api)
    {
        case GraphicsApi::Metal:
            return context.metalDevice != nullptr;
        case GraphicsApi::Vulkan:
            return context.vulkan.device != nullptr;
        case GraphicsApi::OpenGL:
            return context.openGL.context != nullptr;
        case GraphicsApi::D3D11:
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
            return context.d3d11.device != nullptr && context.d3d11.immediateContext != nullptr;
#else
            return false;
#endif
        case GraphicsApi::D3D12:
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
            return context.d3d12.device != nullptr && context.d3d12.queue != nullptr;
#else
            return false;
#endif
    }
    return false;
}

bool SendAll(SocketHandle socket, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sentTotal = 0;
    auto sendDeadline = Clock::now() + kTcpSendDeadline;
    while (sentTotal < size)
    {
        int sent = oxrsys::runtime_socket::Send(socket, bytes + sentTotal, size - sentTotal, 0);
        if (sent < 0)
        {
            if (oxrsys::runtime_socket::IsInterruptedOrWouldBlock())
            {
                if (Clock::now() >= sendDeadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(kTcpSendRetrySleep);
                continue;
            }
            return false;
        }
        if (sent == 0)
        {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
        sendDeadline = Clock::now() + kTcpSendDeadline;
    }
    return true;
}

bool ReadAll(SocketHandle socket, void* data, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t receivedTotal = 0;
    while (receivedTotal < size)
    {
        int received = oxrsys::runtime_socket::Receive(
            socket, bytes + receivedTotal, size - receivedTotal, 0);
        if (received < 0)
        {
            if (oxrsys::runtime_socket::IsInterruptedOrWouldBlock())
            {
                continue;
            }
            return false;
        }
        if (received == 0)
        {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }
    return true;
}

bool ReadTcpRecord(SocketHandle socket, oxr::protocol::TcpRecordHeader& header,
                   std::vector<uint8_t>& payload)
{
    if (!ReadAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (header.magic != oxr::protocol::TCP_RECORD_MAGIC ||
        header.version != oxr::protocol::TCP_RECORD_VERSION ||
        header.payloadSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }

    payload.clear();
    payload.resize(header.payloadSize);
    if (header.payloadSize == 0)
    {
        return true;
    }
    return ReadAll(socket, payload.data(), payload.size());
}

void ConfigureTcpSocket(SocketHandle socket)
{
    oxrsys::runtime_socket::SetTcpNoDelay(socket);
    oxrsys::runtime_socket::SetNoSigpipe(socket);
    oxrsys::runtime_socket::SetSendTimeout(socket, 0, 100000);
}

void CloseTcpSocket(SocketHandle& socket)
{
    oxrsys::runtime_socket::ShutdownAndClose(socket);
}

void JoinWorkerThread(std::thread& worker, const char* name)
{
    if (!worker.joinable())
    {
        return;
    }
    if (worker.get_id() == std::this_thread::get_id())
    {
        spdlog::warn("StreamingServer: {} thread requested its own shutdown; detaching", name);
        worker.detach();
        return;
    }
    try
    {
        worker.join();
    }
    catch (const std::system_error& error)
    {
        spdlog::warn("StreamingServer: failed to join {} thread: {}", name, error.what());
        if (worker.joinable())
        {
            worker.detach();
        }
    }
}

SocketHandle CreateLoopbackListener(uint16_t port)
{
    SocketHandle listenSocket = oxrsys::runtime_socket::Create(AF_INET, SOCK_STREAM, 0);
    if (!oxrsys::runtime_socket::IsValid(listenSocket))
    {
        return oxrsys::runtime_socket::InvalidSocket;
    }

    oxrsys::runtime_socket::SetReuseAddress(listenSocket);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        oxrsys::runtime_socket::Close(listenSocket);
        return oxrsys::runtime_socket::InvalidSocket;
    }

    if (listen(listenSocket, 1) < 0)
    {
        oxrsys::runtime_socket::Close(listenSocket);
        return oxrsys::runtime_socket::InvalidSocket;
    }

    return listenSocket;
}

SocketHandle AcceptWithTimeout(SocketHandle listenSocket)
{
    int ready = oxrsys::runtime_socket::SelectOneReadable(listenSocket, 1, 0);
    if (ready <= 0)
    {
        return oxrsys::runtime_socket::InvalidSocket;
    }

    SocketHandle clientSocket = accept(listenSocket, nullptr, nullptr);
    if (oxrsys::runtime_socket::IsValid(clientSocket))
    {
        ConfigureTcpSocket(clientSocket);
    }
    return clientSocket;
}

struct MetricSummary
{
    double average = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    size_t count = 0;
};

MetricSummary Summarize(std::vector<double>& values)
{
    MetricSummary summary = {};
    if (values.empty())
    {
        return summary;
    }

    std::sort(values.begin(), values.end());
    summary.count = values.size();
    summary.average = std::accumulate(values.begin(), values.end(), 0.0) / summary.count;

    auto percentile = [&values](double p) {
        size_t index = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * (values.size() - 1));
        return values[index];
    };

    summary.p50 = percentile(0.50);
    summary.p95 = percentile(0.95);
    return summary;
}

class TelemetryWindow
{
public:
    void Add(double sample)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(sample);
    }

    MetricSummary Consume()
    {
        std::vector<double> samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples.swap(samples_);
        }
        return Summarize(samples);
    }

private:
    std::mutex mutex_;
    std::vector<double> samples_;
};

struct EncodeTelemetry
{
    TelemetryWindow queueWaitMs;
    TelemetryWindow gpuCopyMs;
    TelemetryWindow encodeSubmitMs;
    TelemetryWindow callbackLatencyMs;
    TelemetryWindow totalPipelineMs;
    std::mutex logMutex;
    Clock::time_point lastLogTime = Clock::now();
};

} // namespace

StreamingServer::StreamingServer()
    : frameQueue_([this](StreamingFrame& frame) { ReleaseStreamingFrame(frame); }),
      packetDispatchState_(std::make_shared<PacketDispatchState>())
{
}

StreamingServer::~StreamingServer()
{
    Stop();
}

std::shared_ptr<StreamingServer::CallbackAccess> StreamingServer::GetCallbackAccess()
{
    std::lock_guard<std::mutex> lock(callbackAccessMutex_);
    return callbackAccess_;
}

std::shared_ptr<StreamingServer::CallbackAccess> StreamingServer::RenewCallbackAccess()
{
    auto next = std::make_shared<CallbackAccess>();
    next->server = this;
    next->accepting = true;

    std::shared_ptr<CallbackAccess> previous;
    {
        std::lock_guard<std::mutex> lock(callbackAccessMutex_);
        previous = std::move(callbackAccess_);
        callbackAccess_ = next;
    }
    InvalidateCallbackAccess(previous);
    return next;
}

void StreamingServer::InvalidateCallbackAccess()
{
    std::shared_ptr<CallbackAccess> previous;
    {
        std::lock_guard<std::mutex> lock(callbackAccessMutex_);
        previous = std::move(callbackAccess_);
        callbackAccess_.reset();
    }
    InvalidateCallbackAccess(previous);
}

void StreamingServer::InvalidateCallbackAccess(const std::shared_ptr<CallbackAccess>& access)
{
    if (access == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(access->mutex);
    access->accepting = false;
    access->server = nullptr;
}

bool StreamingServer::Start(uint32_t renderWidth, uint32_t renderHeight, uint32_t refreshRateHz)
{
    (void)refreshRateHz;
    if (running_.load())
    {
        return false;
    }

    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;

    const ConfigValues config = Config::Get().GetValues();
    refreshRateHz_ = config.refreshRateHz;
    targetRefreshRateHz_.store(refreshRateHz_);
    wifiEnabled_ = config.streamingTransport != "usb_adb";
    usbAdbEnabled_ = config.streamingTransport != "wifi";
    StreamLayout streamLayout = BuildStreamLayout(
        renderWidth, renderHeight, config.resolutionScale, config, graphicsContext_);
    {
        std::lock_guard<std::mutex> layoutLock(streamLayoutMutex_);
        streamLayout_.activeResolutionScale = streamLayout.resolutionScale;
        streamLayout_.scaledWidth = streamLayout.scaledWidth;
        streamLayout_.scaledHeight = streamLayout.scaledHeight;
        streamLayout_.encodedWidth = streamLayout.encodedWidth;
        streamLayout_.encodedHeight = streamLayout.encodedHeight;
        streamLayout_.foveatedTargetEyeWidth = streamLayout.foveatedTargetEyeWidth;
        streamLayout_.foveatedTargetEyeHeight = streamLayout.foveatedTargetEyeHeight;
        streamLayout_.foveatedEncodingActive = streamLayout.foveatedEncodingActive;
        streamLayout_.streamConfigSequence = 0;
    }
    clientFoveatedEncodingActive_.store(false);
    clientSupportsFoveatedEncoding_.store(false);
    clientSupportsStreamReconfigure_.store(false);
    clientSupportsMixedRealityPassthrough_.store(false);
    clientSupportsSpatialEntity_.store(false);
    activeVideoCodec_.store(oxr::protocol::VideoCodec::H265);
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        ResetPendingStreamConfigLocked();
    }

    if (streamLayout.foveationPreset != oxr::protocol::FoveationPreset::Off &&
        !streamLayout.foveatedEncodingActive)
    {
        const bool unavailable =
            !PlatformSupportsFoveatedEncoding() ||
            !IsGraphicsContextValid(graphicsContext_) ||
            !VideoEncoder::SupportsFoveatedEncoding(graphicsContext_);
        if (unavailable)
        {
            spdlog::warn("StreamingServer: foveated encoding preset '{}' is configured but unavailable; advertising normal video",
                         config.foveatedEncodingPreset);
        }
        else
        {
            spdlog::warn("StreamingServer: foveated encoding preset '{}' is configured but resolution_scale={:.2f} cannot be announced coherently; advertising normal video",
                         config.foveatedEncodingPreset,
                         streamLayout.resolutionScale);
        }
    }

    spdlog::info("StreamingServer: Resolution scaling {:.0f}%: {}x{} -> {}x{} per eye, encoded {}x{}{}",
                  streamLayout.resolutionScale * 100.0f,
                  renderWidth,
                  renderHeight,
                  streamLayout.scaledWidth,
                  streamLayout.scaledHeight,
                  streamLayout.encodedWidth,
                  streamLayout.encodedHeight,
                  streamLayout.foveatedEncodingActive ? " with foveated encoding" : "");

    broadcastSocket_ = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    if (!oxrsys::runtime_socket::IsValid(broadcastSocket_))
    {
        spdlog::error("StreamingServer: Failed to create broadcast socket: {}",
                      oxrsys::runtime_socket::LastErrorText());
        return false;
    }

    oxrsys::runtime_socket::SetBroadcast(broadcastSocket_);
    oxrsys::runtime_socket::SetReuseAddress(broadcastSocket_);

    controlSocket_ = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    if (!oxrsys::runtime_socket::IsValid(controlSocket_))
    {
        spdlog::error("StreamingServer: Failed to create control socket: {}",
                      oxrsys::runtime_socket::LastErrorText());
        oxrsys::runtime_socket::Close(broadcastSocket_);
        return false;
    }

    oxrsys::runtime_socket::SetReuseAddress(controlSocket_);

    sockaddr_in controlAddr = {};
    controlAddr.sin_family = AF_INET;
    controlAddr.sin_port = htons(oxr::protocol::CONTROL_PORT);
    controlAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(controlSocket_, (sockaddr*)&controlAddr, sizeof(controlAddr)) < 0)
    {
        spdlog::error("StreamingServer: Failed to bind control socket on port {}",
                       oxr::protocol::CONTROL_PORT);
        oxrsys::runtime_socket::Close(broadcastSocket_);
        oxrsys::runtime_socket::Close(controlSocket_);
        return false;
    }

    videoSocket_ = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    if (!oxrsys::runtime_socket::IsValid(videoSocket_))
    {
        spdlog::error("StreamingServer: Failed to create video socket: {}",
                      oxrsys::runtime_socket::LastErrorText());
        oxrsys::runtime_socket::Close(broadcastSocket_);
        oxrsys::runtime_socket::Close(controlSocket_);
        return false;
    }

    oxrsys::runtime_socket::SetSendBuffer(videoSocket_, 4 * 1024 * 1024);
    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->acceptingPackets = true;
    }

    trackingReceiver_ = std::make_unique<TrackingReceiver>();
    if (!trackingReceiver_->Start())
    {
        spdlog::error("StreamingServer: Failed to start tracking receiver");
        oxrsys::runtime_socket::Close(broadcastSocket_);
        oxrsys::runtime_socket::Close(controlSocket_);
        oxrsys::runtime_socket::Close(videoSocket_);
        trackingReceiver_.reset();
        return false;
    }

    frameQueue_.Start();
    running_.store(true);
    state_.store(State::Broadcasting);
    RuntimeStatus::SetIdle();

    if (usbAdbEnabled_ && !StartUsbTcpListeners())
    {
        if (!wifiEnabled_)
        {
            spdlog::error("StreamingServer: Failed to start required USB ADB TCP listeners");
            running_.store(false);
            frameQueue_.Stop();
            oxrsys::runtime_socket::Close(broadcastSocket_);
            oxrsys::runtime_socket::Close(controlSocket_);
            oxrsys::runtime_socket::Close(videoSocket_);
            trackingReceiver_->Stop();
            trackingReceiver_.reset();
            return false;
        }
        spdlog::warn("StreamingServer: USB ADB TCP listeners unavailable; continuing with WiFi");
        usbAdbEnabled_ = false;
    }

    if (wifiEnabled_ && running_.load() && state_.load() == State::Broadcasting)
    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        if (!broadcastThread_.joinable() && state_.load() == State::Broadcasting)
        {
            broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
        }
    }
    controlThread_ = std::thread(&StreamingServer::ControlThread, this);
    videoSendThread_ = std::thread(&StreamingServer::VideoSendThread, this);
    encodeThread_ = std::thread(&StreamingServer::EncodeThread, this);

    std::string ip = GetLocalIpAddress();
    spdlog::info("StreamingServer: Started transport={} wifi={} usb_adb={} on {} ({}x{} @ {}Hz)",
                  config.streamingTransport, wifiEnabled_, usbAdbEnabled_,
                  ip, renderWidth_, renderHeight_, refreshRateHz_);
    return true;
}

void StreamingServer::Stop()
{
    std::lock_guard<std::mutex> stopLock(stopMutex_);

    InvalidateCallbackAccess();
    running_.store(false);
    state_.store(State::Stopped);
    frameQueue_.Stop();
    videoSendCv_.notify_all();
    RuntimeStatus::SetIdle();
    SendUsbDisconnectBestEffort();
    StopUsbTcpSockets();

    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->acceptingPackets = false;
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = oxrsys::runtime_socket::InvalidSocket;
        packetDispatchState_->videoUsesTcp = false;
    }
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_.clear();
        clientName_.clear();
        clientPort_ = 0;
        clientUsesUsbAdb_.store(false);
    }

    oxrsys::runtime_socket::Close(broadcastSocket_);
    oxrsys::runtime_socket::Close(controlSocket_);
    oxrsys::runtime_socket::Close(videoSocket_);

    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        JoinWorkerThread(broadcastThread_, "broadcast");
    }
    JoinWorkerThread(controlThread_, "control");
    JoinWorkerThread(encodeThread_, "encode");
    JoinWorkerThread(videoSendThread_, "video send");
    JoinWorkerThread(tcpControlThread_, "USB control");
    JoinWorkerThread(tcpVideoThread_, "USB video");
    JoinWorkerThread(tcpTrackingThread_, "USB tracking");
    JoinWorkerThread(tcpSpatialThread_, "USB spatial");
    frameQueue_.Clear();
    ClearVideoSendQueue();

    if (trackingReceiver_ != nullptr)
    {
        trackingReceiver_->Stop();
        trackingReceiver_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_.reset();
    }

    spdlog::info("StreamingServer: Stopped");
}

void StreamingServer::BroadcastThread()
{
    oxr::protocol::ServerAnnounce announce = BuildServerAnnounce(false);

    sockaddr_in broadcastAddr = {};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(oxr::protocol::DISCOVERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    while (running_.load() && state_.load() == State::Broadcasting)
    {
        oxrsys::runtime_socket::SendTo(broadcastSocket_,
                                       &announce,
                                       sizeof(announce),
                                       0,
                                       (sockaddr*)&broadcastAddr,
                                       sizeof(broadcastAddr));

        for (int i = 0; i < 10 && running_.load() && state_.load() == State::Broadcasting; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    spdlog::info("StreamingServer: Broadcast thread ended");
}

StreamingServer::StreamLayoutState StreamingServer::GetStreamLayoutState() const
{
    std::lock_guard<std::mutex> layoutLock(streamLayoutMutex_);
    return streamLayout_;
}

oxr::protocol::ServerAnnounce StreamingServer::BuildServerAnnounce(
    bool reliableControlTransport) const
{
    oxr::protocol::ServerAnnounce announce = {};
    const StreamLayoutState layoutState = GetStreamLayoutState();
    announce.type = oxr::protocol::MessageType::ServerAnnounce;
    announce.versionMajor = 1;
    announce.versionMinor = 2;
    announce.videoPort = oxr::protocol::VIDEO_PORT;
    announce.trackingPort = oxr::protocol::TRACKING_PORT;
    announce.renderWidth = renderWidth_ * 2;
    announce.renderHeight = renderHeight_;
    announce.refreshRateHz = refreshRateHz_;
    announce.encodedWidth = layoutState.encodedWidth;
    announce.encodedHeight = layoutState.encodedHeight;
    strncpy(announce.serverName, "OXRSys Runtime", sizeof(announce.serverName) - 1);

    const ConfigValues config = Config::Get().GetValues();
    const oxr::protocol::FoveationPreset foveationPreset =
        layoutState.foveatedEncodingActive
            ? ParseFoveationPreset(config.foveatedEncodingPreset)
            : oxr::protocol::FoveationPreset::Off;
    const oxr::protocol::FoveationLayout layout =
        oxr::protocol::CalculateFoveationLayout(layoutState.foveatedTargetEyeWidth,
                                                layoutState.foveatedTargetEyeHeight,
                                                foveationPreset);

    if (layoutState.foveatedEncodingActive)
    {
        announce.serverFeatures |= oxr::protocol::SERVER_FEATURE_FOVEATED_ENCODING;
    }
    oxr::protocol::ClientFoveationPreset clientFoveationPreset =
        ParseClientFoveationPreset(config.clientFoveationPreset);
    if (HasClientFoveationOverride(config.clientFoveationPreset))
    {
        announce.serverFeatures |= oxr::protocol::SERVER_FEATURE_CLIENT_FOVEATION;
    }
    if (config.clientUpscaling)
    {
        announce.serverFeatures |= oxr::protocol::SERVER_FEATURE_CLIENT_UPSCALING;
    }
    if (config.abrMode == "full" && reliableControlTransport)
    {
        announce.serverFeatures |= oxr::protocol::SERVER_FEATURE_STREAM_RECONFIGURE;
    }
    if (config.passthroughEnabled)
    {
        announce.serverFeatures |= oxr::protocol::SERVER_FEATURE_MIXED_REALITY_PASSTHROUGH;
    }
    // Occlusion is configured separately from support advertisement. Keep it
    // fail-closed until a valid app depth layer, headset depth, or scene mesh
    // source is connected to the compositor path.
    // Spatial flags remain fail-closed until a real backend is attached.
    // Headset audio is part of the wire protocol, but the stream is advertised
    // only once an actual capture/playback path is attached.

    announce.audioPort = oxr::protocol::AUDIO_PORT;
    announce.foveatedEncodingPreset = foveationPreset;
    announce.clientFoveationPreset = clientFoveationPreset;
    announce.clientUpscalingMode = config.clientUpscaling
        ? oxr::protocol::ClientUpscalingMode::SnapdragonGsr
        : oxr::protocol::ClientUpscalingMode::Off;
    announce.clientReprojectionMode = ParseClientReprojectionMode(config.clientReprojectionMode);
    announce.audioSampleRateHz = 48000;
    announce.foveationCenterSizeX = layout.parameters.centerSizeX;
    announce.foveationCenterSizeY = layout.parameters.centerSizeY;
    announce.foveationCenterShiftX = layout.parameters.centerShiftX;
    announce.foveationCenterShiftY = layout.parameters.centerShiftY;
    announce.foveationEdgeRatioX = layout.parameters.edgeRatioX;
    announce.foveationEdgeRatioY = layout.parameters.edgeRatioY;
    announce.spatialPort = oxr::protocol::SPATIAL_PORT;
    return announce;
}

void StreamingServer::ControlThread()
{
    uint8_t buffer[512];

    while (running_.load())
    {
        oxrsys::runtime_socket::SetReceiveTimeout(controlSocket_, 1, 0);

        sockaddr_in clientAddr = {};
        oxrsys::runtime_socket::SocketLength addrLen = sizeof(clientAddr);
        int received = oxrsys::runtime_socket::ReceiveFrom(controlSocket_,
                                                           buffer,
                                                           sizeof(buffer),
                                                           0,
                                                           (sockaddr*)&clientAddr,
                                                           &addrLen);
        if (received < 1)
        {
            continue;
        }

        uint8_t type = buffer[0];
        if (type == static_cast<uint8_t>(oxr::protocol::MessageType::ClientConnect) &&
            received >= static_cast<int>(oxr::protocol::CLIENT_CONNECT_BASE_SIZE))
        {
            if (wifiEnabled_)
            {
                oxr::protocol::ClientConnect clientConnect = {};
                memcpy(&clientConnect, buffer, std::min<size_t>(
                    static_cast<size_t>(received), sizeof(clientConnect)));
                HandleClientConnect(clientConnect, clientAddr);
            }
        }
        else if (type == static_cast<uint8_t>(oxr::protocol::MessageType::ServerDisconnect))
        {
            HandleClientDisconnect();
        }
        else
        {
            HandleControlPayload(buffer, static_cast<size_t>(received));
        }
    }

    spdlog::info("StreamingServer: Control thread ended");
}

bool StreamingServer::StartUsbTcpListeners()
{
    tcpControlListenSocket_ = CreateLoopbackListener(oxr::protocol::CONTROL_PORT);
    tcpVideoListenSocket_ = CreateLoopbackListener(oxr::protocol::VIDEO_PORT);
    tcpTrackingListenSocket_ = CreateLoopbackListener(oxr::protocol::TRACKING_PORT);
    tcpSpatialListenSocket_ = CreateLoopbackListener(oxr::protocol::SPATIAL_PORT);
    const bool spatialBackendAttached = false;
    if (!oxrsys::streaming_transport::UsbAdbTcpListenersReady(
            oxrsys::runtime_socket::IsValid(tcpControlListenSocket_),
            oxrsys::runtime_socket::IsValid(tcpVideoListenSocket_),
            oxrsys::runtime_socket::IsValid(tcpTrackingListenSocket_),
            oxrsys::runtime_socket::IsValid(tcpSpatialListenSocket_),
            spatialBackendAttached))
    {
        StopUsbTcpSockets();
        return false;
    }

    tcpControlThread_ = std::thread(&StreamingServer::TcpControlThread, this);
    tcpVideoThread_ = std::thread(&StreamingServer::TcpVideoThread, this);
    tcpTrackingThread_ = std::thread(&StreamingServer::TcpTrackingThread, this);
    if (oxrsys::runtime_socket::IsValid(tcpSpatialListenSocket_))
    {
        tcpSpatialThread_ = std::thread(&StreamingServer::TcpSpatialThread, this);
    }
    else
    {
        spdlog::warn("StreamingServer: optional USB ADB spatial listener on localhost port {} unavailable; continuing without spatial channel",
                     oxr::protocol::SPATIAL_PORT);
    }
    spdlog::info("StreamingServer: USB ADB TCP listeners active on localhost ports {}/{}/{}{}",
                  oxr::protocol::CONTROL_PORT,
                  oxr::protocol::VIDEO_PORT,
                  oxr::protocol::TRACKING_PORT,
                  oxrsys::runtime_socket::IsValid(tcpSpatialListenSocket_) ? "/9948" : "");
    return true;
}

void StreamingServer::SendUsbDisconnectBestEffort()
{
    SocketHandle controlSocket = oxrsys::runtime_socket::InvalidSocket;
    {
        std::lock_guard<std::mutex> lock(tcpSocketMutex_);
        controlSocket = tcpControlClientSocket_;
    }
    if (oxrsys::runtime_socket::IsValid(controlSocket))
    {
        SendTcpRecord(controlSocket, oxr::protocol::TcpRecordType::Disconnect, nullptr, 0);
    }
}

void StreamingServer::StopUsbTcpSockets()
{
    std::lock_guard<std::mutex> lock(tcpSocketMutex_);
    SocketHandle* sockets[] = {
        &tcpControlListenSocket_,
        &tcpVideoListenSocket_,
        &tcpTrackingListenSocket_,
        &tcpSpatialListenSocket_,
        &tcpControlClientSocket_,
        &tcpVideoClientSocket_,
        &tcpTrackingClientSocket_,
        &tcpSpatialClientSocket_,
    };
    for (SocketHandle* socketPtr : sockets)
    {
        CloseTcpSocket(*socketPtr);
    }
}

void StreamingServer::TcpControlThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        SocketHandle clientSocket = AcceptWithTimeout(tcpControlListenSocket_);
        if (!oxrsys::runtime_socket::IsValid(clientSocket))
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (oxrsys::runtime_socket::IsValid(tcpControlClientSocket_))
            {
                CloseTcpSocket(tcpControlClientSocket_);
            }
            tcpControlClientSocket_ = clientSocket;
        }

        oxr::protocol::ServerAnnounce announce = BuildServerAnnounce(true);
        if (!SendTcpRecord(clientSocket, oxr::protocol::TcpRecordType::ServerAnnounce,
                           &announce, sizeof(announce)))
        {
            {
                std::lock_guard<std::mutex> lock(tcpSocketMutex_);
                if (tcpControlClientSocket_ == clientSocket)
                {
                    tcpControlClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
                }
            }
            CloseTcpSocket(clientSocket);
            continue;
        }

        spdlog::info("StreamingServer: USB ADB control client connected");
        while (running_.load())
        {
            oxr::protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(clientSocket, header, payload))
            {
                break;
            }

            if (header.type == oxr::protocol::TcpRecordType::ClientConnect &&
                payload.size() >= oxr::protocol::CLIENT_CONNECT_BASE_SIZE)
            {
                oxr::protocol::ClientConnect clientConnect = {};
                memcpy(&clientConnect, payload.data(),
                       std::min(payload.size(), sizeof(clientConnect)));
                HandleUsbClientConnect(clientConnect);
            }
            else if (header.type == oxr::protocol::TcpRecordType::Control)
            {
                HandleControlPayload(payload.data(), payload.size());
            }
            else if (header.type == oxr::protocol::TcpRecordType::Disconnect)
            {
                HandleClientDisconnect();
                break;
            }
        }

        bool shouldClose = true;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpControlClientSocket_ == clientSocket)
            {
                tcpControlClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
            }
            else
            {
                shouldClose = false;
            }
        }
        if (shouldClose)
        {
            CloseTcpSocket(clientSocket);
        }
        if (clientUsesUsbAdb_.load())
        {
            HandleClientDisconnect();
        }
        spdlog::info("StreamingServer: USB ADB control client disconnected");
    }
    spdlog::info("StreamingServer: USB ADB control thread ended");
}

void StreamingServer::TcpVideoThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        SocketHandle clientSocket = AcceptWithTimeout(tcpVideoListenSocket_);
        if (!oxrsys::runtime_socket::IsValid(clientSocket))
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (oxrsys::runtime_socket::IsValid(tcpVideoClientSocket_))
            {
                CloseTcpSocket(tcpVideoClientSocket_);
            }
            tcpVideoClientSocket_ = clientSocket;
        }
        {
            std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
            if (clientUsesUsbAdb_.load())
            {
                packetDispatchState_->videoSocket = clientSocket;
                packetDispatchState_->videoUsesTcp = true;
                packetDispatchState_->acceptingPackets = true;
            }
        }

        spdlog::info("StreamingServer: USB ADB video client connected");
    }
    spdlog::info("StreamingServer: USB ADB video thread ended");
}

void StreamingServer::TcpTrackingThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        SocketHandle clientSocket = AcceptWithTimeout(tcpTrackingListenSocket_);
        if (!oxrsys::runtime_socket::IsValid(clientSocket))
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (oxrsys::runtime_socket::IsValid(tcpTrackingClientSocket_))
            {
                CloseTcpSocket(tcpTrackingClientSocket_);
            }
            tcpTrackingClientSocket_ = clientSocket;
        }

        spdlog::info("StreamingServer: USB ADB tracking client connected");
        while (running_.load())
        {
            oxr::protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(clientSocket, header, payload))
            {
                break;
            }
            if (header.type == oxr::protocol::TcpRecordType::Tracking &&
                payload.size() >= sizeof(oxr::protocol::TrackingPacket) &&
                trackingReceiver_ != nullptr)
            {
                trackingReceiver_->InjectPacket(payload.data(), payload.size());
            }
        }

        bool shouldClose = true;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpTrackingClientSocket_ == clientSocket)
            {
                tcpTrackingClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
            }
            else
            {
                shouldClose = false;
            }
        }
        if (shouldClose)
        {
            CloseTcpSocket(clientSocket);
        }
        spdlog::info("StreamingServer: USB ADB tracking client disconnected");
    }
    spdlog::info("StreamingServer: USB ADB tracking thread ended");
}

void StreamingServer::TcpSpatialThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        SocketHandle clientSocket = AcceptWithTimeout(tcpSpatialListenSocket_);
        if (!oxrsys::runtime_socket::IsValid(clientSocket))
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (oxrsys::runtime_socket::IsValid(tcpSpatialClientSocket_))
            {
                CloseTcpSocket(tcpSpatialClientSocket_);
            }
            tcpSpatialClientSocket_ = clientSocket;
        }

        spdlog::info("StreamingServer: USB ADB spatial client connected");
        while (running_.load())
        {
            oxr::protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(clientSocket, header, payload))
            {
                break;
            }
            if (header.type == oxr::protocol::TcpRecordType::Disconnect)
            {
                break;
            }
            if (header.type != oxr::protocol::TcpRecordType::Spatial)
            {
                continue;
            }
            // Spatial payloads are reserved for anchors/scene data. Keep this
            // channel alive independently from video/tracking until a backend is attached.
        }

        bool shouldClose = true;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpSpatialClientSocket_ == clientSocket)
            {
                tcpSpatialClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
            }
            else
            {
                shouldClose = false;
            }
        }
        if (shouldClose)
        {
            CloseTcpSocket(clientSocket);
        }
        spdlog::info("StreamingServer: USB ADB spatial client disconnected");
    }
    spdlog::info("StreamingServer: USB ADB spatial thread ended");
}

void StreamingServer::EncodeThread()
{
    auto telemetry = std::make_shared<EncodeTelemetry>();
    std::shared_ptr<PacketDispatchState> packetDispatchState = packetDispatchState_;

    while (running_.load())
    {
        StreamingFrame frame = {};
        if (!frameQueue_.WaitPop(running_, frame))
        {
            if (!running_.load())
            {
                break;
            }
            continue;
        }

        if (!frame.valid)
        {
            continue;
        }

        double queueWaitMs = (double)(SteadyClockNowNs() - frame.timestampNs) / 1.0e6;
        telemetry->queueWaitMs.Add(queueWaitMs);

        uint32_t currentRefreshHz = std::max(targetRefreshRateHz_.load(), 1u);
        const ConfigValues config = Config::Get().GetValues();
        uint32_t keyframeFrames = std::max(config.keyframeIntervalSec * currentRefreshHz, 1u);
        bool forceKeyframe = frame.frameIndex < 5 || (frame.frameIndex % keyframeFrames == 0);

        std::shared_ptr<VideoEncoder> encoder;
        std::shared_ptr<CallbackAccess> callbackAccess;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (encoder_ != nullptr)
            {
                encoder = encoder_;
            }
            callbackAccess = GetCallbackAccess();
        }

        if (!encoder || !encoder->IsInitialized())
        {
            ReleaseStreamingFrame(frame);
            continue;
        }
        if (callbackAccess == nullptr)
        {
            ReleaseStreamingFrame(frame);
            continue;
        }

        if (forceKeyframe)
        {
            encoder->ForceKeyframe();
        }

        auto encodedFrame = std::make_shared<EncodedVideoFrame>();
        encodedFrame->frameIndex = frame.frameIndex;
        encodedFrame->timestampNs = frame.timestampNs;
        encodedFrame->codec = activeVideoCodec_.load();
        encodedFrame->alphaBlend = frame.alphaBlend;
        encodedFrame->hasPose = frame.hasPose;
        if (frame.hasPose)
        {
            memcpy(encodedFrame->headPosition, frame.headPosition, sizeof(float) * 3);
            memcpy(encodedFrame->headOrientation, frame.headOrientation, sizeof(float) * 4);
        }

        const uint32_t submittedFrameIndex = frame.frameIndex;
        bool encoded = encoder->EncodeStereo(
            std::move(frame.source),
            frame.timestampNs,
            [encodedFrame](const uint8_t* nalData, size_t nalSize, bool isKeyframe, int64_t /*pts*/)
            {
                if (nalData == nullptr || nalSize == 0 ||
                    nalSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD - sizeof(oxr::protocol::TcpVideoNalHeader))
                {
                    return;
                }

                EncodedNalUnit nal = {};
                nal.isKeyframe = isKeyframe;
                nal.tcpPayload.resize(sizeof(oxr::protocol::TcpVideoNalHeader) + nalSize);

                oxr::protocol::TcpVideoNalHeader nalHeader = {};
                nalHeader.presentationTimeNs = encodedFrame->timestampNs;
                nalHeader.frameIndex = encodedFrame->frameIndex;
                nalHeader.payloadSize = static_cast<uint32_t>(nalSize);
                nalHeader.flags = oxr::protocol::VIDEO_FLAG_STEREO;
                if (encodedFrame->alphaBlend)
                {
                    nalHeader.flags |= oxr::protocol::VIDEO_FLAG_ALPHA_BLEND;
                }
                if (isKeyframe)
                {
                    nalHeader.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
                }
                nalHeader.codec = static_cast<uint8_t>(encodedFrame->codec);

                memcpy(nal.tcpPayload.data(), &nalHeader, sizeof(nalHeader));
                memcpy(nal.tcpPayload.data() + sizeof(nalHeader), nalData, nalSize);
                encodedFrame->nals.push_back(std::move(nal));
            },
            [callbackAccess, telemetry, queueWaitMs, encoder, config, encodedFrame](
                const VideoEncoder::FrameMetrics& metrics)
            {
                std::lock_guard<std::mutex> callbackLock(callbackAccess->mutex);
                StreamingServer* server = callbackAccess->accepting
                    ? callbackAccess->server
                    : nullptr;
                if (server == nullptr)
                {
                    return;
                }

                if (!metrics.frameDropped && !encodedFrame->nals.empty())
                {
                    server->QueueEncodedVideoFrame(std::move(*encodedFrame));
                }
                else if (metrics.frameDropped)
                {
                    server->encoderDroppedFramesTotalForAbr_.fetch_add(1);
                }

                telemetry->gpuCopyMs.Add(metrics.gpuCopyMs);
                telemetry->encodeSubmitMs.Add(metrics.encodeSubmitMs);
                telemetry->callbackLatencyMs.Add(metrics.callbackLatencyMs);
                telemetry->totalPipelineMs.Add(queueWaitMs + metrics.totalLatencyMs);

                if (!metrics.frameDropped)
                {
                    float smoothed = static_cast<float>(queueWaitMs + metrics.totalLatencyMs);
                    float previous = server->serverPipelineLatencyMs_.load();
                    server->serverPipelineLatencyMs_.store(previous * 0.85f + smoothed * 0.15f);
                    server->UpdatePredictionHorizon();
                }

                std::lock_guard<std::mutex> logLock(telemetry->logMutex);
                auto now = Clock::now();
                if (now - telemetry->lastLogTime >= std::chrono::seconds(1))
                {
                    MetricSummary queueSummary = telemetry->queueWaitMs.Consume();
                    MetricSummary gpuSummary = telemetry->gpuCopyMs.Consume();
                    MetricSummary submitSummary = telemetry->encodeSubmitMs.Consume();
                    MetricSummary callbackSummary = telemetry->callbackLatencyMs.Consume();
                    MetricSummary totalSummary = telemetry->totalPipelineMs.Consume();

                    const uint32_t replacedFrames = server->replacedFrameCount_.exchange(0);
                    const uint32_t encoderDrops = encoder->GetDroppedFrameCount();
                    const uint32_t keyframeRequests = server->requestKeyframeCount_.exchange(0);
                    const uint32_t pendingDepthMax = server->pendingFrameDepthMax_.exchange(0);
                    const uint32_t videoSendDepthMax = server->videoSendQueueDepthMax_.exchange(0);
                    const uint32_t videoSendDrops = server->videoSendDroppedFrames_.exchange(0);
                    const uint32_t videoTcpFailures = server->videoTcpSendFailures_.exchange(0);
                    const uint32_t udpRetransmits =
                        server->videoUdpRetransmittedPackets_.exchange(0);
                    std::string abrModeName;
                    std::string abrStateName;
                    std::string abrProfileName;
                    {
                        std::lock_guard<std::mutex> abrLock(server->abrStateMutex_);
                        abrModeName = server->abrModeName_;
                        abrStateName = server->abrStateName_;
                        abrProfileName = server->abrProfileName_;
                    }

                    const StreamLayoutState layoutState = server->GetStreamLayoutState();
                    const bool liveReconfigureReady =
                        oxrsys::streaming_reconfigure::AllowsLiveReconfigure(
                            server->clientUsesUsbAdb_.load(),
                            server->clientSupportsStreamReconfigure_.load());
                    RuntimeStatus::StreamingStats stats = {};
                    stats.refreshRateHz = server->targetRefreshRateHz_.load();
                    stats.currentBitrateMbps = server->currentBitrateMbps_.load();
                    stats.maxBitrateMbps = server->configMaxBitrateMbps_.load();
                    stats.renderWidth = server->renderWidth_ * 2;
                    stats.renderHeight = server->renderHeight_;
                    stats.encodedWidth = layoutState.encodedWidth;
                    stats.encodedHeight = layoutState.encodedHeight;
                    stats.videoCodec = VideoCodecName(server->activeVideoCodec_.load());
                    stats.encoderPreset = config.encoderPreset;
                    stats.foveatedEncodingPreset = server->clientFoveatedEncodingActive_.load()
                        ? config.foveatedEncodingPreset
                        : "off";
                    stats.clientFoveationPreset = config.clientFoveationPreset;
                    stats.clientUpscaling = config.clientUpscaling;
                    stats.clientReprojectionMode =
                        ClientReprojectionModeName(server->clientReprojectionMode_.load());
                    stats.abrMode = abrModeName;
                    stats.abrState = abrStateName;
                    stats.abrProfile = abrProfileName;
                    stats.resolutionScale = layoutState.activeResolutionScale;
                    stats.dynamicResolutionMinScale = config.dynamicResolutionMinScale;
                    stats.streamReconfigure = liveReconfigureReady;
                    stats.streamConfigSequence = layoutState.streamConfigSequence;
                    stats.passthroughEnabled = config.passthroughEnabled;
                    stats.passthroughSupported =
                        server->clientSupportsMixedRealityPassthrough_.load();
                    stats.passthroughReady =
                        stats.passthroughEnabled && stats.passthroughSupported;
                    stats.occlusionMode = config.occlusionMode;
                    stats.spatialEnabled = config.spatialEnabled;
                    stats.headsetAudio = false;
                    stats.serverPipelineLatencyMs = server->serverPipelineLatencyMs_.load();
                    stats.clientPipelineLatencyMs = server->clientPipelineLatencyMs_.load();
                    stats.clientReceiveToSubmitMs = server->clientReceiveToSubmitMs_.load();
                    stats.clientDecodeMs = server->clientDecodeLatencyMs_.load();
                    stats.clientCompositorMs = server->clientCompositorLatencyMs_.load();
                    stats.predictionHorizonMs = server->trackingReceiver_
                        ? server->trackingReceiver_->GetPredictionHorizonMs()
                        : 0.0f;
                    stats.displayedFrameAgeMs = server->clientDisplayedFrameAgeMs_.load();
                    stats.encodeQueueAverageMs = queueSummary.average;
                    stats.encodeQueueP95Ms = queueSummary.p95;
                    stats.encodeGpuAverageMs = gpuSummary.average;
                    stats.encodeGpuP95Ms = gpuSummary.p95;
                    stats.encodeSubmitAverageMs = submitSummary.average;
                    stats.encodeSubmitP95Ms = submitSummary.p95;
                    stats.encodeCallbackAverageMs = callbackSummary.average;
                    stats.encodeCallbackP95Ms = callbackSummary.p95;
                    stats.encodeTotalAverageMs = totalSummary.average;
                    stats.encodeTotalP95Ms = totalSummary.p95;
                    stats.encodedFramesTotal = encoder->GetEncodedFrameCount();
                    stats.encoderDroppedFramesTotal = encoderDrops;
                    stats.replacedFramesDelta = replacedFrames;
                    stats.keyframeRequestsDelta = keyframeRequests;
                    stats.pendingDepthMax = pendingDepthMax;
                    stats.videoSendQueueDepthMax = videoSendDepthMax;
                    stats.videoSendDroppedFramesDelta = videoSendDrops;
                    stats.videoTcpSendFailuresDelta = videoTcpFailures;
                    stats.videoUdpRetransmittedPacketsDelta = udpRetransmits;
                    stats.reprojectedFramesDelta = server->clientReprojectedFramesDelta_.load();
                    stats.staleFrameReusesDelta = server->clientStaleFrameReusesDelta_.load();
                    stats.renderPoseFallbacksDelta =
                        server->clientRenderPoseFallbacksDelta_.load();
                    RuntimeStatus::SetStreamingStats(stats);

                    spdlog::info(
                        "StreamingServer: encode queue(avg/p95={:.2f}/{:.2f}ms) gpu({:.2f}/{:.2f}) "
                        "submit({:.3f}/{:.3f}) callback({:.2f}/{:.2f}) total({:.2f}/{:.2f}) "
                        "replaced={} encDrops={} keyframeReq={} depthMax={} sendDepth={} sendDrops={} "
                        "tcpFail={} udpRetrans={} abr={}/{} profile={} displayedAge={:.2f}ms reproj={} stale={} poseFallbacks={}",
                        queueSummary.average, queueSummary.p95,
                        gpuSummary.average, gpuSummary.p95,
                        submitSummary.average, submitSummary.p95,
                        callbackSummary.average, callbackSummary.p95,
                        totalSummary.average, totalSummary.p95,
                        replacedFrames,
                        encoderDrops,
                        keyframeRequests,
                        pendingDepthMax,
                        videoSendDepthMax,
                        videoSendDrops,
                        videoTcpFailures,
                        udpRetransmits,
                        abrModeName,
                        abrStateName,
                        abrProfileName,
                        server->clientDisplayedFrameAgeMs_.load(),
                        server->clientReprojectedFramesDelta_.load(),
                        server->clientStaleFrameReusesDelta_.load(),
                        server->clientRenderPoseFallbacksDelta_.load());

                    telemetry->lastLogTime = now;
                }
            });

        ReleaseStreamingFrame(frame);

        if (!encoded)
        {
            pendingFrameDepthMax_.store(std::max(
                pendingFrameDepthMax_.load(), encoder->GetInFlightFrameCount()));
        }

        static uint32_t logFrameCounter = 0;
        if (++logFrameCounter <= 5 || logFrameCounter % 300 == 0)
        {
            spdlog::info("StreamingServer: queued frame #{} encodeSubmitted={} queueWait={:.2f}ms inflight={}",
                          submittedFrameIndex, encoded, queueWaitMs, encoder->GetInFlightFrameCount());
        }
    }

    spdlog::info("StreamingServer: Encode thread ended");
}

void StreamingServer::HandleClientConnect(const oxr::protocol::ClientConnect& clientConnect,
                                          const sockaddr_in& clientAddr)
{
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
    std::string clientName(clientConnect.deviceName,
        BoundedStringLength(clientConnect.deviceName, sizeof(clientConnect.deviceName)));

    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_ = ipStr;
        clientPort_ = ntohs(clientAddr.sin_port);
        clientName_ = clientName;
        clientUsesUsbAdb_.store(false);
    }
    clientSupportsStreamReconfigure_.store(
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_STREAM_RECONFIGURE));
    const bool clientSupportsPassthrough =
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH);
    clientSupportsMixedRealityPassthrough_.store(clientSupportsPassthrough);
    clientSupportsSpatialEntity_.store(
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_SPATIAL_ENTITY));
    if (Config::Get().GetValues().passthroughEnabled && !clientSupportsPassthrough)
    {
        spdlog::warn("StreamingServer: client '{}' did not advertise passthrough support; app alpha/source-alpha frames will be streamed without headset passthrough",
                     clientName);
    }
    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp = ipStr;
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->videoUsesTcp = false;
        packetDispatchState_->acceptingPackets = true;
    }
    uint32_t negotiatedRefresh = clientConnect.refreshRateHz > 0
        ? clientConnect.refreshRateHz
        : refreshRateHz_;
    targetRefreshRateHz_.store(negotiatedRefresh);
    UpdatePredictionHorizon();

    state_.store(State::Connected);

    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        JoinWorkerThread(broadcastThread_, "broadcast");
    }
    if (!running_.load())
    {
        return;
    }

    bool encoderReady = false;
    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_ = std::make_shared<VideoEncoder>();
        if (IsGraphicsContextValid(graphicsContext_))
        {
            RenewCallbackAccess();
            const ConfigValues config = Config::Get().GetValues();
            const oxr::protocol::VideoCodec selectedCodec = SelectVideoCodec(config, clientConnect);
            if (config.videoCodec != "auto" &&
                selectedCodec != ParseConfiguredVideoCodec(config.videoCodec))
            {
                spdlog::warn("StreamingServer: client '{}' does not support configured video_codec='{}'; using {}",
                             clientName,
                             config.videoCodec,
                             VideoCodecName(selectedCodec));
            }
            activeVideoCodec_.store(selectedCodec);
            uint32_t bitrateMbps =
                (clientConnect.maxBitrateMbps != oxr::protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG)
                ? std::min(config.bitrateMbps, clientConnect.maxBitrateMbps)
                : config.bitrateMbps;
            configMaxBitrateMbps_.store(bitrateMbps);
            currentBitrateMbps_.store(bitrateMbps);
            lastKeyframeRequestCountForAbr_ = 0;
            lastVideoSendDroppedFrameCountForAbr_ = 0;
            lastEncoderDroppedFrameCountForAbr_ = 0;
            requestKeyframeTotalForAbr_.store(0);
            videoSendDroppedFramesTotalForAbr_.store(0);
            encoderDroppedFramesTotalForAbr_.store(0);
            abrController_.Reset(oxrsys::streaming_abr::ParseMode(config.abrMode),
                                 bitrateMbps,
                                 bitrateMbps,
                                 config.resolutionScale,
                                 config.dynamicResolutionMinScale);
            {
                std::lock_guard<std::mutex> abrLock(abrStateMutex_);
                abrModeName_ = oxrsys::streaming_abr::ToString(
                    oxrsys::streaming_abr::ParseMode(config.abrMode));
                abrStateName_ = "stable";
                abrProfileName_ = config.abrMode == "full" ? "balanced" : "bitrate";
            }
            const StreamLayoutState layoutState = GetStreamLayoutState();
            const oxr::protocol::FoveationPreset foveationPreset =
                layoutState.foveatedEncodingActive
                    ? ParseFoveationPreset(config.foveatedEncodingPreset)
                    : oxr::protocol::FoveationPreset::Off;
            const oxr::protocol::FoveationLayout layout =
                oxr::protocol::CalculateFoveationLayout(layoutState.foveatedTargetEyeWidth,
                                                        layoutState.foveatedTargetEyeHeight,
                                                        foveationPreset);
            const bool clientSupportsFoveatedEncoding =
                HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_FOVEATED_ENCODING);
            clientSupportsFoveatedEncoding_.store(clientSupportsFoveatedEncoding);
            const bool useFoveatedEncoding =
                layoutState.foveatedEncodingActive && clientSupportsFoveatedEncoding;
            clientFoveatedEncodingActive_.store(useFoveatedEncoding);
            encoder_->SetFoveationSettings(BuildEncoderFoveationSettings(
                useFoveatedEncoding, layout));
            if (layoutState.foveatedEncodingActive && !clientSupportsFoveatedEncoding)
            {
                spdlog::warn("StreamingServer: client '{}' did not advertise foveated encoding support; sending reduced normal video",
                             clientName);
            }
            if (encoder_->Initialize(layoutState.encodedWidth, layoutState.encodedHeight, negotiatedRefresh,
                                     bitrateMbps, graphicsContext_, selectedCodec))
            {
                encoder_->ForceKeyframe();
                frameIndex_ = 0;
                encoderReady = true;
                spdlog::info("StreamingServer: Client connected via WiFi: {} ({}:{}) refresh={}Hz codec={}",
                              clientName, clientIp_, clientPort_, negotiatedRefresh, VideoCodecName(selectedCodec));
                RuntimeStatus::SetStreaming("wifi", clientName);
            }
            else
            {
                spdlog::error("StreamingServer: Failed to initialize VideoEncoder");
                encoder_.reset();
            }
        }
        else
        {
            spdlog::warn("StreamingServer: No graphics device set, cannot encode video");
            encoder_.reset();
        }
    }

    if (!encoderReady)
    {
        InvalidateCallbackAccess();
        RuntimeStatus::SetIdle();
        state_.store(State::Broadcasting);
        if (wifiEnabled_ && running_.load())
        {
            std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
            if (running_.load() && state_.load() == State::Broadcasting &&
                !broadcastThread_.joinable())
            {
                broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
            }
        }
    }

}

void StreamingServer::HandleUsbClientConnect(const oxr::protocol::ClientConnect& clientConnect)
{
    std::string clientName(clientConnect.deviceName,
        BoundedStringLength(clientConnect.deviceName, sizeof(clientConnect.deviceName)));
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_ = "127.0.0.1";
        clientPort_ = oxr::protocol::CONTROL_PORT;
        clientName_ = clientName;
        clientUsesUsbAdb_.store(true);
    }
    clientSupportsStreamReconfigure_.store(
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_STREAM_RECONFIGURE));
    const bool clientSupportsPassthrough =
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH);
    clientSupportsMixedRealityPassthrough_.store(clientSupportsPassthrough);
    clientSupportsSpatialEntity_.store(
        HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_SPATIAL_ENTITY));
    if (Config::Get().GetValues().passthroughEnabled && !clientSupportsPassthrough)
    {
        spdlog::warn("StreamingServer: USB client '{}' did not advertise passthrough support; app alpha/source-alpha frames will be streamed without headset passthrough",
                     clientName);
    }
    {
        std::lock_guard<std::mutex> tcpLock(tcpSocketMutex_);
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = tcpVideoClientSocket_;
        packetDispatchState_->videoUsesTcp = true;
        packetDispatchState_->acceptingPackets = true;
    }
    uint32_t negotiatedRefresh = clientConnect.refreshRateHz > 0
        ? clientConnect.refreshRateHz
        : refreshRateHz_;
    targetRefreshRateHz_.store(negotiatedRefresh);
    UpdatePredictionHorizon();

    state_.store(State::Connected);

    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        JoinWorkerThread(broadcastThread_, "broadcast");
    }
    if (!running_.load())
    {
        return;
    }

    bool encoderReady = false;
    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_ = std::make_shared<VideoEncoder>();
        if (IsGraphicsContextValid(graphicsContext_))
        {
            RenewCallbackAccess();
            const ConfigValues config = Config::Get().GetValues();
            const oxr::protocol::VideoCodec selectedCodec = SelectVideoCodec(config, clientConnect);
            if (config.videoCodec != "auto" &&
                selectedCodec != ParseConfiguredVideoCodec(config.videoCodec))
            {
                spdlog::warn("StreamingServer: USB client '{}' does not support configured video_codec='{}'; using {}",
                             clientName,
                             config.videoCodec,
                             VideoCodecName(selectedCodec));
            }
            activeVideoCodec_.store(selectedCodec);
            uint32_t bitrateMbps =
                (clientConnect.maxBitrateMbps != oxr::protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG)
                ? std::min(config.bitrateMbps, clientConnect.maxBitrateMbps)
                : config.bitrateMbps;
            configMaxBitrateMbps_.store(bitrateMbps);
            currentBitrateMbps_.store(bitrateMbps);
            lastKeyframeRequestCountForAbr_ = 0;
            lastVideoSendDroppedFrameCountForAbr_ = 0;
            lastEncoderDroppedFrameCountForAbr_ = 0;
            requestKeyframeTotalForAbr_.store(0);
            videoSendDroppedFramesTotalForAbr_.store(0);
            encoderDroppedFramesTotalForAbr_.store(0);
            abrController_.Reset(oxrsys::streaming_abr::ParseMode(config.abrMode),
                                 bitrateMbps,
                                 bitrateMbps,
                                 config.resolutionScale,
                                 config.dynamicResolutionMinScale);
            {
                std::lock_guard<std::mutex> abrLock(abrStateMutex_);
                abrModeName_ = oxrsys::streaming_abr::ToString(
                    oxrsys::streaming_abr::ParseMode(config.abrMode));
                abrStateName_ = "stable";
                abrProfileName_ = config.abrMode == "full" ? "balanced" : "bitrate";
            }
            const StreamLayoutState layoutState = GetStreamLayoutState();
            const oxr::protocol::FoveationPreset foveationPreset =
                layoutState.foveatedEncodingActive
                    ? ParseFoveationPreset(config.foveatedEncodingPreset)
                    : oxr::protocol::FoveationPreset::Off;
            const oxr::protocol::FoveationLayout layout =
                oxr::protocol::CalculateFoveationLayout(layoutState.foveatedTargetEyeWidth,
                                                        layoutState.foveatedTargetEyeHeight,
                                                        foveationPreset);
            const bool clientSupportsFoveatedEncoding =
                HasClientCapability(clientConnect, oxr::protocol::CLIENT_CAPABILITY_FOVEATED_ENCODING);
            clientSupportsFoveatedEncoding_.store(clientSupportsFoveatedEncoding);
            const bool useFoveatedEncoding =
                layoutState.foveatedEncodingActive && clientSupportsFoveatedEncoding;
            clientFoveatedEncodingActive_.store(useFoveatedEncoding);
            encoder_->SetFoveationSettings(BuildEncoderFoveationSettings(
                useFoveatedEncoding, layout));
            if (layoutState.foveatedEncodingActive && !clientSupportsFoveatedEncoding)
            {
                spdlog::warn("StreamingServer: USB client '{}' did not advertise foveated encoding support; sending reduced normal video",
                             clientName);
            }
            if (encoder_->Initialize(layoutState.encodedWidth, layoutState.encodedHeight, negotiatedRefresh,
                                     bitrateMbps, graphicsContext_, selectedCodec))
            {
                encoder_->ForceKeyframe();
                frameIndex_ = 0;
                encoderReady = true;
                spdlog::info("StreamingServer: Client connected via usb_adb: {} refresh={}Hz codec={}",
                              clientName, negotiatedRefresh, VideoCodecName(selectedCodec));
                RuntimeStatus::SetStreaming("usb_adb", clientName);
            }
            else
            {
                spdlog::error("StreamingServer: Failed to initialize VideoEncoder for USB ADB");
                encoder_.reset();
            }
        }
        else
        {
            spdlog::warn("StreamingServer: No graphics device set, cannot encode video");
            encoder_.reset();
        }
    }

    if (!encoderReady)
    {
        InvalidateCallbackAccess();
        RuntimeStatus::SetIdle();
        clientUsesUsbAdb_.store(false);
        state_.store(State::Broadcasting);
        if (wifiEnabled_ && running_.load())
        {
            std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
            if (running_.load() && state_.load() == State::Broadcasting &&
                !broadcastThread_.joinable())
            {
                broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
            }
        }
    }
}

void StreamingServer::HandleClientDisconnect()
{
    std::lock_guard<std::mutex> disconnectLock(disconnectMutex_);
    if (!running_.load())
    {
        return;
    }

    State previousState = state_.exchange(State::Broadcasting);
    bool hadClient = false;
    bool wasUsbClient = false;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        const bool usbClient = clientUsesUsbAdb_.load();
        hadClient = usbClient || !clientIp_.empty() ||
                    !clientName_.empty() || clientPort_ != 0;
        wasUsbClient = usbClient;
        clientIp_.clear();
        clientName_.clear();
        clientPort_ = 0;
        clientUsesUsbAdb_.store(false);
    }
    if (previousState != State::Connected && !hadClient)
    {
        return;
    }
    InvalidateCallbackAccess();

    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->videoUsesTcp = false;
    }
    if (wasUsbClient)
    {
        std::lock_guard<std::mutex> lock(tcpSocketMutex_);
        CloseTcpSocket(tcpControlClientSocket_);
        CloseTcpSocket(tcpVideoClientSocket_);
        CloseTcpSocket(tcpTrackingClientSocket_);
        CloseTcpSocket(tcpSpatialClientSocket_);
    }

    {
        frameQueue_.Clear();
        ClearVideoSendQueue();
    }

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_.reset();
    }

    targetRefreshRateHz_.store(refreshRateHz_);
    clientFoveatedEncodingActive_.store(false);
    clientSupportsFoveatedEncoding_.store(false);
    clientSupportsStreamReconfigure_.store(false);
    clientSupportsMixedRealityPassthrough_.store(false);
    clientSupportsSpatialEntity_.store(false);
    activeVideoCodec_.store(oxr::protocol::VideoCodec::H265);
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        ResetPendingStreamConfigLocked();
    }
    clientDisplayedFrameAgeMs_.store(0.0f);
    clientReprojectedFramesDelta_.store(0);
    clientStaleFrameReusesDelta_.store(0);
    clientRenderPoseFallbacksDelta_.store(0);
    clientReprojectionMode_.store(oxr::protocol::ClientReprojectionMode::Off);
    requestKeyframeTotalForAbr_.store(0);
    videoSendDroppedFramesTotalForAbr_.store(0);
    encoderDroppedFramesTotalForAbr_.store(0);
    lastKeyframeRequestCountForAbr_ = 0;
    lastVideoSendDroppedFrameCountForAbr_ = 0;
    lastEncoderDroppedFrameCountForAbr_ = 0;
    UpdatePredictionHorizon();

    if (previousState == State::Connected)
    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        JoinWorkerThread(broadcastThread_, "broadcast");
    }

    if (previousState == State::Connected && wifiEnabled_ && running_.load())
    {
        std::lock_guard<std::mutex> broadcastLock(broadcastThreadMutex_);
        if (running_.load() && state_.load() == State::Broadcasting &&
            !broadcastThread_.joinable())
        {
            broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
        }
    }

    RuntimeStatus::SetIdle();
    spdlog::info("StreamingServer: Client disconnected, resuming broadcast");
}

void StreamingServer::HandleLatencyReport(const oxr::protocol::LatencyReport& report)
{
    clientReceiveToSubmitMs_.store(report.receiveToDecoderSubmitMs);
    clientDecodeLatencyMs_.store(report.decodeLatencyMs);
    clientCompositorLatencyMs_.store(report.compositorLatencyMs);
    clientDisplayedFrameAgeMs_.store(report.displayedFrameAgeMs);
    clientReprojectedFramesDelta_.store(report.reprojectedFrames);
    clientStaleFrameReusesDelta_.store(report.staleFrameReuses);
    clientRenderPoseFallbacksDelta_.store(report.renderPoseFallbacks);
    clientReprojectionMode_.store(report.reprojectionMode);

    float previous = clientPipelineLatencyMs_.load();
    float smoothed = previous * 0.8f + report.totalClientLatencyMs * 0.2f;
    clientPipelineLatencyMs_.store(smoothed);
    UpdatePredictionHorizon();

    // Adaptive bitrate: use sliding-window client health signals with fast downshift
    // and slow recovery to avoid oscillation.
    {
        uint32_t currentBitrate = currentBitrateMbps_.load();
        uint32_t keyframeRequests = requestKeyframeTotalForAbr_.load();
        uint32_t newKeyframeRequests = keyframeRequests - lastKeyframeRequestCountForAbr_;
        lastKeyframeRequestCountForAbr_ = keyframeRequests;
        uint32_t videoSendDroppedFrames = videoSendDroppedFramesTotalForAbr_.load();
        uint32_t newVideoSendDrops =
            videoSendDroppedFrames - lastVideoSendDroppedFrameCountForAbr_;
        lastVideoSendDroppedFrameCountForAbr_ = videoSendDroppedFrames;
        uint32_t encoderDroppedFrames = encoderDroppedFramesTotalForAbr_.load();
        uint32_t newEncoderDrops =
            encoderDroppedFrames - lastEncoderDroppedFrameCountForAbr_;
        lastEncoderDroppedFrameCountForAbr_ = encoderDroppedFrames;

        oxrsys::streaming_abr::Sample sample = {};
        sample.totalClientLatencyMs = smoothed;
        sample.displayedFrameAgeMs = report.displayedFrameAgeMs;
        sample.keyframeRequestsDelta = newKeyframeRequests;
        sample.videoSendDroppedFramesDelta = newVideoSendDrops;
        sample.encoderDroppedFramesDelta = newEncoderDrops;
        sample.reprojectedFramesDelta = report.reprojectedFrames;
        sample.staleFrameReusesDelta = report.staleFrameReuses;
        sample.renderPoseFallbacksDelta = report.renderPoseFallbacks;

        oxrsys::streaming_abr::Decision decision = abrController_.Update(sample);
        {
            std::lock_guard<std::mutex> abrLock(abrStateMutex_);
            abrStateName_ = oxrsys::streaming_abr::ToString(decision.state);
            abrProfileName_ = decision.profile;
        }

        if (decision.bitrateChanged && decision.targetBitrateMbps != currentBitrate)
        {
            currentBitrateMbps_.store(decision.targetBitrateMbps);
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (encoder_ != nullptr)
            {
                encoder_->SetBitrate(decision.targetBitrateMbps);
            }
        }
        MaybeRequestStreamReconfigure(decision.targetResolutionScale);
    }

    static auto lastLogTime = Clock::now();
    auto now = Clock::now();
    if (now - lastLogTime >= std::chrono::seconds(1))
    {
        spdlog::info("StreamingServer: client latency receive->submit={:.2f}ms decode={:.2f}ms compositor={:.2f}ms total={:.2f}ms displayedAge={:.2f}ms reproj={} stale={} poseFallbacks={} horizon={:.2f}ms bitrate={}Mbps",
                      report.receiveToDecoderSubmitMs,
                      report.decodeLatencyMs,
                      report.compositorLatencyMs,
                      report.totalClientLatencyMs,
                      report.displayedFrameAgeMs,
                      report.reprojectedFrames,
                      report.staleFrameReuses,
                      report.renderPoseFallbacks,
                      trackingReceiver_ ? trackingReceiver_->GetPredictionHorizonMs() : 0.0f,
                      currentBitrateMbps_.load());
        lastLogTime = now;
    }
}

void StreamingServer::HandleKeyframeRequest(const oxr::protocol::RequestKeyframe& request)
{
    requestKeyframeCount_.fetch_add(1);
    requestKeyframeTotalForAbr_.fetch_add(1);

    std::lock_guard<std::mutex> lock(encoderMutex_);
    if (encoder_ != nullptr)
    {
        encoder_->ForceKeyframe();
    }

    spdlog::info("StreamingServer: Keyframe requested (reasons=0x{:x}, detail={})",
                  request.reasonFlags, request.detail);
}

bool StreamingServer::SendControlPayload(const void* payload, size_t payloadSize)
{
    if (payload == nullptr || payloadSize == 0)
    {
        return false;
    }

    if (clientUsesUsbAdb_.load())
    {
        SocketHandle controlSocket = oxrsys::runtime_socket::InvalidSocket;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            controlSocket = tcpControlClientSocket_;
        }
        return oxrsys::runtime_socket::IsValid(controlSocket) &&
               SendTcpRecord(controlSocket, oxr::protocol::TcpRecordType::Control,
                             payload, payloadSize);
    }

    std::string clientIp;
    uint16_t clientPort = 0;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp = clientIp_;
        clientPort = clientPort_;
    }
    if (clientIp.empty() || clientPort == 0 ||
        !oxrsys::runtime_socket::IsValid(controlSocket_))
    {
        return false;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(clientPort);
    inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);
    const int sent = oxrsys::runtime_socket::SendTo(
        controlSocket_, payload, payloadSize, kBestEffortSendFlags,
        reinterpret_cast<sockaddr*>(&destAddr), sizeof(destAddr));
    return sent == static_cast<int>(payloadSize);
}

void StreamingServer::ResetPendingStreamConfigLocked()
{
    pendingStreamConfigUpdate_ = {};
    pendingStreamConfigSequence_.store(0);
    pendingResolutionScale_.store(0.0f);
    streamConfigPending_.Reset();
}

void StreamingServer::TickPendingStreamConfigTimeout(int64_t nowNs)
{
    oxr::protocol::StreamConfigUpdate retryUpdate = {};
    bool retryPendingUpdate = false;
    bool disconnectAfterTimeout = false;
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        const oxrsys::streaming_reconfigure::TickAction action =
            streamConfigPending_.Tick(nowNs, kStreamConfigAckTimeoutNs, kStreamConfigMaxRetries);
        if (action == oxrsys::streaming_reconfigure::TickAction::Retry)
        {
            retryUpdate = pendingStreamConfigUpdate_;
            retryPendingUpdate = true;
        }
        else if (action == oxrsys::streaming_reconfigure::TickAction::Timeout)
        {
            spdlog::warn("StreamingServer: stream config seq={} timed out after {} retries; disconnecting client to recover decoder/encoder sync",
                         pendingStreamConfigUpdate_.sequence,
                         kStreamConfigMaxRetries);
            ResetPendingStreamConfigLocked();
            disconnectAfterTimeout = true;
        }
        if (pendingStreamConfigSequence_.load() != 0 && !retryPendingUpdate)
        {
            return;
        }
    }

    if (disconnectAfterTimeout)
    {
        HandleClientDisconnect();
        return;
    }

    if (retryPendingUpdate)
    {
        if (!SendControlPayload(&retryUpdate, sizeof(retryUpdate)))
        {
            {
                std::lock_guard<std::mutex> lock(streamConfigMutex_);
                ResetPendingStreamConfigLocked();
            }
            spdlog::warn("StreamingServer: failed to retry stream config update seq={}",
                         retryUpdate.sequence);
            HandleClientDisconnect();
        }
        else
        {
            spdlog::info("StreamingServer: retried stream config update seq={} encoded={}x{}",
                         retryUpdate.sequence,
                         retryUpdate.encodedWidth,
                         retryUpdate.encodedHeight);
        }
    }
}

void StreamingServer::MaybeRequestStreamReconfigure(float targetResolutionScale)
{
    const ConfigValues config = Config::Get().GetValues();
    const bool reliableControlTransport = clientUsesUsbAdb_.load();
    if (config.abrMode != "full" ||
        !oxrsys::streaming_reconfigure::AllowsLiveReconfigure(
            reliableControlTransport, clientSupportsStreamReconfigure_.load()) ||
        state_.load() != State::Connected)
    {
        return;
    }

    const int64_t nowNs = SteadyClockNowNs();
    TickPendingStreamConfigTimeout(nowNs);
    if (pendingStreamConfigSequence_.load() != 0)
    {
        return;
    }

    const StreamLayoutState currentLayout = GetStreamLayoutState();
    const float minScale = std::clamp(
        config.dynamicResolutionMinScale, 0.25f, config.resolutionScale);
    const float clampedScale = std::clamp(targetResolutionScale, minScale, config.resolutionScale);
    if (std::fabs(clampedScale - currentLayout.activeResolutionScale) < 0.025f)
    {
        return;
    }

    const StreamLayout layout = BuildStreamLayout(
        renderWidth_, renderHeight_, clampedScale, config, graphicsContext_);
    if (layout.encodedWidth == currentLayout.encodedWidth &&
        layout.encodedHeight == currentLayout.encodedHeight)
    {
        std::lock_guard<std::mutex> layoutLock(streamLayoutMutex_);
        streamLayout_.activeResolutionScale = layout.resolutionScale;
        return;
    }

    oxr::protocol::StreamConfigUpdate update = {};
    update.sequence = currentLayout.streamConfigSequence + 1;
    update.renderWidth = renderWidth_ * 2;
    update.renderHeight = renderHeight_;
    update.encodedWidth = layout.encodedWidth;
    update.encodedHeight = layout.encodedHeight;
    update.targetBitrateMbps = currentBitrateMbps_.load();
    update.refreshRateHz = targetRefreshRateHz_.load();
    update.flags = oxr::protocol::STREAM_CONFIG_FLAG_RECONFIGURE_DECODER |
                   oxr::protocol::STREAM_CONFIG_FLAG_FORCE_KEYFRAME;
    if (layout.foveatedEncodingActive && clientSupportsFoveatedEncoding_.load())
    {
        update.flags |= oxr::protocol::STREAM_CONFIG_FLAG_FOVEATED_ENCODING;
        update.foveatedEncodingPreset = layout.foveationPreset;
    }
    if (config.clientUpscaling)
    {
        update.flags |= oxr::protocol::STREAM_CONFIG_FLAG_CLIENT_UPSCALING;
        update.clientUpscalingMode = oxr::protocol::ClientUpscalingMode::SnapdragonGsr;
    }
    update.foveationCenterSizeX = layout.foveationLayout.parameters.centerSizeX;
    update.foveationCenterSizeY = layout.foveationLayout.parameters.centerSizeY;
    update.foveationCenterShiftX = layout.foveationLayout.parameters.centerShiftX;
    update.foveationCenterShiftY = layout.foveationLayout.parameters.centerShiftY;
    update.foveationEdgeRatioX = layout.foveationLayout.parameters.edgeRatioX;
    update.foveationEdgeRatioY = layout.foveationLayout.parameters.edgeRatioY;

    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        pendingStreamConfigUpdate_ = update;
        pendingResolutionScale_.store(layout.resolutionScale);
        pendingStreamConfigSequence_.store(update.sequence);
        streamConfigPending_.Begin(update.sequence, nowNs);
    }

    if (!SendControlPayload(&update, sizeof(update)))
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        ResetPendingStreamConfigLocked();
        spdlog::warn("StreamingServer: failed to send stream config update seq={} scale={:.2f}",
                     update.sequence, layout.resolutionScale);
        return;
    }

    spdlog::info("StreamingServer: requested stream config update seq={} scale={:.2f} encoded={}x{}",
                 update.sequence, layout.resolutionScale,
                 update.encodedWidth, update.encodedHeight);
}

void StreamingServer::ApplyPendingStreamConfigLocked(
    const oxr::protocol::StreamConfigUpdate& update)
{
    const ConfigValues config = Config::Get().GetValues();
    const float targetScale = pendingResolutionScale_.load();
    const StreamLayout layout = BuildStreamLayout(
        renderWidth_, renderHeight_, targetScale, config, graphicsContext_);
    if (layout.encodedWidth != update.encodedWidth ||
        layout.encodedHeight != update.encodedHeight)
    {
        {
            std::lock_guard<std::mutex> lock(streamConfigMutex_);
            ResetPendingStreamConfigLocked();
        }
        spdlog::warn("StreamingServer: stream config seq={} no longer matches current layout after config reload; reconnecting client",
                     update.sequence);
        HandleClientDisconnect();
        return;
    }

    auto newEncoder = std::make_shared<VideoEncoder>();
    const bool useFoveatedEncoding =
        layout.foveatedEncodingActive && clientSupportsFoveatedEncoding_.load();
    newEncoder->SetFoveationSettings(BuildEncoderFoveationSettings(
        useFoveatedEncoding, layout.foveationLayout));

    const uint32_t bitrateMbps = currentBitrateMbps_.load();
    const uint32_t refreshHz = std::max(targetRefreshRateHz_.load(), 1u);
    const oxr::protocol::VideoCodec codec = activeVideoCodec_.load();
    if (!newEncoder->Initialize(layout.encodedWidth, layout.encodedHeight,
                                refreshHz, bitrateMbps, graphicsContext_, codec))
    {
        {
            std::lock_guard<std::mutex> lock(streamConfigMutex_);
            ResetPendingStreamConfigLocked();
        }
        spdlog::error("StreamingServer: failed to initialize encoder for stream config seq={} encoded={}x{}",
                      update.sequence, layout.encodedWidth, layout.encodedHeight);
        return;
    }

    frameQueue_.Clear();
    ClearVideoSendQueue();
    newEncoder->ForceKeyframe();

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        RenewCallbackAccess();
        encoder_ = std::move(newEncoder);
    }

    {
        std::lock_guard<std::mutex> layoutLock(streamLayoutMutex_);
        streamLayout_.activeResolutionScale = layout.resolutionScale;
        streamLayout_.scaledWidth = layout.scaledWidth;
        streamLayout_.scaledHeight = layout.scaledHeight;
        streamLayout_.encodedWidth = layout.encodedWidth;
        streamLayout_.encodedHeight = layout.encodedHeight;
        streamLayout_.foveatedTargetEyeWidth = layout.foveatedTargetEyeWidth;
        streamLayout_.foveatedTargetEyeHeight = layout.foveatedTargetEyeHeight;
        streamLayout_.foveatedEncodingActive = layout.foveatedEncodingActive;
        streamLayout_.streamConfigSequence = update.sequence;
    }
    clientFoveatedEncodingActive_.store(useFoveatedEncoding);
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        ResetPendingStreamConfigLocked();
    }

    spdlog::info("StreamingServer: applied stream config seq={} scale={:.2f} encoded={}x{} bitrate={}Mbps",
                 update.sequence, layout.resolutionScale, layout.encodedWidth, layout.encodedHeight, bitrateMbps);
}

void StreamingServer::HandleStreamConfigAck(const oxr::protocol::StreamConfigAck& ack)
{
    oxr::protocol::StreamConfigUpdate update = {};
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(streamConfigMutex_);
        const uint32_t pendingSequence = pendingStreamConfigSequence_.load();
        if (pendingSequence == 0 || ack.sequence != pendingSequence)
        {
            return;
        }
        update = pendingStreamConfigUpdate_;
        accepted = ack.status == oxr::protocol::STREAM_CONFIG_ACK_OK &&
                   ack.encodedWidth == update.encodedWidth &&
                   ack.encodedHeight == update.encodedHeight &&
                   streamConfigPending_.AcceptAck(ack.sequence, true);
        if (!accepted)
        {
            streamConfigPending_.AcceptAck(ack.sequence, false);
            ResetPendingStreamConfigLocked();
        }
    }

    if (!accepted)
    {
        spdlog::warn("StreamingServer: stream config seq={} rejected by client status={} ack={}x{} expected={}x{}",
                     ack.sequence,
                     static_cast<uint32_t>(ack.status),
                     ack.encodedWidth,
                     ack.encodedHeight,
                     update.encodedWidth,
                     update.encodedHeight);
        return;
    }

    ApplyPendingStreamConfigLocked(update);
}

void StreamingServer::HandleControlPayload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < 1)
    {
        return;
    }

    uint8_t type = data[0];
    if (type == static_cast<uint8_t>(oxr::protocol::ControlType::LatencyReport) &&
        size >= oxr::protocol::LATENCY_REPORT_BASE_SIZE)
    {
        oxr::protocol::LatencyReport report = {};
        memcpy(&report, data, std::min(size, sizeof(report)));
        HandleLatencyReport(report);
    }
    else if (type == static_cast<uint8_t>(oxr::protocol::ControlType::RequestKeyframe) &&
             size >= sizeof(oxr::protocol::RequestKeyframe))
    {
        HandleKeyframeRequest(*reinterpret_cast<const oxr::protocol::RequestKeyframe*>(data));
    }
    else if (type == static_cast<uint8_t>(oxr::protocol::ControlType::NackRequest) &&
             size >= sizeof(oxr::protocol::NackRequest))
    {
        if (!clientUsesUsbAdb_.load())
        {
            HandleNackRequest(*reinterpret_cast<const oxr::protocol::NackRequest*>(data));
        }
    }
    else if (type == static_cast<uint8_t>(oxr::protocol::ControlType::StreamConfigAck) &&
             size >= sizeof(oxr::protocol::StreamConfigAck))
    {
        HandleStreamConfigAck(*reinterpret_cast<const oxr::protocol::StreamConfigAck*>(data));
    }
}

void StreamingServer::UpdatePredictionHorizon()
{
    if (trackingReceiver_ == nullptr)
    {
        return;
    }

    float horizonMs = serverPipelineLatencyMs_.load() + clientPipelineLatencyMs_.load();
    horizonMs = std::clamp(horizonMs, 0.0f, 80.0f);
    trackingReceiver_->SetPredictionHorizonMs(horizonMs);
}

void StreamingServer::SendFrame(FrameSource frameSource,
                                const float* renderHeadOrientation,
                                const float* renderHeadPosition)
{
    if (!frameSource.IsStereoValid() || state_.load() != State::Connected)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> encoderLock(encoderMutex_);
        if (encoder_ == nullptr || !encoder_->IsInitialized())
        {
            return;
        }
    }

    StreamingFrame frame = {};
    frame.source = std::move(frameSource);
    frame.frameIndex = frameIndex_++;
    frame.timestampNs = SteadyClockNowNs();
    frame.alphaBlend = frameSource.alphaBlend;
    frame.valid = true;
    pendingFrameDepthMax_.store(std::max(pendingFrameDepthMax_.load(), 1u));

    // Tag the frame with the exact head pose it was rendered for. The application's render pose
    // (from xrLocateViews) is preferred so the client reprojects against the pose the pixels were
    // drawn with; re-predicting here would echo a pose taken later than the one rendered, leaving a
    // per-frame rotation mismatch that the client cannot correct (head-rotation jitter). Fall back
    // to the latest predicted pose only when the render pose is unavailable.
    if (renderHeadOrientation != nullptr && renderHeadPosition != nullptr)
    {
        memcpy(frame.headPosition, renderHeadPosition, sizeof(float) * 3);
        memcpy(frame.headOrientation, renderHeadOrientation, sizeof(float) * 4);
        frame.hasPose = true;
    }
    else if (trackingReceiver_ != nullptr)
    {
        oxr::protocol::TrackingPacket pose = {};
        if (trackingReceiver_->GetPredictedPose(pose))
        {
            memcpy(frame.headPosition, pose.headPosition, sizeof(float) * 3);
            memcpy(frame.headOrientation, pose.headOrientation, sizeof(float) * 4);
            frame.hasPose = true;
        }
    }

    if (frameQueue_.PushLatest(std::move(frame)))
    {
        replacedFrameCount_.fetch_add(1);
    }
}

void StreamingServer::ReleaseStreamingFrame(StreamingFrame& frame)
{
    if (!frame.valid)
    {
        return;
    }

    frame = {};
}

void StreamingServer::QueueEncodedVideoFrame(EncodedVideoFrame frame)
{
    if (frame.nals.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(videoSendMutex_);
        while (videoSendQueue_.size() >= MaxVideoSendQueueFrames)
        {
            auto droppable = std::find_if(
                videoSendQueue_.begin(),
                videoSendQueue_.end(),
                [](const EncodedVideoFrame& queuedFrame) {
                    return std::none_of(
                        queuedFrame.nals.begin(),
                        queuedFrame.nals.end(),
                        [](const EncodedNalUnit& nal) { return nal.isKeyframe; });
                });
            if (droppable != videoSendQueue_.end())
            {
                videoSendQueue_.erase(droppable);
            }
            else
            {
                videoSendQueue_.pop_front();
            }
            videoSendDroppedFrames_.fetch_add(1);
            videoSendDroppedFramesTotalForAbr_.fetch_add(1);
        }
        videoSendQueue_.push_back(std::move(frame));
        videoSendQueueDepthMax_.store(std::max(
            videoSendQueueDepthMax_.load(),
            static_cast<uint32_t>(videoSendQueue_.size())));
    }
    videoSendCv_.notify_one();
}

void StreamingServer::ClearVideoSendQueue()
{
    std::lock_guard<std::mutex> lock(videoSendMutex_);
    videoSendQueue_.clear();
}

void StreamingServer::VideoSendThread()
{
    while (running_.load())
    {
        TickPendingStreamConfigTimeout(SteadyClockNowNs());

        EncodedVideoFrame frame = {};
        bool dropStaleFrame = false;
        {
            std::unique_lock<std::mutex> lock(videoSendMutex_);
            videoSendCv_.wait(lock, [this] {
                return !running_.load() || !videoSendQueue_.empty();
            });
            if (!running_.load() && videoSendQueue_.empty())
            {
                break;
            }
            if (videoSendQueue_.empty())
            {
                continue;
            }

            frame = std::move(videoSendQueue_.front());
            videoSendQueue_.pop_front();

            const bool newerFrameQueued = !videoSendQueue_.empty();
            const bool hasKeyframeNal = std::any_of(
                frame.nals.begin(), frame.nals.end(),
                [](const EncodedNalUnit& nal) { return nal.isKeyframe; });
            dropStaleFrame = newerFrameQueued && !hasKeyframeNal;
        }

        if (dropStaleFrame)
        {
            videoSendDroppedFrames_.fetch_add(1);
            videoSendDroppedFramesTotalForAbr_.fetch_add(1);
            continue;
        }

        SendEncodedVideoFrame(frame);
    }

    ClearVideoSendQueue();
    spdlog::info("StreamingServer: video send thread ended");
}

void StreamingServer::SendEncodedVideoFrame(const EncodedVideoFrame& frame)
{
    if (frame.hasPose)
    {
        SendRenderPosePacket(frame);
    }

    for (const EncodedNalUnit& nal : frame.nals)
    {
        if (nal.tcpPayload.size() <= sizeof(oxr::protocol::TcpVideoNalHeader))
        {
            continue;
        }

        const auto* nalHeader =
            reinterpret_cast<const oxr::protocol::TcpVideoNalHeader*>(nal.tcpPayload.data());
        const uint8_t* nalData =
            nal.tcpPayload.data() + sizeof(oxr::protocol::TcpVideoNalHeader);
        const size_t nalSize = std::min<size_t>(
            nalHeader->payloadSize,
            nal.tcpPayload.size() - sizeof(oxr::protocol::TcpVideoNalHeader));
        SendNalUnit(packetDispatchState_, frame.frameIndex, nalData, nalSize,
                    nal.isKeyframe, frame.alphaBlend, frame.timestampNs, frame.codec);
    }
}

void StreamingServer::SendRenderPosePacket(const EncodedVideoFrame& frame)
{
    std::string clientIp;
    SocketHandle videoSocket = oxrsys::runtime_socket::InvalidSocket;
    bool videoUsesTcp = false;
    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        if (!packetDispatchState_->acceptingPackets)
        {
            return;
        }
        clientIp = packetDispatchState_->clientIp;
        videoSocket = packetDispatchState_->videoSocket;
        videoUsesTcp = packetDispatchState_->videoUsesTcp;
    }

    if (!oxrsys::runtime_socket::IsValid(videoSocket) || (!videoUsesTcp && clientIp.empty()))
    {
        return;
    }

    if (videoUsesTcp)
    {
        oxr::protocol::TcpRenderPose pose = {};
        pose.frameIndex = frame.frameIndex;
        pose.presentationTimeNs = frame.timestampNs;
        memcpy(pose.position, frame.headPosition, sizeof(float) * 3);
        memcpy(pose.orientation, frame.headOrientation, sizeof(float) * 4);

        std::lock_guard<std::mutex> sendLock(packetDispatchState_->sendMutex);
        if (!SendTcpRecord(videoSocket, oxr::protocol::TcpRecordType::RenderPose,
                           &pose, sizeof(pose)))
        {
            videoTcpSendFailures_.fetch_add(1);
            MarkTcpVideoSendFailed(packetDispatchState_, videoSocket);
        }
        return;
    }

    float posePayload[7];
    memcpy(posePayload, frame.headPosition, sizeof(float) * 3);
    memcpy(posePayload + 3, frame.headOrientation, sizeof(float) * 4);

    oxr::protocol::VideoPacketHeader poseHeader = {};
    poseHeader.frameIndex = frame.frameIndex;
    poseHeader.packetIndex = 0;
    poseHeader.totalPackets = 0;
    poseHeader.payloadSize = sizeof(posePayload);
    poseHeader.flags = oxr::protocol::VIDEO_FLAG_RENDER_POSE;
    poseHeader.codec = static_cast<uint8_t>(frame.codec);
    poseHeader.presentationTimeNs = frame.timestampNs;

    uint8_t buf[sizeof(poseHeader) + sizeof(posePayload)];
    memcpy(buf, &poseHeader, sizeof(poseHeader));
    memcpy(buf + sizeof(poseHeader), posePayload, sizeof(posePayload));

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
    inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);
    oxrsys::runtime_socket::SendTo(videoSocket,
                                   buf,
                                   sizeof(buf),
                                   kBestEffortSendFlags,
                                   (sockaddr*)&destAddr,
                                   sizeof(destAddr));
}

void StreamingServer::SendNalUnit(const std::shared_ptr<PacketDispatchState>& dispatchState,
                                  uint32_t frameIndex, const uint8_t* data, size_t size,
                                  bool isKeyframe, bool alphaBlend, int64_t timestampNs,
                                  oxr::protocol::VideoCodec codec)
{
    std::string clientIp;
    SocketHandle videoSocket = oxrsys::runtime_socket::InvalidSocket;
    bool videoUsesTcp = false;
    {
        if (!dispatchState)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        if (!dispatchState->acceptingPackets)
        {
            return;
        }
        clientIp = dispatchState->clientIp;
        videoSocket = dispatchState->videoSocket;
        videoUsesTcp = dispatchState->videoUsesTcp;
    }

    if (!oxrsys::runtime_socket::IsValid(videoSocket) || (!videoUsesTcp && clientIp.empty()))
    {
        return;
    }

    if (videoUsesTcp)
    {
        if (size > oxr::protocol::TCP_MAX_RECORD_PAYLOAD - sizeof(oxr::protocol::TcpVideoNalHeader))
        {
            spdlog::warn("StreamingServer: USB TCP NAL too large ({} bytes), dropping", size);
            return;
        }

        oxr::protocol::TcpVideoNalHeader nalHeader = {};
        nalHeader.presentationTimeNs = timestampNs;
        nalHeader.frameIndex = frameIndex;
        nalHeader.payloadSize = static_cast<uint32_t>(size);
        nalHeader.flags = oxr::protocol::VIDEO_FLAG_STEREO;
        if (alphaBlend)
        {
            nalHeader.flags |= oxr::protocol::VIDEO_FLAG_ALPHA_BLEND;
        }
        if (isKeyframe)
        {
            nalHeader.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
        }
        nalHeader.codec = static_cast<uint8_t>(codec);

        std::lock_guard<std::mutex> sendLock(dispatchState->sendMutex);
        if (!SendTcpRecordParts(videoSocket,
                                oxr::protocol::TcpRecordType::VideoNal,
                                &nalHeader,
                                sizeof(nalHeader),
                                data,
                                size))
        {
            videoTcpSendFailures_.fetch_add(1);
            MarkTcpVideoSendFailed(dispatchState, videoSocket);
        }
        return;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
    inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);

    uint16_t totalPackets = static_cast<uint16_t>(
        (size + oxr::protocol::MAX_PACKET_PAYLOAD - 1) / oxr::protocol::MAX_PACKET_PAYLOAD);

    uint8_t packetBuffer[oxr::protocol::VIDEO_PACKET_SIZE];

    // Prepare cache slot for this frame's packets (for NACK retransmission).
    size_t cacheIndex = 0;
    {
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        cacheIndex = dispatchState->cacheWriteIndex % PacketDispatchState::MaxCachedFrames;
        CachedFrame& cachedFrame = dispatchState->cachedFrames[cacheIndex];
        cachedFrame.frameIndex = frameIndex;
        cachedFrame.packetCount = totalPackets;
        cachedFrame.complete = false;
        if (cachedFrame.packets.size() < totalPackets)
        {
            cachedFrame.packets.resize(totalPackets);
        }
        for (size_t i = 0; i < totalPackets; ++i)
        {
            cachedFrame.packets[i].size = 0;
        }
        dispatchState->cacheWriteIndex++;
    }

    // Collect per-packet payload pointers and sizes for FEC generation
    thread_local std::vector<const uint8_t*> payloadPtrs;
    thread_local std::vector<uint16_t> payloadSizes;
    payloadPtrs.resize(totalPackets);
    payloadSizes.resize(totalPackets);

    size_t offset = 0;
    for (uint16_t i = 0; i < totalPackets; i++)
    {
        size_t payloadSize = std::min(size - offset, oxr::protocol::MAX_PACKET_PAYLOAD);
        payloadPtrs[i] = data + offset;
        payloadSizes[i] = static_cast<uint16_t>(payloadSize);

        oxr::protocol::VideoPacketHeader header = {};
        header.frameIndex = frameIndex;
        header.packetIndex = i;
        header.totalPackets = totalPackets;
        header.payloadSize = static_cast<uint16_t>(payloadSize);
        header.flags = oxr::protocol::VIDEO_FLAG_STEREO;
        if (alphaBlend)
        {
            header.flags |= oxr::protocol::VIDEO_FLAG_ALPHA_BLEND;
        }
        if (isKeyframe)
        {
            header.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
        }
        if (i == totalPackets - 1)
        {
            header.flags |= oxr::protocol::VIDEO_FLAG_END_OF_FRAME;
        }
        header.codec = static_cast<uint8_t>(codec);
        header.presentationTimeNs = timestampNs;

        size_t packetSize = sizeof(header) + payloadSize;
        memcpy(packetBuffer, &header, sizeof(header));
        memcpy(packetBuffer + sizeof(header), data + offset, payloadSize);
        oxrsys::runtime_socket::SendTo(videoSocket,
                                       packetBuffer,
                                       packetSize,
                                       kBestEffortSendFlags,
                                       (sockaddr*)&destAddr,
                                       sizeof(destAddr));

        // Cache for NACK retransmission. The frame is advertised to NACK only
        // once all packets have been copied.
        {
            std::lock_guard<std::mutex> lock(dispatchState->mutex);
            CachedFrame& cachedFrame = dispatchState->cachedFrames[cacheIndex];
            if (cachedFrame.frameIndex == frameIndex && i < cachedFrame.packets.size())
            {
                CachedPacket& packet = cachedFrame.packets[i];
                memcpy(packet.data.data(), packetBuffer, packetSize);
                packet.size = packetSize;
            }
        }

        offset += payloadSize;

        // After each complete FEC group (or at the end of the frame), send a parity packet
        uint32_t nextIdx = i + 1;
        bool isGroupEnd = (nextIdx % oxr::protocol::FEC_GROUP_SIZE == 0);
        bool isFrameEnd = (nextIdx == totalPackets);
        if (isGroupEnd || isFrameEnd)
        {
            uint32_t groupIndex = i / oxr::protocol::FEC_GROUP_SIZE;
            uint32_t groupStart = groupIndex * oxr::protocol::FEC_GROUP_SIZE;
            uint32_t groupCount = nextIdx - groupStart;

            uint8_t fecPayload[oxr::protocol::MAX_PACKET_PAYLOAD];
            oxr::fec::Encode(&payloadPtrs[groupStart], &payloadSizes[groupStart],
                             groupCount, fecPayload);

            oxr::protocol::VideoPacketHeader fecHeader = {};
            fecHeader.frameIndex = frameIndex;
            fecHeader.packetIndex = static_cast<uint16_t>(groupIndex);
            fecHeader.totalPackets = totalPackets;
            fecHeader.payloadSize = static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD);
            fecHeader.flags = oxr::protocol::VIDEO_FLAG_FEC | oxr::protocol::VIDEO_FLAG_STEREO;
            if (alphaBlend)
            {
                fecHeader.flags |= oxr::protocol::VIDEO_FLAG_ALPHA_BLEND;
            }
            fecHeader.fecGroupLastPacketPayloadSize =
                payloadSizes[groupStart + groupCount - 1];
            if (isKeyframe)
            {
                fecHeader.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
            }
            fecHeader.codec = static_cast<uint8_t>(codec);
            fecHeader.presentationTimeNs = timestampNs;

            memcpy(packetBuffer, &fecHeader, sizeof(fecHeader));
            memcpy(packetBuffer + sizeof(fecHeader), fecPayload, oxr::protocol::MAX_PACKET_PAYLOAD);
            oxrsys::runtime_socket::SendTo(
                videoSocket,
                packetBuffer,
                sizeof(fecHeader) + oxr::protocol::MAX_PACKET_PAYLOAD,
                kBestEffortSendFlags,
                (sockaddr*)&destAddr,
                sizeof(destAddr));
        }
    }

    {
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        CachedFrame& cachedFrame = dispatchState->cachedFrames[cacheIndex];
        if (cachedFrame.frameIndex == frameIndex)
        {
            cachedFrame.complete = true;
        }
    }
}

bool StreamingServer::SendTcpRecord(SocketHandle socket, oxr::protocol::TcpRecordType type,
                                    const void* payload, size_t payloadSize)
{
    if (!oxrsys::runtime_socket::IsValid(socket) ||
        payloadSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }

    oxr::protocol::TcpRecordHeader header = {};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payloadSize);
    if (!SendAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (payloadSize == 0)
    {
        return true;
    }
    return SendAll(socket, payload, payloadSize);
}

bool StreamingServer::SendTcpRecordParts(SocketHandle socket, oxr::protocol::TcpRecordType type,
                                         const void* firstPayload, size_t firstPayloadSize,
                                         const void* secondPayload, size_t secondPayloadSize)
{
    const size_t payloadSize = firstPayloadSize + secondPayloadSize;
    if (!oxrsys::runtime_socket::IsValid(socket) ||
        payloadSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD ||
        (firstPayloadSize > 0 && firstPayload == nullptr) ||
        (secondPayloadSize > 0 && secondPayload == nullptr))
    {
        return false;
    }

    oxr::protocol::TcpRecordHeader header = {};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payloadSize);
    if (!SendAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (firstPayloadSize > 0 && !SendAll(socket, firstPayload, firstPayloadSize))
    {
        return false;
    }
    if (secondPayloadSize > 0 && !SendAll(socket, secondPayload, secondPayloadSize))
    {
        return false;
    }
    return true;
}

void StreamingServer::MarkTcpVideoSendFailed(
    const std::shared_ptr<PacketDispatchState>& dispatchState,
    SocketHandle socket)
{
    if (!dispatchState || !oxrsys::runtime_socket::IsValid(socket))
    {
        return;
    }

    bool clearedDispatch = false;
    {
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        if (dispatchState->videoUsesTcp && dispatchState->videoSocket == socket)
        {
            dispatchState->videoSocket = oxrsys::runtime_socket::InvalidSocket;
            dispatchState->videoUsesTcp = false;
            dispatchState->acceptingPackets = false;
            clearedDispatch = true;
        }
    }

    if (clearedDispatch)
    {
        oxrsys::runtime_socket::Shutdown(socket);
        spdlog::warn("StreamingServer: USB TCP video send failed; disabled stale video dispatch");
    }
}

void StreamingServer::HandleNackRequest(const oxr::protocol::NackRequest& request)
{
    struct RetransmitPacket
    {
        std::array<uint8_t, oxr::protocol::VIDEO_PACKET_SIZE> data = {};
        size_t size = 0;
    };

    std::string clientIp;
    SocketHandle videoSocket = oxrsys::runtime_socket::InvalidSocket;
    std::vector<RetransmitPacket> retransmitPackets;
    retransmitPackets.reserve(static_cast<size_t>(__builtin_popcountll(request.missingBitmask)));

    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        if (!packetDispatchState_->acceptingPackets || packetDispatchState_->clientIp.empty() ||
            !oxrsys::runtime_socket::IsValid(packetDispatchState_->videoSocket))
        {
            return;
        }

        const CachedFrame* cached = nullptr;
        for (size_t i = 0; i < PacketDispatchState::MaxCachedFrames; i++)
        {
            if (packetDispatchState_->cachedFrames[i].frameIndex == request.frameIndex &&
                packetDispatchState_->cachedFrames[i].packetCount == request.totalPackets &&
                packetDispatchState_->cachedFrames[i].complete &&
                !packetDispatchState_->cachedFrames[i].packets.empty())
            {
                cached = &packetDispatchState_->cachedFrames[i];
                break;
            }
        }

        if (cached == nullptr)
        {
            spdlog::debug("StreamingServer: NACK for frame {} but not in cache", request.frameIndex);
            return;
        }

        for (uint32_t bit = 0; bit < 64; bit++)
        {
            if (!(request.missingBitmask & (1ULL << bit)))
            {
                continue;
            }

            uint32_t pktIdx = request.packetIndexStart + bit;
            if (pktIdx >= cached->packetCount ||
                pktIdx >= cached->packets.size() ||
                cached->packets[pktIdx].size == 0)
            {
                continue;
            }

            RetransmitPacket packet = {};
            packet.size = cached->packets[pktIdx].size;
            memcpy(packet.data.data(), cached->packets[pktIdx].data.data(), packet.size);
            retransmitPackets.push_back(packet);
        }

        clientIp = packetDispatchState_->clientIp;
        videoSocket = packetDispatchState_->videoSocket;
    }

    if (retransmitPackets.empty())
    {
        return;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
    inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);

    uint32_t retransmitted = 0;
    for (const RetransmitPacket& packet : retransmitPackets)
    {
        oxrsys::runtime_socket::SendTo(videoSocket,
                                       packet.data.data(),
                                       packet.size,
                                       kBestEffortSendFlags,
                                       (sockaddr*)&destAddr,
                                       sizeof(destAddr));
        retransmitted++;
    }

    if (retransmitted > 0)
    {
        videoUdpRetransmittedPackets_.fetch_add(retransmitted);
        spdlog::info("StreamingServer: NACK retransmitted {}/{} packets for frame {}",
                      retransmitted, __builtin_popcountll(request.missingBitmask),
                      request.frameIndex);
    }
}

std::string StreamingServer::GetClientName() const
{
    std::lock_guard<std::mutex> lock(clientMutex_);
    return clientName_;
}

std::string StreamingServer::GetLocalIpAddress() const
{
#if defined(_WIN32)
    if (!oxrsys::runtime_socket::EnsureInitialized())
    {
        return "0.0.0.0";
    }

    char hostName[256] = {};
    if (gethostname(hostName, sizeof(hostName)) != 0)
    {
        return "0.0.0.0";
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(hostName, nullptr, &hints, &addresses) != 0)
    {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";
    for (addrinfo* current = addresses; current != nullptr; current = current->ai_next)
    {
        if (current->ai_addr == nullptr || current->ai_addr->sa_family != AF_INET)
        {
            continue;
        }
        auto* addr = reinterpret_cast<sockaddr_in*>(current->ai_addr);
        const uint32_t hostAddress = ntohl(addr->sin_addr.s_addr);
        if ((hostAddress >> 24) == 127)
        {
            continue;
        }

        char ipStr[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr)) != nullptr)
        {
            result = ipStr;
            break;
        }
    }

    freeaddrinfo(addresses);
    return result;
#else
    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0)
    {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";
    for (struct ifaddrs* ifa = ifas; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK)
        {
            continue;
        }

        auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

        std::string ifName(ifa->ifa_name);
        if (ifName == "en0")
        {
            result = ipStr;
            break;
        }
        if (result == "0.0.0.0")
        {
            result = ipStr;
        }
    }

    freeifaddrs(ifas);
    return result;
#endif
}
