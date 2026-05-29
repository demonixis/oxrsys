// SPDX-License-Identifier: MPL-2.0

#include "AdbBridge.h"
#include "Launcher.h"
#include "PlatformSupport.h"
#include "RuntimeActivity.h"
#include "RuntimeManager.h"
#include "ServerConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QTemporaryDir>

#include <stdexcept>

namespace
{

void expect(bool condition, const QString& message)
{
    if (!condition)
    {
        throw std::runtime_error(message.toStdString());
    }
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
           "Failed to open " + path);
    file.write(data);
}

void makeExecutable(const QString& path)
{
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                              QFile::ReadGroup | QFile::ExeGroup |
                              QFile::ReadOther | QFile::ExeOther);
}

void testServerConfigRoundTrip()
{
    const ServerConfig parsed = ServerConfig::parse(R"(
        [streaming]
        bitrate_mbps = 85
        transport = "usb_adb"
        resolution_scale = 0.50
        encoder_preset = "speed"

        [logging]
        quest_logcat = yes
    )");

    expect(parsed.bitrateMbps == 85, "Expected bitrate parse");
    expect(parsed.transport == "usb_adb", "Expected transport parse");
    expect(parsed.resolutionScale == 0.50, "Expected resolution parse");
    expect(parsed.encoderPreset == "speed", "Expected preset parse");
    expect(parsed.questLogcat, "Expected quest_logcat parse");

    const QString merged = parsed.mergedInto(ServerConfig::defaultText());
    expect(merged.contains("transport = \"usb_adb\""), "Expected transport serialization");
    expect(merged.contains("bitrate_mbps = 85"), "Expected bitrate serialization");
}

void testRuntimeManifestGeneration()
{
    const QString json = RuntimeManager::runtimeManifestJson("/tmp/OXRSys Runtime/liboxrsys-runtime.so");
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    const QJsonObject runtime = document.object().value("runtime").toObject();
    expect(document.object().value("file_format_version").toString() == "1.0.0",
           "Expected manifest format version");
    expect(runtime.value("library_path").toString().endsWith("liboxrsys-runtime.so"),
           "Expected runtime library path");
}

void testAdbParsing()
{
    const QList<AdbDevice> devices = AdbBridge::parseDevices(R"(
List of devices attached
1WMHH000000000 device usb:336592896X product:hollywood model:Quest_3
ABC unauthorized usb:1-1
)");
    expect(devices.size() == 2, "Expected two adb devices");
    expect(devices.first().serial == "1WMHH000000000", "Expected adb serial");
    expect(devices.first().isUsable(), "Expected authorized adb device");
    expect(!devices.last().isUsable(), "Expected unauthorized adb device");

    const QSet<int> ports = AdbBridge::parseReversePorts(R"(
UsbFfs tcp:9944 tcp:9944
UsbFfs tcp:9945 tcp:9945
UsbFfs tcp:9946 tcp:9946
)");
    QSet<int> expectedPorts;
    expectedPorts.insert(9944);
    expectedPorts.insert(9945);
    expectedPorts.insert(9946);
    expect(ports == expectedPorts, "Expected reverse ports");
}

void testRuntimeActivityParsing()
{
    const RuntimeActivity activity = RuntimeActivity::parse(R"({
      "state": "streaming",
      "transport": "usb_adb",
      "device_type": "quest",
      "client_name": "Quest",
      "application_name": "Unity",
      "process_id": 42,
      "streaming_stats": {
        "sample_unix_ms": 1800000001000,
        "refresh_rate_hz": 90,
        "current_bitrate_mbps": 42,
        "max_bitrate_mbps": 50,
        "render_width": 3664,
        "render_height": 1920,
        "encoded_width": 2752,
        "encoded_height": 1440,
        "latency_ms": {
          "server_pipeline": 12.5,
          "client_pipeline": 18.25,
          "prediction_horizon": 30.75
        },
        "encode_ms": {
          "total_p95": 9.5
        },
        "counters": {
          "encoder_dropped_frames_total": 2
        }
      }
    })", false);

    expect(activity.isStreaming(), "Expected streaming activity");
    expect(activity.stateDisplayName() == "Streaming (USB)", "Expected USB state display");
    expect(activity.deviceDisplayName() == "Quest", "Expected Quest device display");
    expect(activity.hasStreamingStats, "Expected stats");
    expect(activity.streamingStats.refreshRateHz == 90, "Expected refresh stats");
    expect(activity.streamingStats.encodeTotalP95Ms == 9.5, "Expected encode stats");
}

