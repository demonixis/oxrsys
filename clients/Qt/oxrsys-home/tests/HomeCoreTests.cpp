// SPDX-License-Identifier: MPL-2.0

#include "AdbBridge.h"
#include "HomeModel.h"
#include "Launcher.h"
#include "PlatformSupport.h"
#include "RuntimeActivity.h"
#include "RuntimeManager.h"
#include "ServerConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdio>
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

QByteArray readFile(const QString& path)
{
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "Failed to read " + path);
    return file.readAll();
}

void makeExecutable(const QString& path)
{
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                              QFile::ReadGroup | QFile::ExeGroup |
                              QFile::ReadOther | QFile::ExeOther);
}

bool containsCleanPath(const QStringList& paths, const QString& expected)
{
    const QString cleanExpected = QDir::cleanPath(expected);
    for (const QString& path : paths)
    {
        if (QDir::cleanPath(path) == cleanExpected)
        {
            return true;
        }
    }
    return false;
}

HomePaths testHomePaths(const QString& root)
{
    HomePaths paths;
    paths.configRoot = QDir(root).filePath("config");
    paths.dataRoot = QDir(root).filePath("data");
    paths.stateRoot = QDir(root).filePath("state");
    paths.activeRuntimeDirectory = QDir(root).filePath("openxr/1");
    paths.activeRuntimePath = QDir(paths.activeRuntimeDirectory).filePath("active_runtime.json");
    paths.configFilePath = QDir(paths.configRoot).filePath("oxrsys-runtime.toml");
    paths.launcherAppsPath = QDir(paths.configRoot).filePath("launcher_apps.json");
    paths.runtimeStatusPath = QDir(paths.stateRoot).filePath("runtime_status.json");
    return paths;
}

void testServerConfigRoundTrip()
{
    const ServerConfig parsed = ServerConfig::parse(R"(
        [streaming]
        bitrate_mbps = 85
        transport = "usb_adb"
        resolution_scale = 0.50
        refresh_rate_hz = 120
        encoder_preset = "speed"
        foveated_encoding_preset = "medium"
        client_foveation_preset = "high"
        client_upscaling = true
        headset_audio = true

        [logging]
        quest_logcat = yes
    )");

    expect(parsed.bitrateMbps == 85, "Expected bitrate parse");
    expect(parsed.transport == "usb_adb", "Expected transport parse");
    expect(parsed.resolutionScale == 0.50, "Expected resolution parse");
    expect(parsed.refreshRateHz == 120, "Expected refresh parse");
    expect(parsed.encoderPreset == "speed", "Expected preset parse");
    expect(parsed.foveatedEncodingPreset == "medium", "Expected FFE parse");
    expect(parsed.clientFoveationPreset == "high", "Expected client foveation parse");
    expect(parsed.clientUpscaling, "Expected upscaling parse");
    expect(parsed.headsetAudio, "Expected audio parse");
    expect(parsed.questLogcat, "Expected quest_logcat parse");

    const QString merged = parsed.mergedInto(ServerConfig::defaultText());
    expect(merged.contains("transport = \"usb_adb\""), "Expected transport serialization");
    expect(merged.contains("bitrate_mbps = 85"), "Expected bitrate serialization");
    expect(merged.contains("refresh_rate_hz = 120"), "Expected refresh serialization");
    expect(merged.contains("foveated_encoding_preset = \"medium\""), "Expected FFE serialization");
    expect(merged.contains("client_foveation_preset = \"high\""), "Expected FFR serialization");
    expect(merged.contains("client_upscaling = true"), "Expected upscaling serialization");
    expect(merged.contains("headset_audio = true"), "Expected audio serialization");
}

