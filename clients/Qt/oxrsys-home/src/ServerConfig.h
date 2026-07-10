// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <oxrsys/protocol/Protocol.h>

#include <QString>

struct ServerConfig
{
    static constexpr int MinBitrateMbps =
        static_cast<int>(oxr::protocol::STREAMING_MIN_BITRATE_MBPS);
    static constexpr int MaxBitrateMbps =
        static_cast<int>(oxr::protocol::STREAMING_MAX_BITRATE_MBPS);

    bool runtimeEnabled = true;
    int bitrateMbps = 50;
    int refreshRateHz = 72;
    double resolutionScale = 0.75;
    double dynamicResolutionMinScale = 0.50;
    QString renderDevice = "quest3";
    int keyframeIntervalSec = 2;
    QString videoCodec = "h265";
    QString encoderPreset = "balanced";
    bool encoder10Bit = false;
    QString transport = "auto";
    QString foveatedEncodingPreset = "off";
    QString clientFoveationPreset = "auto";
    bool clientUpscaling = false;
    double clientSharpening = 0.0;
    QString clientReprojection = "pose";
    QString abrMode = "bitrate";
    bool passthroughEnabled = false;
    bool appAlphaBlendPassthrough = false;
    QString occlusionMode = "off";
    bool headsetAudio = false;
    bool spatialEnabled = false;
    bool spatialAnchors = false;
    bool spatialScene = false;
    bool spatialPersistence = false;
    bool fileLogging = true;
    bool questLogcat = false;

    static QString defaultText();
    static ServerConfig parse(const QString& text);
    QString mergedInto(const QString& currentText) const;
};

QString encoderPresetDisplayName(const QString& value);
QString videoCodecDisplayName(const QString& value);
QString transportDisplayName(const QString& value);
QString foveationPresetDisplayName(const QString& value);
QString clientReprojectionDisplayName(const QString& value);
QString abrModeDisplayName(const QString& value);
QString occlusionModeDisplayName(const QString& value);