void testDesktopInspection()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString executablePath = QDir(temporaryDir.path()).filePath("Godot");
    writeFile(executablePath, "#!/bin/sh\n");
    makeExecutable(executablePath);

    const QString desktopPath = QDir(temporaryDir.path()).filePath("godot.desktop");
    writeFile(desktopPath,
              QString("[Desktop Entry]\n"
                      "Type=Application\n"
                      "Name=Godot Test\n"
                      "Exec=%1 %%F\n")
                  .arg(executablePath)
                  .toUtf8());

    const LauncherApp app = LauncherScanner::inspectPath(desktopPath, "automatic", false);
    expect(app.name == "Godot Test", "Expected desktop app name");
    expect(app.kind == "godot", "Expected Godot kind");
    expect(app.executablePath == executablePath, "Expected desktop executable path");
}

void testMacAppInspection()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString bundlePath = QDir(temporaryDir.path()).filePath("Unity.app");
    const QString contentsPath = QDir(bundlePath).filePath("Contents");
    const QString macOsPath = QDir(contentsPath).filePath("MacOS");
    QDir().mkpath(macOsPath);
    writeFile(QDir(contentsPath).filePath("Info.plist"), R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>Unity</string>
  <key>CFBundleName</key><string>Unity</string>
  <key>CFBundleIdentifier</key><string>com.unity3d.UnityEditor5.x</string>
</dict>
</plist>)");
    const QString executablePath = QDir(macOsPath).filePath("Unity");
    writeFile(executablePath, "#!/bin/sh\n");
    makeExecutable(executablePath);

    const LauncherApp app = LauncherScanner::inspectPath(bundlePath, "manual", false);
    expect(app.name == "Unity", "Expected app bundle name");
    expect(app.kind == "unity", "Expected Unity kind");
    expect(app.executablePath == executablePath, "Expected app bundle executable");
}

void testLauncherStore()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    LauncherStore store;
    store.manualApps.append({
        "Custom App",
        "/tmp/custom",
        "/tmp/custom",
        "custom",
        "manual",
        QDateTime::fromMSecsSinceEpoch(1800000000000, QTimeZone::UTC),
    });
    store.hiddenAutomaticAppPaths.append("/tmp/hidden");

    const QString storePath = QDir(temporaryDir.path()).filePath("launcher_apps.json");
    store.save(storePath);
    const LauncherStore loaded = LauncherStore::load(storePath);
    expect(loaded.manualApps.size() == 1, "Expected loaded manual app");
    expect(loaded.hiddenAutomaticAppPaths == QStringList({"/tmp/hidden"}),
           "Expected loaded hidden app");
}

void testShellSplitting()
{
    const QStringList tokens = splitCommandLine("'quoted app' --flag %F");
    expect(tokens.size() == 3, "Expected split token count");
    expect(tokens.first() == "quoted app", "Expected quoted token");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        testServerConfigRoundTrip();
        testRuntimeManifestGeneration();
        testAdbParsing();
        testRuntimeActivityParsing();
        testDesktopInspection();
        testMacAppInspection();
        testLauncherStore();
        testShellSplitting();
    }
    catch (const std::exception& error)
    {
        qCritical("Home core tests failed: %s", error.what());
        return 1;
    }

    qInfo("Home core tests passed");
    return 0;
}