void testHomeModelResetsStreamingConfigToDefaults()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    HomePaths paths = testHomePaths(temporaryDir.path());

    writeFile(paths.configFilePath, R"(
[general]
runtime_enabled = false

[streaming]
bitrate_mbps = 85
transport = "usb_adb"
encoder_preset = "speed"

[logging]
file_logging = false
quest_logcat = true
)");

    const QString organization = "OXRSysHomeTests";
    const QString application =
        "HomeQt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings settings(organization, application);
    settings.clear();

    HomeModel model(nullptr, organization, application, paths, false);
    expect(model.serverConfig().bitrateMbps == 85,
           "Expected custom bitrate before default reset");
    expect(model.serverConfig().transport == "usb_adb",
           "Expected custom transport before default reset");

    model.resetStreamingConfigToDefaults();
    expect(model.serverConfig().runtimeEnabled, "Expected default runtime enabled");
    expect(model.serverConfig().bitrateMbps == 50, "Expected default bitrate");
    expect(model.serverConfig().transport == "auto", "Expected default transport");
    expect(model.serverConfig().fileLogging, "Expected default file logging");
    expect(!model.serverConfig().questLogcat, "Expected default quest logcat off");

    QFile file(paths.configFilePath);
    expect(file.open(QIODevice::ReadOnly | QIODevice::Text),
           "Expected default reset to write config");
    const QString text = QString::fromUtf8(file.readAll());
    expect(text.contains("bitrate_mbps = 50"), "Expected default bitrate serialization");
    expect(text.contains("transport = \"auto\""), "Expected default transport serialization");
    expect(text.contains("refresh_rate_hz = 72"), "Expected default refresh serialization");
    expect(text.contains("foveated_encoding_preset = \"off\""), "Expected default FFE serialization");
    expect(text.contains("client_foveation_preset = \"medium\""), "Expected default FFR serialization");
    expect(text.contains("client_upscaling = false"), "Expected default upscaling serialization");
    expect(text.contains("headset_audio = false"), "Expected default audio serialization");

    settings.clear();
}

void testRuntimeRegistration()
{
#if defined(Q_OS_LINUX)
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString runtimeLibraryPath = QDir(temporaryDir.path()).filePath("liboxrsys-runtime.so");
    const QString manifestPath = QDir(temporaryDir.path()).filePath("oxrsys-runtime.json");
    writeFile(runtimeLibraryPath, "runtime");
    writeFile(manifestPath, "{}");

    HomePaths paths;
    paths.activeRuntimeDirectory = QDir(temporaryDir.path()).filePath("openxr/1");
    paths.activeRuntimePath = QDir(paths.activeRuntimeDirectory).filePath("active_runtime.json");

    RuntimeManager manager(paths);
    QString error;
    expect(manager.registerRuntimeManifest(manifestPath, &error),
           "Expected runtime registration: " + error);

    const RuntimeRegistrationStatus status = manager.registrationStatus();
    expect(status.activeRuntimeExists, "Expected active runtime registration");
    expect(QFileInfo(paths.activeRuntimePath).exists() ||
               !QFileInfo(paths.activeRuntimePath).symLinkTarget().isEmpty(),
           "Expected active runtime file");

    const QString linkTarget = QFileInfo(paths.activeRuntimePath).symLinkTarget();
    if (!linkTarget.isEmpty())
    {
        expect(normalizedPath(linkTarget) == normalizedPath(manifestPath),
               "Expected active runtime symlink target");
        expect(normalizedPath(status.activeRuntimeTarget) == normalizedPath(manifestPath),
               "Expected registration status target");
    }

    expect(manager.unregisterRuntime(&error), "Expected runtime unregistration: " + error);
    expect(!manager.registrationStatus().activeRuntimeExists,
           "Expected active runtime registration to be removed");
#endif
}

void testRuntimeLaunchManifestPreference()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString selectedLibraryPath =
        QDir(temporaryDir.path()).filePath("selected/" + runtimeLibraryFileName());
    const QString selectedManifestPath = QDir(temporaryDir.path()).filePath("selected/oxrsys-runtime.json");
    const QString installedLibraryPath =
        QDir(temporaryDir.path()).filePath("installed/" + runtimeLibraryFileName());
    const QString installedManifestPath = QDir(temporaryDir.path()).filePath("installed/oxrsys-runtime.json");

    writeFile(selectedLibraryPath, "selected");
    writeFile(selectedManifestPath, RuntimeManager::runtimeManifestJson(selectedLibraryPath).toUtf8());
    writeFile(installedLibraryPath, "installed");
    writeFile(installedManifestPath, RuntimeManager::runtimeManifestJson(installedLibraryPath).toUtf8());

    HomePaths paths;
    paths.installedRuntimeDirectory = QFileInfo(installedLibraryPath).absolutePath();
    paths.installedRuntimeManifestPath = installedManifestPath;

    RuntimeManager manager(paths);
    expect(normalizedPath(manager.activeLaunchRuntimeManifestPath(selectedManifestPath, false)) ==
               normalizedPath(selectedManifestPath),
           "Expected selected manifest when installed runtime is not preferred");
    expect(normalizedPath(manager.activeLaunchRuntimeManifestPath(selectedManifestPath, true)) ==
               normalizedPath(installedManifestPath),
           "Expected installed manifest when installed runtime is preferred");
}

