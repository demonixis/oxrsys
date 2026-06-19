// SPDX-License-Identifier: MPL-2.0

#include "ServerConfig.h"

#include "PlatformSupport.h"

#include <QRegularExpression>
#include <QList>
#include <QPair>
#include <QStringList>

namespace
{

QString rawValue(const QString& key, const QString& text)
{
    const QRegularExpression regex(
        QStringLiteral("(?m)^\\s*%1\\s*=\\s*(.+?)\\s*$")
            .arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch match = regex.match(text);
    if (!match.hasMatch())
    {
        return {};
    }
    return match.captured(1).trimmed();
}

bool boolValue(const QString& key, const QString& text, bool* ok)
{
    const QString value = rawValue(key, text).toLower();
    if (value == "true" || value == "1" || value == "yes")
    {
        *ok = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no")
    {
        *ok = true;
        return false;
    }
    *ok = false;
    return false;
}

bool isSupportedRefreshRate(int value)
{
    return value == 60 || value == 72 || value == 80 || value == 90 || value == 120;
}

bool isFoveationPreset(const QString& value)
{
    return value == "off" || value == "light" || value == "medium" || value == "high";
}

bool isClientFoveationPreset(const QString& value)
{
    return value == "auto" || isFoveationPreset(value);
}

bool isClientReprojection(const QString& value)
{
    return value == "off" || value == "pose" || value == "pose_warp";
}

bool isAbrMode(const QString& value)
{
    return value == "off" || value == "bitrate" || value == "full";
}

QString stringValue(const QString& key, const QString& text)
{
    QString value = rawValue(key, text);
    if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
    {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

QString boolString(bool value)
{
    return value ? "true" : "false";
}

QString decimalString(double value)
{
    return QString::number(value, 'f', 2);
}

bool isSectionHeader(const QString& line)
{
    const QString trimmed = line.trimmed();
    return trimmed.startsWith('[') && trimmed.endsWith(']');
}

bool matchesKey(const QString& key, const QString& line)
{
    const QString trimmed = line.trimmed();
    const int equalsIndex = trimmed.indexOf('=');
    if (equalsIndex < 0)
    {
        return false;
    }
    return trimmed.left(equalsIndex).trimmed() == key;
}

QString upsertSection(QString text,
                      const QString& sectionName,
                      const QList<QPair<QString, QString>>& keys)
{
    QStringList lines = text.split('\n');
    const QString sectionHeader = QString("[%1]").arg(sectionName);

    auto nextSectionStart = [&lines](qsizetype index) -> qsizetype {
        for (qsizetype i = index + 1; i < lines.size(); ++i)
        {
            if (isSectionHeader(lines.at(i)))
            {
                return i;
            }
        }
        return lines.size();
    };

    int sectionIndex = -1;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (lines.at(i).trimmed() == sectionHeader)
        {
            sectionIndex = i;
            break;
        }
    }

    if (sectionIndex >= 0)
    {
        int sectionEnd = nextSectionStart(sectionIndex);
        int insertIndex = sectionEnd;
        for (const auto& item : keys)
        {
            const QString newLine = QString("%1 = %2").arg(item.first, item.second);
            int existingIndex = -1;
            for (int i = sectionIndex + 1; i < sectionEnd; ++i)
            {
                if (matchesKey(item.first, lines.at(i)))
                {
                    existingIndex = i;
                    break;
                }
            }
            if (existingIndex >= 0)
            {
                lines[existingIndex] = newLine;
            }
            else
            {
                lines.insert(insertIndex, newLine);
                ++insertIndex;
                ++sectionEnd;
            }
        }
    }
    else
    {
        if (!lines.isEmpty() && !lines.last().isEmpty())
        {
            lines.append(QString());
        }
        lines.append(sectionHeader);
        for (const auto& item : keys)
        {
            lines.append(QString("%1 = %2").arg(item.first, item.second));
        }
    }

    return lines.join('\n');
}

QString removeSectionKeys(QString text, const QString& sectionName, const QStringList& keys)
{
    QStringList lines = text.split('\n');
    const QString sectionHeader = QString("[%1]").arg(sectionName);

    int sectionIndex = -1;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (lines.at(i).trimmed() == sectionHeader)
        {
            sectionIndex = i;
            break;
        }
    }
    if (sectionIndex < 0)
    {
        return text;
    }

    int sectionEnd = lines.size();
    for (int i = sectionIndex + 1; i < lines.size(); ++i)
    {
        if (isSectionHeader(lines.at(i)))
        {
            sectionEnd = i;
            break;
        }
    }

    for (int i = sectionEnd - 1; i > sectionIndex; --i)
    {
        for (const QString& key : keys)
        {
            if (matchesKey(key, lines.at(i)))
            {
                lines.removeAt(i);
                if (i - 1 > sectionIndex && lines.at(i - 1).contains("Rendering FOV"))
                {
                    lines.removeAt(i - 1);
                }
                break;
            }
        }
    }
    return lines.join('\n');
}

} // namespace

QString ServerConfig::defaultText()
{
    return QStringLiteral(
        "# OXRSys Runtime Configuration\n"
        "# Preferred location:\n"
        "#   Linux: ${XDG_CONFIG_HOME:-~/.config}/oxrsys/oxrsys-runtime.toml\n"
        "#   macOS: ~/Library/Application Support/OXRSys/oxrsys-runtime.toml\n"
        "#   Windows: %%APPDATA%%/OXRSys/oxrsys-runtime.toml\n"
        "\n"
        "[general]\n"
        "runtime_enabled = true\n"
        "\n"
        "[streaming]\n"
        "bitrate_mbps = 50\n"
        "refresh_rate_hz = 72\n"
        "resolution_scale = 0.75\n"
        "keyframe_interval_sec = 2\n"
        "encoder_preset = \"balanced\"\n"
        "transport = \"auto\"\n"
        "foveated_encoding_preset = \"off\"\n"
        "client_foveation_preset = \"auto\"\n"
        "client_upscaling = false\n"
        "client_reprojection = \"pose\"\n"
        "abr_mode = \"bitrate\"\n"
        "headset_audio = false\n"
        "\n"
        "[logging]\n"
        "file_logging = true\n"
        "quest_logcat = false\n");
}

ServerConfig ServerConfig::parse(const QString& text)
{
    ServerConfig config;
    bool ok = false;

    const bool runtimeEnabled = boolValue("runtime_enabled", text, &ok);
    if (ok)
    {
        config.runtimeEnabled = runtimeEnabled;
    }

    const int bitrate = rawValue("bitrate_mbps", text).toInt(&ok);
    if (ok && bitrate >= ServerConfig::MinBitrateMbps &&
        bitrate <= ServerConfig::MaxBitrateMbps)
    {
        config.bitrateMbps = bitrate;
    }

    const int refreshRate = rawValue("refresh_rate_hz", text).toInt(&ok);
    if (ok && isSupportedRefreshRate(refreshRate))
    {
        config.refreshRateHz = refreshRate;
    }

    const double resolutionScale = rawValue("resolution_scale", text).toDouble(&ok);
    if (ok && resolutionScale >= 0.25 && resolutionScale <= 1.0)
    {
        config.resolutionScale = resolutionScale;
    }

    const int keyframeInterval = rawValue("keyframe_interval_sec", text).toInt(&ok);
    if (ok && keyframeInterval >= 1 && keyframeInterval <= 10)
    {
        config.keyframeIntervalSec = keyframeInterval;
    }

    const QString preset = stringValue("encoder_preset", text);
    if (preset == "quality" || preset == "balanced" || preset == "speed")
    {
        config.encoderPreset = preset;
    }

    const QString transportValue = stringValue("transport", text);
    if (transportValue == "auto" || transportValue == "wifi" || transportValue == "usb_adb")
    {
        config.transport = transportValue;
    }

    const QString foveatedEncodingPreset = stringValue("foveated_encoding_preset", text);
    if (isFoveationPreset(foveatedEncodingPreset))
    {
        config.foveatedEncodingPreset = foveatedEncodingPreset;
    }

    const QString clientFoveationPreset = stringValue("client_foveation_preset", text);
    if (isClientFoveationPreset(clientFoveationPreset))
    {
        config.clientFoveationPreset = clientFoveationPreset;
    }

    const bool clientUpscaling = boolValue("client_upscaling", text, &ok);
    if (ok)
    {
        config.clientUpscaling = clientUpscaling;
    }

    const QString clientReprojection = stringValue("client_reprojection", text);
    if (isClientReprojection(clientReprojection))
    {
        config.clientReprojection = clientReprojection;
    }

    const QString abrMode = stringValue("abr_mode", text);
    if (isAbrMode(abrMode))
    {
        config.abrMode = abrMode;
    }

    const bool headsetAudio = boolValue("headset_audio", text, &ok);
    if (ok)
    {
        config.headsetAudio = headsetAudio;
    }

    const bool fileLogging = boolValue("file_logging", text, &ok);
    if (ok)
    {
        config.fileLogging = fileLogging;
    }

    const bool questLogcat = boolValue("quest_logcat", text, &ok);
    if (ok)
    {
        config.questLogcat = questLogcat;
    }

    return config;
}

QString ServerConfig::mergedInto(const QString& currentText) const
{
    QString text = currentText.trimmed();
    if (text.isEmpty())
    {
        text = defaultText().trimmed();
    }
    text = removeSectionKeys(text, "streaming", QStringList{QStringLiteral("fov_degrees")});

    text = upsertSection(text, "general", {
        {"runtime_enabled", boolString(runtimeEnabled)},
    });
    text = upsertSection(text, "streaming", {
        {"bitrate_mbps", QString::number(bitrateMbps)},
        {"refresh_rate_hz", QString::number(refreshRateHz)},
        {"resolution_scale", decimalString(resolutionScale)},
        {"keyframe_interval_sec", QString::number(keyframeIntervalSec)},
        {"encoder_preset", QString("\"%1\"").arg(encoderPreset)},
        {"transport", QString("\"%1\"").arg(transport)},
        {"foveated_encoding_preset", QString("\"%1\"").arg(foveatedEncodingPreset)},
        {"client_foveation_preset", QString("\"%1\"").arg(clientFoveationPreset)},
        {"client_upscaling", boolString(clientUpscaling)},
        {"client_reprojection", QString("\"%1\"").arg(clientReprojection)},
        {"abr_mode", QString("\"%1\"").arg(abrMode)},
        {"headset_audio", boolString(headsetAudio)},
    });
    text = upsertSection(text, "logging", {
        {"file_logging", boolString(fileLogging)},
        {"quest_logcat", boolString(questLogcat)},
    });

    return text.endsWith('\n') ? text : text + '\n';
}

QString encoderPresetDisplayName(const QString& value)
{
    if (value == "quality")
    {
        return "Quality";
    }
    if (value == "speed")
    {
        return "Speed";
    }
    return "Balanced";
}

QString transportDisplayName(const QString& value)
{
    if (value == "wifi")
    {
        return "WiFi";
    }
    if (value == "usb_adb")
    {
        return "USB ADB";
    }
    return "Auto";
}

QString foveationPresetDisplayName(const QString& value)
{
    if (value == "auto")
    {
        return "Auto";
    }
    if (value == "light")
    {
        return "Light";
    }
    if (value == "medium")
    {
        return "Medium";
    }
    if (value == "high")
    {
        return "High";
    }
    return "Off";
}

QString clientReprojectionDisplayName(const QString& value)
{
    if (value == "off")
    {
        return "Off";
    }
    if (value == "pose_warp")
    {
        return "Pose Warp";
    }
    return "Pose";
}

QString abrModeDisplayName(const QString& value)
{
    if (value == "off")
    {
        return "Off";
    }
    if (value == "full")
    {
        return "Full";
    }
    return "Bitrate";
}
