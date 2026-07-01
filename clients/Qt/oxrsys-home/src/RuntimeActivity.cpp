// SPDX-License-Identifier: MPL-2.0

#include "RuntimeActivity.h"

#include "PlatformSupport.h"

#include <QFile>
#include <QJsonDocument>

namespace
{

int intValue(const QJsonObject& object, const QString& key)
{
    return object.value(key).toInt();
}

qint64 int64Value(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble())
    {
        return static_cast<qint64>(value.toDouble());
    }
    return 0;
}

double doubleValue(const QJsonObject& object, const QString& key)
{
    return object.value(key).toDouble();
}

QString nonEmptyString(const QJsonObject& object, const QString& key)
{
    const QString value = object.value(key).toString();
    return value.isEmpty() ? QString() : value;
}

} // namespace

RuntimeStreamingStats RuntimeStreamingStats::parse(const QJsonObject& object)
{
    RuntimeStreamingStats stats;
    const QJsonObject latency = object.value("latency_ms").toObject();
    const QJsonObject encode = object.value("encode_ms").toObject();
    const QJsonObject counters = object.value("counters").toObject();

    stats.sampleUnixMilliseconds = int64Value(object, "sample_unix_ms");
    stats.refreshRateHz = intValue(object, "refresh_rate_hz");
    stats.currentBitrateMbps = intValue(object, "current_bitrate_mbps");
    stats.maxBitrateMbps = intValue(object, "max_bitrate_mbps");
    stats.renderWidth = intValue(object, "render_width");
    stats.renderHeight = intValue(object, "render_height");
    stats.encodedWidth = intValue(object, "encoded_width");
    stats.encodedHeight = intValue(object, "encoded_height");
    stats.videoCodec = object.value("video_codec").toString();
    stats.encoderPreset = object.value("encoder_preset").toString();
    stats.foveatedEncodingPreset = object.value("foveated_encoding_preset").toString();
    stats.clientFoveationPreset = object.value("client_foveation_preset").toString();
    stats.clientUpscaling = object.value("client_upscaling").toBool();
    stats.clientReprojectionMode = object.value("client_reprojection_mode").toString();
    stats.abrMode = object.value("abr_mode").toString();
    stats.abrState = object.value("abr_state").toString();
    stats.abrProfile = object.value("abr_profile").toString();
    stats.resolutionScale = doubleValue(object, "resolution_scale");
    stats.dynamicResolutionMinScale = doubleValue(object, "dynamic_resolution_min_scale");
    stats.streamReconfigure = object.value("stream_reconfigure").toBool();
    stats.streamConfigSequence = intValue(object, "stream_config_sequence");
    stats.passthroughEnabled = object.value("passthrough_enabled").toBool();
    stats.passthroughSupported = object.value("passthrough_supported").toBool();
    stats.passthroughReady = object.value("passthrough_ready").toBool();
    stats.occlusionMode = object.value("occlusion_mode").toString();
    stats.spatialEnabled = object.value("spatial_enabled").toBool();
    stats.headsetAudio = object.value("headset_audio").toBool();

    stats.serverPipelineMs = doubleValue(latency, "server_pipeline");
    stats.clientPipelineMs = doubleValue(latency, "client_pipeline");
    stats.clientReceiveToSubmitMs = doubleValue(latency, "client_receive_to_submit");
    stats.clientDecodeMs = doubleValue(latency, "client_decode");
    stats.clientCompositorMs = doubleValue(latency, "client_compositor");
    stats.predictionHorizonMs = doubleValue(latency, "prediction_horizon");
    stats.displayedFrameAgeMs = doubleValue(latency, "displayed_frame_age");

    stats.encodeQueueAverageMs = doubleValue(encode, "queue_avg");
    stats.encodeQueueP95Ms = doubleValue(encode, "queue_p95");
    stats.encodeGpuAverageMs = doubleValue(encode, "gpu_avg");
    stats.encodeGpuP95Ms = doubleValue(encode, "gpu_p95");
    stats.encodeSubmitAverageMs = doubleValue(encode, "submit_avg");
    stats.encodeSubmitP95Ms = doubleValue(encode, "submit_p95");
    stats.encodeCallbackAverageMs = doubleValue(encode, "callback_avg");
    stats.encodeCallbackP95Ms = doubleValue(encode, "callback_p95");
    stats.encodeTotalAverageMs = doubleValue(encode, "total_avg");
    stats.encodeTotalP95Ms = doubleValue(encode, "total_p95");

    stats.encodedFramesTotal = intValue(counters, "encoded_frames_total");
    stats.encoderDroppedFramesTotal = intValue(counters, "encoder_dropped_frames_total");
    stats.replacedFramesDelta = intValue(counters, "replaced_frames_delta");
    stats.keyframeRequestsDelta = intValue(counters, "keyframe_requests_delta");
    stats.pendingDepthMax = intValue(counters, "pending_depth_max");
    stats.reprojectedFramesDelta = intValue(counters, "reprojected_frames_delta");
    stats.staleFrameReusesDelta = intValue(counters, "stale_frame_reuses_delta");
    stats.renderPoseFallbacksDelta = intValue(counters, "render_pose_fallbacks_delta");
    return stats;
}