void testWindowsRuntimeInstallCopiesCompanionDlls()
{
#if defined(Q_OS_WIN)
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString sourceDirectory = QDir(temporaryDir.path()).filePath("bundled-runtime");
    const QString installedDirectory = QDir(temporaryDir.path()).filePath("installed-runtime");
    const QString configPath = QDir(temporaryDir.path()).filePath("config/oxrsys-runtime.toml");
    const QString installedManifestPath = QDir(installedDirectory).filePath("oxrsys-runtime.json");

    writeFile(QDir(sourceDirectory).filePath(runtimeLibraryFileName()), "runtime-v1");
    writeFile(QDir(sourceDirectory).filePath("oxrsys-runtime.toml"), "transport = \"auto\"\n");
    writeFile(QDir(sourceDirectory).filePath("avcodec-62.dll"), "avcodec-v1");
    writeFile(QDir(sourceDirectory).filePath("avutil-60.dll"), "avutil-v1");
    writeFile(QDir(sourceDirectory).filePath("swscale-9.dll"), "swscale-v1");

    HomePaths paths;
    paths.configFilePath = configPath;
    paths.installedRuntimeDirectory = installedDirectory;
    paths.installedRuntimeManifestPath = installedManifestPath;

    RuntimeManager manager(paths, sourceDirectory);
    QString installedManifest;
    QString error;
    expect(manager.installBundledRuntime(&installedManifest, &error),
           "Expected runtime install to succeed: " + error);
    expect(installedManifest == installedManifestPath, "Expected installed manifest path");

    expect(readFile(QDir(installedDirectory).filePath(runtimeLibraryFileName())) == "runtime-v1",
           "Expected runtime library copy");
    expect(readFile(QDir(installedDirectory).filePath("avcodec-62.dll")) == "avcodec-v1",
           "Expected avcodec companion DLL copy");
    expect(readFile(QDir(installedDirectory).filePath("avutil-60.dll")) == "avutil-v1",
           "Expected avutil companion DLL copy");
    expect(readFile(QDir(installedDirectory).filePath("swscale-9.dll")) == "swscale-v1",
           "Expected swscale companion DLL copy");
    expect(!manager.installStatus().installedRuntimeNeedsUpdate,
           "Expected freshly installed runtime to be current");

    QFile::remove(QDir(installedDirectory).filePath("avutil-60.dll"));
    expect(manager.installStatus().installedRuntimeNeedsUpdate,
           "Expected missing companion DLL to require update");

    writeFile(QDir(installedDirectory).filePath("avutil-60.dll"), "avutil-v0");
    expect(manager.installStatus().installedRuntimeNeedsUpdate,
           "Expected changed companion DLL to require update");
#endif
}

void testWindowsRuntimeInstallAcceptsStaticRuntimeWithoutCompanionDlls()
{
#if defined(Q_OS_WIN)
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString sourceDirectory = QDir(temporaryDir.path()).filePath("bundled-runtime");
    const QString installedDirectory = QDir(temporaryDir.path()).filePath("installed-runtime");
    const QString configPath = QDir(temporaryDir.path()).filePath("config/oxrsys-runtime.toml");
    const QString installedManifestPath = QDir(installedDirectory).filePath("oxrsys-runtime.json");

    writeFile(QDir(sourceDirectory).filePath(runtimeLibraryFileName()), "runtime-static-v1");
    writeFile(QDir(sourceDirectory).filePath("oxrsys-runtime.toml"), "transport = \"auto\"\n");

    HomePaths paths;
    paths.configFilePath = configPath;
    paths.installedRuntimeDirectory = installedDirectory;
    paths.installedRuntimeManifestPath = installedManifestPath;

    RuntimeManager manager(paths, sourceDirectory);
    QString installedManifest;
    QString error;
    expect(manager.installBundledRuntime(&installedManifest, &error),
           "Expected static runtime install to succeed: " + error);
    expect(installedManifest == installedManifestPath, "Expected installed manifest path");

    expect(readFile(QDir(installedDirectory).filePath(runtimeLibraryFileName())) ==
               "runtime-static-v1",
           "Expected static runtime library copy");
    expect(!QFileInfo(QDir(installedDirectory).filePath("avcodec-62.dll")).exists(),
           "Expected no FFmpeg companion DLL for static runtime");
    expect(!manager.installStatus().installedRuntimeNeedsUpdate,
           "Expected static runtime without companion DLLs to be current");
#endif
}

