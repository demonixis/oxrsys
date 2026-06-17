// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

class RuntimeStatus
{
public:
    struct StreamingStats
    {
        int64_t sampleUnixMilliseconds = 0;
        uint32_t refreshRateHz = 0;
        uint32_t currentBitrateMbps = 0;
        uint32_t maxBitrateMbps = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t encodedWidth = 0;
        uint32_t encodedHeight = 0;
        std::string encoderPreset;
        std::string foveatedEncodingPreset;
        std::string clientFoveationPreset;
        bool clientUpscaling = false;
        bool headsetAudio = false;

        double serverPipelineLatencyMs = 0.0;
        double clientPipelineLatencyMs = 0.0;
        double clientReceiveToSubmitMs = 0.0;
        double clientDecodeMs = 0.0;
        double clientCompositorMs = 0.0;
        double predictionHorizonMs = 0.0;

        double encodeQueueAverageMs = 0.0;
        double encodeQueueP95Ms = 0.0;
        double encodeGpuAverageMs = 0.0;
        double encodeGpuP95Ms = 0.0;
        double encodeSubmitAverageMs = 0.0;
        double encodeSubmitP95Ms = 0.0;
        double encodeCallbackAverageMs = 0.0;
        double encodeCallbackP95Ms = 0.0;
        double encodeTotalAverageMs = 0.0;
        double encodeTotalP95Ms = 0.0;

        uint32_t encodedFramesTotal = 0;
        uint32_t encoderDroppedFramesTotal = 0;
        uint32_t replacedFramesDelta = 0;
        uint32_t keyframeRequestsDelta = 0;
        uint32_t pendingDepthMax = 0;
    };

    static void SetApplicationName(const std::string& applicationName);
    static void ClearApplicationName();
    static void SetIdle();
    static void SetStreaming(const std::string& transport, const std::string& clientName);
    static void SetStreamingStats(const StreamingStats& stats);
};