bool RuntimeActivity::isStreaming() const
{
    return state == "streaming";
}

QString RuntimeActivity::stateDisplayName() const
{
    if (state != "streaming")
    {
        return "Idle";
    }
    if (transport == "wifi")
    {
        return "Streaming (WiFi)";
    }
    if (transport == "usb_adb")
    {
        return "Streaming (USB)";
    }
    return "Streaming";
}

QString RuntimeActivity::deviceDisplayName() const
{
    if (!isStreaming())
    {
        return "None";
    }
    if (deviceType == "quest")
    {
        return "Quest";
    }
    if (deviceType == "pico")
    {
        return "Pico";
    }
    if (deviceType == "simulator")
    {
        return "Simulator";
    }
    if (deviceType == "vision_pro")
    {
        return "Vision Pro";
    }
    return "Unknown";
}

RuntimeActivity RuntimeActivity::idle()
{
    return {};
}

RuntimeActivity RuntimeActivity::parse(const QByteArray& data, bool validateProcess)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
    {
        return idle();
    }

    const QJsonObject object = document.object();
    const qint64 processId = int64Value(object, "process_id");
    if (validateProcess && processId > 0 && !isProcessRunning(processId))
    {
        return idle();
    }

    RuntimeActivity activity;
    activity.state = nonEmptyString(object, "state");
    if (activity.state.isEmpty())
    {
        activity.state = "idle";
    }
    activity.transport = nonEmptyString(object, "transport");
    activity.deviceType = nonEmptyString(object, "device_type");
    activity.clientName = nonEmptyString(object, "client_name");
    activity.applicationName = nonEmptyString(object, "application_name");
    activity.processId = processId;
    activity.updatedAtUnixMilliseconds = int64Value(object, "updated_at_unix_ms");
    if (activity.state == "streaming" && object.value("streaming_stats").isObject())
    {
        activity.hasStreamingStats = true;
        activity.streamingStats = RuntimeStreamingStats::parse(
            object.value("streaming_stats").toObject());
    }
    return activity;
}

RuntimeActivity RuntimeActivity::readFromFile(const QString& path, bool validateProcess)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return idle();
    }
    return parse(file.readAll(), validateProcess);
}

QString dimensionsText(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return "Unknown";
    }
    return QString("%1 x %2").arg(width).arg(height);
}

QString millisecondsText(double value)
{
    if (value >= 100.0)
    {
        return QString("%1 ms").arg(value, 0, 'f', 0);
    }
    return QString("%1 ms").arg(value, 0, 'f', 1);
}
