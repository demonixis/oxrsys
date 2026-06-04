// SPDX-License-Identifier: MPL-2.0

#include "RuntimeStatus.h"
#include "Config.h"
#include "RuntimePlatform.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace
{

std::mutex& StatusMutex()
{
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::string& ApplicationName()
{
    static auto* name = new std::string();
    return *name;
}

std::string& CurrentState()
{
    static auto* state = new std::string("idle");
    return *state;
}

std::string& CurrentTransport()
{
    static auto* transport = new std::string();
    return *transport;
}

std::string& CurrentClientName()
{
    static auto* clientName = new std::string();
    return *clientName;
}

RuntimeStatus::StreamingStats& CurrentStreamingStats()
{
    static auto* stats = new RuntimeStatus::StreamingStats();
    return *stats;
}

bool& HasStreamingStats()
{
    static auto* hasStats = new bool(false);
    return *hasStats;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream output;
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20)
                {
                    output << "\\u";
                    output << "00";
                    const char* hex = "0123456789abcdef";
                    output << hex[(character >> 4) & 0x0f];
                    output << hex[character & 0x0f];
                }
                else
                {
                    output << static_cast<char>(character);
                }
                break;
        }
    }
    return output.str();
}

std::string DeviceTypeForClientName(std::string clientName)
{
    std::transform(clientName.begin(), clientName.end(), clientName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (clientName.find("quest") != std::string::npos)
    {
        return "quest";
    }
    if (clientName.find("pico") != std::string::npos)
    {
        return "pico";
    }
    if (clientName.find("vision") != std::string::npos)
    {
        return "vision_pro";
    }
    if (clientName.find("simulator") != std::string::npos ||
        clientName.find("viewer") != std::string::npos)
    {
        return "simulator";
    }
    if (!clientName.empty())
    {
        return "unknown";
    }
    return "";
}

int64_t UnixTimeMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void WriteStreamingStats(std::ofstream& file, const RuntimeStatus::StreamingStats& stats)
{
    file << "  \"streaming_stats\": {\n";
    file << "    \"sample_unix_ms\": " << static_cast<long long>(stats.sampleUnixMilliseconds) << ",\n";
    file << "    \"refresh_rate_hz\": " << stats.refreshRateHz << ",\n";
    file << "    \"current_bitrate_mbps\": " << stats.currentBitrateMbps << ",\n";
    file << "    \"max_bitrate_mbps\": " << stats.maxBitrateMbps << ",\n";
    file << "    \"render_width\": " << stats.renderWidth << ",\n";
    file << "    \"render_height\": " << stats.renderHeight << ",\n";
    file << "    \"encoded_width\": " << stats.encodedWidth << ",\n";
    file << "    \"encoded_height\": " << stats.encodedHeight << ",\n";
    file << "    \"latency_ms\": {\n";
    file << "      \"server_pipeline\": " << stats.serverPipelineLatencyMs << ",\n";
    file << "      \"client_pipeline\": " << stats.clientPipelineLatencyMs << ",\n";
    file << "      \"client_receive_to_submit\": " << stats.clientReceiveToSubmitMs << ",\n";
    file << "      \"client_decode\": " << stats.clientDecodeMs << ",\n";
    file << "      \"client_compositor\": " << stats.clientCompositorMs << ",\n";
    file << "      \"prediction_horizon\": " << stats.predictionHorizonMs << "\n";
    file << "    },\n";
    file << "    \"encode_ms\": {\n";
    file << "      \"queue_avg\": " << stats.encodeQueueAverageMs << ",\n";
    file << "      \"queue_p95\": " << stats.encodeQueueP95Ms << ",\n";
    file << "      \"gpu_avg\": " << stats.encodeGpuAverageMs << ",\n";
    file << "      \"gpu_p95\": " << stats.encodeGpuP95Ms << ",\n";
    file << "      \"submit_avg\": " << stats.encodeSubmitAverageMs << ",\n";
    file << "      \"submit_p95\": " << stats.encodeSubmitP95Ms << ",\n";
    file << "      \"callback_avg\": " << stats.encodeCallbackAverageMs << ",\n";
    file << "      \"callback_p95\": " << stats.encodeCallbackP95Ms << ",\n";
    file << "      \"total_avg\": " << stats.encodeTotalAverageMs << ",\n";
    file << "      \"total_p95\": " << stats.encodeTotalP95Ms << "\n";
    file << "    },\n";
    file << "    \"counters\": {\n";
    file << "      \"encoded_frames_total\": " << stats.encodedFramesTotal << ",\n";
    file << "      \"encoder_dropped_frames_total\": " << stats.encoderDroppedFramesTotal << ",\n";
    file << "      \"replaced_frames_delta\": " << stats.replacedFramesDelta << ",\n";
    file << "      \"keyframe_requests_delta\": " << stats.keyframeRequestsDelta << ",\n";
    file << "      \"pending_depth_max\": " << stats.pendingDepthMax << "\n";
    file << "    }\n";
    file << "  }\n";
}

void WriteStatusLocked(const std::string& state,
                       const std::string& transport,
                       const std::string& clientName)
{
    try
    {
        const Config& config = Config::Get();
        std::filesystem::create_directories(config.appSupportDir);

        const std::string deviceType = DeviceTypeForClientName(clientName);
        const std::string tempPath = config.runtimeStatusPath + ".tmp";

        std::ofstream file(tempPath, std::ios::trunc);
        if (!file.is_open())
        {
            return;
        }

        file << "{\n";
        file << "  \"state\": \"" << JsonEscape(state) << "\",\n";
        file << "  \"transport\": \"" << JsonEscape(transport) << "\",\n";
        file << "  \"device_type\": \"" << JsonEscape(deviceType) << "\",\n";
        file << "  \"client_name\": \"" << JsonEscape(clientName) << "\",\n";
        file << "  \"application_name\": \"" << JsonEscape(ApplicationName()) << "\",\n";
        file << "  \"process_id\": "
             << static_cast<long long>(oxrsys::runtime_platform::ProcessId()) << ",\n";
        file << "  \"updated_at_unix_ms\": " << UnixTimeMilliseconds();
        if (state == "streaming" && HasStreamingStats())
        {
            file << ",\n";
            WriteStreamingStats(file, CurrentStreamingStats());
        }
        else
        {
            file << "\n";
        }
        file << "}\n";
        file.close();

        std::filesystem::rename(tempPath, config.runtimeStatusPath);
    }
    catch (const std::exception& ex)
    {
        (void)ex;
    }
    catch (...)
    {
    }
}

} // namespace

