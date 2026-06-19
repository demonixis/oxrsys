// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "RuntimeStatus.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::ostringstream output;
    output << file.rdbuf();
    return output.str();
}

bool Contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::filesystem::path RuntimeStatusPathForHome(const std::filesystem::path& home)
{
#if defined(__APPLE__)
    return home / "Library/Application Support/OXRSys/runtime_status.json";
#else
    return home / ".local/state/oxrsys/runtime_status.json";
#endif
}

} // namespace

TEST_CASE("RuntimeStatus writes streaming stats only while streaming", "[runtime-status]")
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        ("openxr-runtime-status-test-" + std::to_string(suffix));
    std::filesystem::create_directories(home);

    setenv("HOME", home.string().c_str(), 1);

    RuntimeStatus::SetApplicationName("Status Test");
    RuntimeStatus::SetStreaming("usb_adb", "Quest 3");

    RuntimeStatus::StreamingStats stats = {};
    stats.sampleUnixMilliseconds = 1800000000000;
    stats.refreshRateHz = 90;
    stats.currentBitrateMbps = 42;
    stats.maxBitrateMbps = 50;
    stats.renderWidth = 3664;
    stats.renderHeight = 1920;
    stats.encodedWidth = 2752;
    stats.encodedHeight = 1440;
    stats.encoderPreset = "quality";
    stats.foveatedEncodingPreset = "medium";
    stats.clientFoveationPreset = "high";
    stats.clientUpscaling = true;
    stats.headsetAudio = false;
    stats.serverPipelineLatencyMs = 12.5;
    stats.clientPipelineLatencyMs = 18.25;
    stats.clientReceiveToSubmitMs = 1.5;
    stats.clientDecodeMs = 6.75;
    stats.clientCompositorMs = 11.0;
    stats.predictionHorizonMs = 30.75;
    stats.encodeQueueAverageMs = 0.5;
    stats.encodeQueueP95Ms = 1.25;
    stats.encodeGpuAverageMs = 2.0;
    stats.encodeGpuP95Ms = 3.5;
    stats.encodeSubmitAverageMs = 0.1;
    stats.encodeSubmitP95Ms = 0.2;
    stats.encodeCallbackAverageMs = 4.0;
    stats.encodeCallbackP95Ms = 5.5;
    stats.encodeTotalAverageMs = 8.0;
    stats.encodeTotalP95Ms = 9.5;
    stats.encodedFramesTotal = 120;
    stats.encoderDroppedFramesTotal = 2;
    stats.replacedFramesDelta = 3;
    stats.keyframeRequestsDelta = 1;
    stats.pendingDepthMax = 1;
    stats.videoSendQueueDepthMax = 2;
    stats.videoSendDroppedFramesDelta = 4;
    stats.videoTcpSendFailuresDelta = 1;
    stats.videoUdpRetransmittedPacketsDelta = 7;
    RuntimeStatus::SetStreamingStats(stats);

    const auto statusPath = RuntimeStatusPathForHome(home);
    const std::string streamingStatus = ReadFile(statusPath);

    CHECK(Contains(streamingStatus, "\"state\": \"streaming\""));
    CHECK(Contains(streamingStatus, "\"transport\": \"usb_adb\""));
    CHECK(Contains(streamingStatus, "\"streaming_stats\""));
    CHECK(Contains(streamingStatus, "\"sample_unix_ms\": 1800000000000"));
    CHECK(Contains(streamingStatus, "\"refresh_rate_hz\": 90"));
    CHECK(Contains(streamingStatus, "\"current_bitrate_mbps\": 42"));
    CHECK(Contains(streamingStatus, "\"encoder_preset\": \"quality\""));
    CHECK(Contains(streamingStatus, "\"foveated_encoding_preset\": \"medium\""));
    CHECK(Contains(streamingStatus, "\"client_foveation_preset\": \"high\""));
    CHECK(Contains(streamingStatus, "\"client_upscaling\": true"));
    CHECK(Contains(streamingStatus, "\"headset_audio\": false"));
    CHECK(Contains(streamingStatus, "\"server_pipeline\": 12.5"));
    CHECK(Contains(streamingStatus, "\"total_p95\": 9.5"));
    CHECK(Contains(streamingStatus, "\"encoded_frames_total\": 120"));
    CHECK(Contains(streamingStatus, "\"keyframe_requests_delta\": 1"));
    CHECK(Contains(streamingStatus, "\"video_send_queue_depth_max\": 2"));
    CHECK(Contains(streamingStatus, "\"video_send_dropped_frames_delta\": 4"));
    CHECK(Contains(streamingStatus, "\"video_tcp_send_failures_delta\": 1"));
    CHECK(Contains(streamingStatus, "\"video_udp_retransmitted_packets_delta\": 7"));

    RuntimeStatus::SetIdle();
    const std::string idleStatus = ReadFile(statusPath);

    CHECK(Contains(idleStatus, "\"state\": \"idle\""));
    CHECK(!Contains(idleStatus, "\"streaming_stats\""));

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}