void testRuntimeBuildDirectoryCandidates()
{
    const QString appDirRuntime =
        QDir(QCoreApplication::applicationDirPath()).filePath("../../../runtime");
    expect(containsCleanPath(runtimeBuildDirectoryCandidates(), appDirRuntime),
           "Expected top-level build runtime candidate");
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

void testQuestUsbReadinessKeepsPreviouslyVerifiedReverse()
{
    QSet<int> completePorts;
    for (int port : AdbBridge::reversePorts())
    {
        completePorts.insert(port);
    }

    const QString serial = "1WMHH000000000";
    const AdbStatus unavailableAdb;
    const TransportReadiness unavailableReadiness =
        questUsbTransportReadiness(unavailableAdb, {}, serial, completePorts);
    expect(unavailableReadiness.isReady,
           "Expected previously verified reverse to stay ready when adb is unavailable");
    expect(!unavailableReadiness.canConfigureUsb,
           "Expected unavailable adb not to advertise configure action");
    expect(unavailableReadiness.message.contains("previously verified"),
           "Expected previously verified readiness message");

    const AdbStatus availableAdb = {"adb", "ADB found."};
    const TransportReadiness hiddenDeviceReadiness =
        questUsbTransportReadiness(availableAdb, {}, serial, completePorts);
    expect(hiddenDeviceReadiness.isReady,
           "Expected previously verified reverse to stay ready when adb no longer reports the device");

    const QList<AdbDevice> visibleDevices = {{serial, "device", "model:Quest_3"}};
    const QSet<int> partialPorts = {9944};
    const TransportReadiness missingPortsReadiness =
        questUsbTransportReadiness(availableAdb, visibleDevices, serial, partialPorts);
    expect(!missingPortsReadiness.isReady, "Expected incomplete reverse ports not to be ready");
    expect(missingPortsReadiness.canConfigureUsb,
           "Expected visible device with missing ports to allow USB configure");
}

void testAdbCustomPathSelection()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

#if !defined(Q_OS_WIN)
    const QString customAdbPath = QDir(temporaryDir.path()).filePath("adb");
    writeFile(customAdbPath,
              "#!/bin/sh\n"
              "if [ \"$1\" = \"version\" ]; then\n"
              "  echo \"Android Debug Bridge version 1.0.41\"\n"
              "  exit 0\n"
              "fi\n"
              "exit 0\n");
    makeExecutable(customAdbPath);

    const QStringList candidates = AdbBridge::candidatePaths(customAdbPath);
    expect(candidates.first() == customAdbPath,
           "Expected custom adb path to be the first candidate");
    expect(AdbBridge::resolveExecutablePath(customAdbPath) ==
               QFileInfo(customAdbPath).absoluteFilePath(),
           "Expected valid custom adb path to be selected");
    expect(AdbBridge::status(customAdbPath).isAvailable(),
           "Expected valid custom adb status");
#endif

    const QString invalidPath = QDir(temporaryDir.path()).filePath("invalid-adb");
    writeFile(invalidPath, "not executable");
    const AdbStatus invalidStatus = AdbBridge::status(invalidPath);
    expect(!invalidStatus.isAvailable(), "Expected invalid custom adb to be unavailable");
    expect(invalidStatus.message.contains("Custom ADB path is invalid"),
           "Expected invalid custom adb status message");
    expect(AdbBridge::resolveExecutablePath(invalidPath).isEmpty(),
           "Expected invalid custom adb to block automatic fallback");
}

void testCustomAdbPreferencePersistence()
{
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");
    const HomePaths paths = testHomePaths(temporaryDir.path());

    const QString organization = "OXRSysHomeTests";
    const QString application =
        "HomeQt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings settings(organization, application);
    settings.clear();

    {
        HomeModel model(nullptr, organization, application, paths, false);
        model.setCustomAdbPath("/tmp/custom-adb");
        expect(model.customAdbPath() == "/tmp/custom-adb",
               "Expected custom adb path to update in model");
    }
    {
        HomeModel model(nullptr, organization, application, paths, false);
        expect(model.customAdbPath() == "/tmp/custom-adb",
               "Expected custom adb path to persist in settings");
        model.clearCustomAdbPath();
        expect(model.customAdbPath().isEmpty(),
               "Expected custom adb path to clear in model");
    }
    {
        HomeModel model(nullptr, organization, application, paths, false);
        expect(model.customAdbPath().isEmpty(),
               "Expected cleared custom adb path to persist");
    }

    settings.clear();
}