void RuntimeStatus::SetApplicationName(const std::string& applicationName)
{
    std::lock_guard<std::mutex> lock(StatusMutex());
    ApplicationName() = applicationName;
    CurrentState() = "idle";
    CurrentTransport().clear();
    CurrentClientName().clear();
    HasStreamingStats() = false;
    WriteStatusLocked("idle", "", "");
}

void RuntimeStatus::ClearApplicationName()
{
    std::lock_guard<std::mutex> lock(StatusMutex());
    ApplicationName().clear();
    CurrentState() = "idle";
    CurrentTransport().clear();
    CurrentClientName().clear();
    HasStreamingStats() = false;
    WriteStatusLocked("idle", "", "");
}

void RuntimeStatus::SetIdle()
{
    std::lock_guard<std::mutex> lock(StatusMutex());
    CurrentState() = "idle";
    CurrentTransport().clear();
    CurrentClientName().clear();
    HasStreamingStats() = false;
    WriteStatusLocked("idle", "", "");
}

void RuntimeStatus::SetStreaming(const std::string& transport, const std::string& clientName)
{
    std::lock_guard<std::mutex> lock(StatusMutex());
    CurrentState() = "streaming";
    CurrentTransport() = transport;
    CurrentClientName() = clientName;
    HasStreamingStats() = false;
    WriteStatusLocked("streaming", transport, clientName);
}

void RuntimeStatus::SetStreamingStats(const StreamingStats& stats)
{
    std::lock_guard<std::mutex> lock(StatusMutex());
    if (CurrentState() != "streaming")
    {
        return;
    }

    CurrentStreamingStats() = stats;
    if (CurrentStreamingStats().sampleUnixMilliseconds == 0)
    {
        CurrentStreamingStats().sampleUnixMilliseconds = UnixTimeMilliseconds();
    }
    HasStreamingStats() = true;
    WriteStatusLocked("streaming", CurrentTransport(), CurrentClientName());
}
