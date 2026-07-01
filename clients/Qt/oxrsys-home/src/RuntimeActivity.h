// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

struct RuntimeStreamingStats
{
    qint64 sampleUnixMilliseconds = 0;
    int refreshRateHz = 0;
    int currentBitrateMbps = 0;
    int maxBitrateMbps = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    int encodedWidth = 0;
    int encodedHeight = 0;
    QString videoCodec;
    QString encoderPreset;
    QString foveatedEncodingPreset;
    QString clientFoveationPreset;
    bool clientUpscaling = false;
    QString clientReprojectionMode;
    QString abrMode;
    QString abrState;
    QString abrProfile;
    double resolutionScale = 0.0;
    double dynamicResolutionMinScale = 0.0;
    bool streamReconfigure = false;
    int streamConfigSequence = 0;
    bool passthroughEnabled = false;
    bool passthroughSupported = false;
    bool passthroughReady = false;
    QString occlusionMode;
    bool spatialEnabled = false;
    bool headsetAudio = false;

    double serverPipelineMs = 0.0;
    double clientPipelineMs = 0.0;
    double clientReceiveToSubmitMs = 0.0;
    double clientDecodeMs = 0.0;
    double clientCompositorMs = 0.0;
    double predictionHorizonMs = 0.0;
    double displayedFrameAgeMs = 0.0;

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

    int encodedFramesTotal = 0;
    int encoderDroppedFramesTotal = 0;
    int replacedFramesDelta = 0;
    int keyframeRequestsDelta = 0;
    int pendingDepthMax = 0;
    int reprojectedFramesDelta = 0;
    int staleFrameReusesDelta = 0;
    int renderPoseFallbacksDelta = 0;

    static RuntimeStreamingStats parse(const QJsonObject& object);
};

struct RuntimeActivity
{
    QString state = "idle";
    QString transport;
    QString deviceType;
    QString clientName;
    QString applicationName;
    qint64 processId = 0;
    qint64 updatedAtUnixMilliseconds = 0;
    bool hasStreamingStats = false;
    RuntimeStreamingStats streamingStats;

    bool isStreaming() const;
    QString stateDisplayName() const;
    QString deviceDisplayName() const;

    static RuntimeActivity idle();
    static RuntimeActivity parse(const QByteArray& data, bool validateProcess);
    static RuntimeActivity readFromFile(const QString& path, bool validateProcess);
};

QString dimensionsText(int width, int height);
QString millisecondsText(double value);
