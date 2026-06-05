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
    int fovDegrees = 100;
    double resolutionScale = 0.75;
    int keyframeIntervalSec = 2;
    QString encoderPreset = "balanced";
    QString transport = "auto";
    bool fileLogging = true;
    bool questLogcat = false;

    static QString defaultText();
    static ServerConfig parse(const QString& text);
    QString mergedInto(const QString& currentText) const;
};

QString encoderPresetDisplayName(const QString& value);
QString transportDisplayName(const QString& value);