void testHomeModelTransportRefreshIsAsyncWithSlowAdb()
{
#if !defined(Q_OS_WIN)
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString slowAdbPath = QDir(temporaryDir.path()).filePath("adb");
    writeFile(slowAdbPath,
              "#!/bin/sh\n"
              "sleep 1\n"
              "if [ \"$1\" = \"version\" ]; then\n"
              "  echo \"Android Debug Bridge version 1.0.41\"\n"
              "  exit 0\n"
              "fi\n"
              "if [ \"$1\" = \"devices\" ]; then\n"
              "  echo \"List of devices attached\"\n"
              "  exit 0\n"
              "fi\n"
              "exit 0\n");
    makeExecutable(slowAdbPath);

    HomePaths paths = testHomePaths(temporaryDir.path());

    const QString organization = "OXRSysHomeTests";
    const QString application =
        "HomeQt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings settings(organization, application);
    settings.clear();
    settings.setValue("customAdbPath", slowAdbPath);

    QElapsedTimer timer;
    timer.start();
    HomeModel model(nullptr, organization, application, paths);
    expect(timer.elapsed() < 250, "Expected HomeModel construction to avoid slow adb blocking");

    timer.restart();
    model.refreshQuestUsbDevices();
    expect(timer.elapsed() < 100, "Expected USB refresh to return before slow adb completes");

    settings.clear();
#endif
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
        "encoder_preset": "quality",
        "foveated_encoding_preset": "medium",
        "client_foveation_preset": "high",
        "client_upscaling": true,
        "headset_audio": false,
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
    expect(activity.streamingStats.encoderPreset == "quality", "Expected encoder preset stats");
    expect(activity.streamingStats.foveatedEncodingPreset == "medium", "Expected FFE stats");
    expect(activity.streamingStats.clientFoveationPreset == "high", "Expected FFR stats");
    expect(activity.streamingStats.clientUpscaling, "Expected upscaling stats");
    expect(!activity.streamingStats.headsetAudio, "Expected audio stats");
    expect(activity.streamingStats.encodeTotalP95Ms == 9.5, "Expected encode stats");
}

void testDesktopInspection()
{
#if defined(Q_OS_WIN)
    return;
#else
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
#endif
}

void testMacAppInspection()
{
#if defined(Q_OS_WIN)
    return;
#else
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
#endif
}

void testWindowsExecutableInspection()
{
#if defined(Q_OS_WIN)
    QTemporaryDir temporaryDir;
    expect(temporaryDir.isValid(), "Expected temporary directory");

    const QString executablePath = QDir(temporaryDir.path()).filePath("Godot.exe");
    writeFile(executablePath, "MZ");
    makeExecutable(executablePath);

    const LauncherApp app = LauncherScanner::inspectPath(executablePath, "manual", false);
    expect(app.name == "Godot", "Expected Windows executable app name");
    expect(app.kind == "godot", "Expected Windows executable kind");
    expect(app.executablePath == QFileInfo(executablePath).absoluteFilePath(),
           "Expected Windows executable path");
#endif
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

void testTerminalLaunchScript()
{
    LauncherApp app;
    app.name = "Godot XR!";
    app.path = "/tmp/Godot XR";
    app.executablePath = "/tmp/Godot XR/bin/godot";

    expect(terminalSafeName(app.name) == "Godot_XR", "Expected terminal-safe app name");

    const QString script = terminalLaunchScript(app, "/tmp/runtime/oxrsys-runtime.json");
    expect(script.startsWith("#!/bin/sh\n"), "Expected POSIX terminal script");
    expect(script.contains("cd "), "Expected terminal script working directory");
    expect(script.contains("export XR_RUNTIME_JSON="), "Expected runtime manifest export");
    expect(script.contains("oxrsys-runtime.json"), "Expected manifest path in terminal script");
    expect(script.contains("godot"), "Expected executable in terminal script");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        testServerConfigRoundTrip();
        testHomeModelResetsStreamingConfigToDefaults();
        testRuntimeRegistration();
        testRuntimeLaunchManifestPreference();
        testWindowsRuntimeInstallCopiesCompanionDlls();
        testWindowsRuntimeInstallAcceptsStaticRuntimeWithoutCompanionDlls();
        testRuntimeBuildDirectoryCandidates();
        testAdbParsing();
        testQuestUsbReadinessKeepsPreviouslyVerifiedReverse();
        testAdbCustomPathSelection();
        testCustomAdbPreferencePersistence();
        testHomeModelTransportRefreshIsAsyncWithSlowAdb();
        testRuntimeActivityParsing();
        testDesktopInspection();
        testMacAppInspection();
        testWindowsExecutableInspection();
        testLauncherStore();
        testShellSplitting();
        testTerminalLaunchScript();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Home core tests failed: %s\n", error.what());
        qCritical("Home core tests failed: %s", error.what());
        return 1;
    }

    std::fprintf(stderr, "Home core tests passed\n");
    qInfo("Home core tests passed");
    return 0;
}
