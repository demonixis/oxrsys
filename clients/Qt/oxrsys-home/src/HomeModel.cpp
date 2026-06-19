// SPDX-License-Identifier: MPL-2.0

#include "HomeModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QThread>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace
{

constexpr int MaxLogCharacters = 30000;
constexpr int MaxRuntimeStatsSamples = 60;

bool shouldDropApplicationLogLine(const QString& line)
{
    static const QStringList ignoredSystemNoiseMarkers = {
        QStringLiteral("com.apple.linkd.autoShortcut"),
        QStringLiteral("Error registering app with intents framework"),
    };
    static const QSet<QString> ignoredSystemNoiseLines = {
        QStringLiteral("Will NOT re-try to establish the connection"),
    };

    if (ignoredSystemNoiseLines.contains(line.trimmed()))
    {
        return true;
    }

    for (const QString& marker : ignoredSystemNoiseMarkers)
    {
        if (line.contains(marker))
        {
            return true;
        }
    }
    return false;
}

QDateTime modificationDate(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? info.lastModified() : QDateTime();
}

QString runtimeStatsIdentity(const RuntimeActivity& activity)
{
    return QString("%1|%2|%3").arg(activity.processId).arg(activity.transport, activity.clientName);
}

QString cleanedPath(QString path)
{
    path = path.trimmed();
    if (path.size() >= 2)
    {
        const QChar first = path.front();
        const QChar last = path.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
        {
            path = path.mid(1, path.size() - 2).trimmed();
        }
    }
    return path;
}

bool containsUsableDevice(const QList<AdbDevice>& devices, const QString& serial)
{
    for (const AdbDevice& device : devices)
    {
        if (device.serial == serial && device.isUsable())
        {
            return true;
        }
    }
    return false;
}

QString portsText(const QList<int>& ports)
{
    QStringList values;
    for (int port : ports)
    {
        values.append(QString::number(port));
    }
    return values.join(", ");
}

QString portsText(const QSet<int>& ports)
{
    QList<int> sorted = ports.values();
    std::sort(sorted.begin(), sorted.end());
    return portsText(sorted);
}

struct WifiReadiness
{
    bool ready = true;
    QString message;
};

struct TransportRefreshResult
{
    int requestId = 0;
    bool validateUsbSelection = false;
    QString adbPath;
    QString requestedSerial;
    WifiReadiness wifi;
    AdbStatus adbStatus;
    QList<AdbDevice> devices;
    QString selectedSerial;
    QSet<int> reversePorts;
    QString usbStatus;
};

struct UsbConfigureResult
{
    int requestId = 0;
    QString adbPath;
    QString selectedSerial;
    AdbStatus adbStatus;
    QSet<int> reversePorts;
    QString error;
};

QString processOutput(const QString& program,
                      const QStringList& arguments,
                      QString* errorMessage = nullptr)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(3000))
    {
        process.kill();
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("%1 timed out.").arg(program);
        }
        return {};
    }

    const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = stderrText.isEmpty()
                ? QString("%1 exited with code %2.").arg(program).arg(process.exitCode())
                : stderrText;
        }
        return {};
    }
    return stdoutText;
}

WifiReadiness wifiReadinessStatus()
{
#if defined(Q_OS_MACOS)
    constexpr auto networksetup = "/usr/sbin/networksetup";
    QString error;
    const QString hardwarePorts =
        processOutput(networksetup, {"-listallhardwareports"}, &error);
    if (hardwarePorts.isEmpty())
    {
        return {false, "WiFi readiness unavailable: " + error};
    }

    bool readingWifiPort = false;
    QString wifiDevice;
    const QStringList lines = hardwarePorts.split('\n');
    for (const QString& rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        if (line.startsWith("Hardware Port:"))
        {
            const QString portName = line.mid(QString("Hardware Port:").size()).trimmed();
            readingWifiPort = portName == "Wi-Fi" || portName == "AirPort";
            continue;
        }
        if (readingWifiPort && line.startsWith("Device:"))
        {
            wifiDevice = line.mid(QString("Device:").size()).trimmed();
            break;
        }
    }

    if (wifiDevice.isEmpty())
    {
        return {false, "WiFi readiness unavailable: no WiFi network device was reported by networksetup."};
    }

    const QString power = processOutput(networksetup, {"-getairportpower", wifiDevice}, &error);
    if (power.isEmpty())
    {
        return {false, QString("WiFi readiness unavailable for %1: %2").arg(wifiDevice, error)};
    }
    if (power.endsWith(": On", Qt::CaseInsensitive) || power.endsWith(" On", Qt::CaseInsensitive))
    {
        return {true, QString("macOS WiFi is enabled on %1.").arg(wifiDevice)};
    }
    if (power.endsWith(": Off", Qt::CaseInsensitive) || power.endsWith(" Off", Qt::CaseInsensitive))
    {
        return {false, QString("macOS WiFi is disabled on %1.").arg(wifiDevice)};
    }
    return {false, QString("macOS WiFi state for %1 is unclear: %2").arg(wifiDevice, power)};
#elif defined(Q_OS_LINUX)
    return {true, "Linux will use the WiFi transport. Check the desktop network manager if streaming cannot connect."};
#elif defined(Q_OS_WIN)
    return {true, "Windows will use the WiFi transport."};
#else
    return {true, QString("%1 will use the WiFi transport.").arg(platformName())};
#endif
}

QList<AdbDevice> usableDevices(const QList<AdbDevice>& devices)
{
    QList<AdbDevice> usable;
    for (const AdbDevice& device : devices)
    {
        if (device.isUsable())
        {
            usable.append(device);
        }
    }
    return usable;
}

TransportRefreshResult collectTransportRefresh(int requestId,
                                               QString adbPath,
                                               QString requestedSerial,
                                               bool validateUsbSelection)
{
    TransportRefreshResult result;
    result.requestId = requestId;
    result.validateUsbSelection = validateUsbSelection;
    result.adbPath = std::move(adbPath);
    result.requestedSerial = std::move(requestedSerial);
    result.wifi = wifiReadinessStatus();
    result.adbStatus = AdbBridge::status(result.adbPath);

    if (!result.adbStatus.isAvailable())
    {
        result.usbStatus = result.adbStatus.message;
        return result;
    }

    QString error;
    result.devices = AdbBridge::devices(&error, result.adbPath);
    if (!error.isEmpty())
    {
        result.devices.clear();
        result.usbStatus = "adb is unavailable or failed: " + error;
        return result;
    }

    const QList<AdbDevice> usable = usableDevices(result.devices);
    if (result.requestedSerial.isEmpty() ||
        !containsUsableDevice(usable, result.requestedSerial))
    {
        result.selectedSerial = usable.isEmpty() ? QString() : usable.first().serial;
    }
    else
    {
        result.selectedSerial = result.requestedSerial;
    }

    if (!result.selectedSerial.isEmpty())
    {
        result.reversePorts =
            AdbBridge::reverseMappings(result.selectedSerial, &error, result.adbPath);
        if (!error.isEmpty())
        {
            result.reversePorts.clear();
            result.usbStatus = "Failed to read adb reverse mappings: " + error;
            return result;
        }
    }

    if (result.devices.isEmpty())
    {
        result.usbStatus = "No Quest device reported by adb.";
    }
    else if (usable.isEmpty())
    {
        result.usbStatus = "ADB sees device(s), but none are authorized for reverse tunneling.";
    }
    else
    {
        result.usbStatus =
            QString("Ready to configure USB ADB reverse for %1 authorized device%2.")
                .arg(usable.size())
                .arg(usable.size() == 1 ? "" : "s");
        if (!result.reversePorts.isEmpty())
        {
            result.usbStatus += " Active reverse ports: " + portsText(result.reversePorts) + ".";
        }
    }
    return result;
}

UsbConfigureResult collectUsbConfigure(int requestId,
                                       QString adbPath,
                                       QString selectedSerial)
{
    UsbConfigureResult result;
    result.requestId = requestId;
    result.adbPath = std::move(adbPath);
    result.selectedSerial = std::move(selectedSerial);
    result.adbStatus = AdbBridge::status(result.adbPath);
    if (!result.adbStatus.isAvailable())
    {
        result.error = result.adbStatus.message;
        return result;
    }

    QString error;
    result.reversePorts =
        AdbBridge::configureReverse(result.selectedSerial, &error, result.adbPath);
    if (!error.isEmpty())
    {
        result.error = "Failed to configure adb reverse: " + error;
    }
    return result;
}

} // namespace

QString filteredApplicationLogText(const QString& text)
{
    const QStringList lines = text.split('\n', Qt::KeepEmptyParts);
    QStringList filtered;
    filtered.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index)
    {
        const QString& line = lines[index];
        if (index == lines.size() - 1 && line.isEmpty())
        {
            filtered.append(line);
            continue;
        }
        if (!shouldDropApplicationLogLine(line))
        {
            filtered.append(line);
        }
    }
    return filtered.join('\n');
}

HomeModel::HomeModel(QObject* parent,
                     const QString& settingsOrganization,
                     const QString& settingsApplication,
                     HomePaths paths)
    : QObject(parent)
    , paths_(paths)
    , runtimeManager_(paths_)
    , settings_(settingsOrganization, settingsApplication)
{
    runtimeManifestPath_ =
        settings_.value("runtimeManifestPath", defaultRuntimeManifestPath()).toString();
    customAdbPath_ = cleanedPath(settings_.value("customAdbPath").toString());
    transportWorkerThread_ = new QThread(this);
    transportWorkerThread_->setObjectName("OXRSysHomeTransportWorker");
    transportWorker_ = new QObject();
    transportWorker_->moveToThread(transportWorkerThread_);
    connect(transportWorkerThread_, &QThread::finished, transportWorker_, &QObject::deleteLater);
    transportWorkerThread_->start();
    loadAll();

    configSaveTimer_.setSingleShot(true);
    configSaveTimer_.setInterval(600);
    connect(&configSaveTimer_, &QTimer::timeout,
            this, &HomeModel::saveStructuredConfig);

    pollTimer_.setInterval(1000);
    connect(&pollTimer_, &QTimer::timeout, this, [this]() {
        refreshRuntimeStatus();
        refreshRuntimeActivity();
        refreshTransportHealth();
        pollConfigChangesIfNeeded();
    });
    pollTimer_.start();
}

HomeModel::~HomeModel()
{
    pollTimer_.stop();
    configSaveTimer_.stop();
    if (transportWorkerThread_ != nullptr)
    {
        transportWorkerThread_->quit();
        transportWorkerThread_->wait(12000);
        transportWorker_ = nullptr;
    }
    const QList<QProcess*> processes = launchedProcesses_.values();
    for (QProcess* process : processes)
    {
        if (process->state() != QProcess::NotRunning)
        {
            process->terminate();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
}

const HomePaths& HomeModel::paths() const
{
    return paths_;
}

const ServerConfig& HomeModel::serverConfig() const
{
    return serverConfig_;
}

ServerConfig& HomeModel::mutableServerConfig()
{
    return serverConfig_;
}

const RuntimeRegistrationStatus& HomeModel::runtimeRegistrationStatus() const
{
    return runtimeRegistrationStatus_;
}

const RuntimeActivity& HomeModel::runtimeActivity() const
{
    return runtimeActivity_;
}

const QList<RuntimeStreamingStats>& HomeModel::runtimeStatsHistory() const
{
    return runtimeStatsHistory_;
}

const QList<LauncherApp>& HomeModel::launcherApps() const
{
    return launcherApps_;
}

const QList<AdbDevice>& HomeModel::questUsbDevices() const
{
    return questUsbDevices_;
}

const QSet<int>& HomeModel::selectedQuestUsbReversePorts() const
{
    return selectedQuestUsbReversePorts_;
}

const QMap<QString, QString>& HomeModel::appLogs() const
{
    return appLogs_;
}

QString HomeModel::runtimeManifestPath() const
{
    return runtimeManifestPath_;
}

QString HomeModel::activeLaunchRuntimeManifestPath() const
{
    return runtimeManager_.activeLaunchRuntimeManifestPath(runtimeManifestPath_);
}

QString HomeModel::statusMessage() const
{
    return statusMessage_;
}

QString HomeModel::questUsbStatus() const
{
    return questUsbStatus_;
}

const AdbStatus& HomeModel::adbStatus() const
{
    return adbStatus_;
}

QString HomeModel::customAdbPath() const
{
    return customAdbPath_;
}

QString HomeModel::selectedQuestUsbSerial() const
{
    return selectedQuestUsbSerial_;
}

QString HomeModel::selectedLogAppId() const
{
    return selectedLogAppId_;
}

QString HomeModel::currentProfileAppDisplayName() const
{
    if (!runtimeActivity_.applicationName.isEmpty())
    {
        return runtimeActivity_.applicationName;
    }
    if (!activeLaunchedAppId_.isEmpty())
    {
        for (const LauncherApp& app : launcherApps_)
        {
            if (app.id() == activeLaunchedAppId_)
            {
                return app.name;
            }
        }
    }
    return "None";
}

QString HomeModel::mainTransportSelection() const
{
    if (!mainTransportOverride_.isEmpty())
    {
        return mainTransportOverride_;
    }
    if (runtimeActivity_.isStreaming())
    {
        if (runtimeActivity_.transport == "wifi")
        {
            return "wifi";
        }
        if (runtimeActivity_.transport == "usb_adb")
        {
            return "usb_adb";
        }
    }
    if (serverConfig_.transport == "wifi")
    {
        return "wifi";
    }
    return "usb_adb";
}

TransportReadiness HomeModel::mainTransportReadiness() const
{
    if (mainTransportSelection() == "wifi")
    {
        return {wifiReady_, false, wifiStatus_};
    }

    if (!adbStatus_.isAvailable())
    {
        return {false, false, adbStatus_.message};
    }
    if (selectedQuestUsbSerial_.isEmpty() ||
        !containsUsableDevice(questUsbDevices_, selectedQuestUsbSerial_))
    {
        return {false, false, "No authorized Quest device visible over adb."};
    }

    const QList<int> expectedPorts = AdbBridge::reversePorts();
    bool allPortsConfigured = true;
    QList<int> missingPorts;
    for (int port : expectedPorts)
    {
        if (!selectedQuestUsbReversePorts_.contains(port))
        {
            allPortsConfigured = false;
            missingPorts.append(port);
        }
    }
    if (allPortsConfigured)
    {
        return {true, false,
                QString("USB reverse ready for %1 on %2.")
                    .arg(selectedQuestUsbSerial_, portsText(expectedPorts))};
    }
    return {false, true,
            QString("USB reverse missing port%1 %2.")
                .arg(missingPorts.size() > 1 ? "s" : "", portsText(missingPorts))};
}

bool HomeModel::developerModeEnabled() const
{
    return settings_.value("developerModeEnabled", false).toBool();
}

bool HomeModel::isAppRunning(const LauncherApp& app) const
{
    QProcess* process = launchedProcesses_.value(app.id(), nullptr);
    return process != nullptr && process->state() != QProcess::NotRunning;
}

void HomeModel::loadAll()
{
    loadConfigFromDisk();
    refreshRuntimeStatus();
    refreshRuntimeActivity();
    reloadLauncherApps();
    refreshTransportHealth(true);
    emit changed();
}

void HomeModel::setRuntimeManifestPath(const QString& path)
{
    runtimeManifestPath_ = cleanedPath(path);
    settings_.setValue("runtimeManifestPath", runtimeManifestPath_);
    setStatusMessage("Updated runtime manifest path.");
    emit changed();
}

void HomeModel::setDeveloperModeEnabled(bool enabled)
{
    settings_.setValue("developerModeEnabled", enabled);
    emit changed();
}

void HomeModel::setSelectedQuestUsbSerial(const QString& serial)
{
    selectedQuestUsbSerial_ = serial;
    selectedQuestUsbReversePorts_.clear();
    questUsbStatus_ = selectedQuestUsbSerial_.isEmpty()
        ? "No Quest device selected."
        : "Checking adb reverse mappings...";
    refreshTransportHealth(true);
    emit changed();
}

void HomeModel::setCustomAdbPath(const QString& path)
{
    customAdbPath_ = cleanedPath(path);
    if (customAdbPath_.isEmpty())
    {
        settings_.remove("customAdbPath");
    }
    else
    {
        settings_.setValue("customAdbPath", customAdbPath_);
    }
    selectedQuestUsbSerial_.clear();
    selectedQuestUsbReversePorts_.clear();
    refreshTransportHealth(true);
    setStatusMessage(customAdbPath_.isEmpty()
                         ? "ADB will be auto-detected."
                         : "Selected custom ADB executable.");
}

void HomeModel::clearCustomAdbPath()
{
    setCustomAdbPath(QString());
}

void HomeModel::setSelectedLogAppId(const QString& appId)
{
    selectedLogAppId_ = appId;
    emit changed();
}

void HomeModel::setMainTransportSelection(const QString& transport)
{
    if (transport == "usb_adb")
    {
        pendingMainTransportSelection_ = transport;
        questUsbStatus_ = "Checking USB ADB transport...";
        setStatusMessage("Checking USB ADB transport.");
        requestTransportRefresh(true, true);
        emit changed();
        return;
    }
    pendingMainTransportSelection_.clear();
    mainTransportOverride_ = transport;
    serverConfig_.transport = transport;
    saveStructuredConfig();
    refreshTransportHealth(true);
}

void HomeModel::saveStructuredConfig()
{
    QDir().mkpath(QFileInfo(paths_.configFilePath).absolutePath());
    const QString text = serverConfig_.mergedInto(currentConfigText_);
    QFile file(paths_.configFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        setErrorMessage("Failed to save config: " + paths_.configFilePath);
        return;
    }
    file.write(text.toUtf8());
    currentConfigText_ = text;
    lastKnownConfigModificationDate_ = modificationDate(paths_.configFilePath);
    setStatusMessage("Saved runtime configuration.");
    emit changed();
}

void HomeModel::scheduleStructuredConfigSave()
{
    configSaveTimer_.start();
}

void HomeModel::resetConfigFromDisk()
{
    configSaveTimer_.stop();
    loadConfigFromDisk();
    setStatusMessage("Reloaded configuration from disk.");
    emit changed();
}

void HomeModel::resetStreamingConfigToDefaults()
{
    configSaveTimer_.stop();
    serverConfig_ = ServerConfig();
    saveStructuredConfig();
    setStatusMessage("Restored default streaming configuration.");
    emit changed();
}

void HomeModel::refreshRuntimeStatus()
{
    runtimeRegistrationStatus_ = runtimeManager_.registrationStatus();
    emit changed();
}

void HomeModel::registerRuntime()
{
    runtimeManifestPath_ = cleanedPath(runtimeManifestPath_);
    settings_.setValue("runtimeManifestPath", runtimeManifestPath_);
    const bool wasRegistered = runtimeRegistrationStatus_.activeRuntimeExists;
    QString error;
    if (!runtimeManager_.registerRuntimeManifest(runtimeManifestPath_, &error))
    {
        setErrorMessage("Failed to register runtime: " + error);
        return;
    }
    refreshRuntimeStatus();
    setStatusMessage(wasRegistered ? "Updated the OpenXR runtime registration."
                                   : "Registered the OpenXR runtime.");
}

void HomeModel::unregisterRuntime()
{
    QString error;
    if (!runtimeManager_.unregisterRuntime(&error))
    {
        setErrorMessage("Failed to unregister runtime: " + error);
        return;
    }
    refreshRuntimeStatus();
    setStatusMessage("Unregistered the OpenXR runtime.");
}

void HomeModel::reloadLauncherApps()
{
    launcherStore_ = LauncherStore::load(paths_.launcherAppsPath);
    launcherApps_ = LauncherScanner::merge(LauncherScanner::scanAutomaticApps(), launcherStore_);
    if (!selectedLogAppId_.isEmpty())
    {
        bool stillPresent = false;
        for (const LauncherApp& app : launcherApps_)
        {
            if (app.id() == selectedLogAppId_)
            {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent)
        {
            selectedLogAppId_.clear();
        }
    }
    emit changed();
}

void HomeModel::addLauncherApp(const QString& path)
{
    LauncherApp app = LauncherScanner::inspectPath(path, "manual", true);
    if (app.name.isEmpty())
    {
        setErrorMessage("No launchable app or executable was found at " + path);
        return;
    }

    const QString appPath = normalizedPath(app.path);
    launcherStore_.manualApps.erase(
        std::remove_if(launcherStore_.manualApps.begin(), launcherStore_.manualApps.end(),
                       [&appPath](const LauncherApp& stored) {
                           return normalizedPath(stored.path) == appPath;
                       }),
        launcherStore_.manualApps.end());
    launcherStore_.manualApps.append(app);
    launcherStore_.hiddenAutomaticAppPaths.removeAll(app.path);
    launcherStore_.save(paths_.launcherAppsPath);
    reloadLauncherApps();
    setStatusMessage(QString("Added %1 to the launcher.").arg(app.name));
}

void HomeModel::removeLauncherApp(const LauncherApp& app)
{
    stopApp(app);
    const QString appPath = normalizedPath(app.path);
    if (app.source == "manual")
    {
        launcherStore_.manualApps.erase(
            std::remove_if(launcherStore_.manualApps.begin(), launcherStore_.manualApps.end(),
                           [&appPath](const LauncherApp& stored) {
                               return normalizedPath(stored.path) == appPath;
                           }),
            launcherStore_.manualApps.end());
    }
    else if (!launcherStore_.hiddenAutomaticAppPaths.contains(app.path))
    {
        launcherStore_.hiddenAutomaticAppPaths.append(app.path);
    }
    launcherStore_.save(paths_.launcherAppsPath);
    reloadLauncherApps();
    setStatusMessage(QString("Removed %1 from the launcher.").arg(app.name));
}

void HomeModel::launchApp(const LauncherApp& app)
{
    if (isAppRunning(app))
    {
        return;
    }

    const QString manifestPath = activeLaunchRuntimeManifestPath();
    if (!QFileInfo(manifestPath).isFile())
    {
        setErrorMessage("Runtime JSON not found at " + manifestPath);
        return;
    }
    if (!isExecutableFile(app.executablePath))
    {
        setErrorMessage("Executable not found: " + app.executablePath);
        return;
    }

    auto* process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XR_RUNTIME_JSON", manifestPath);
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(QFileInfo(app.executablePath).absolutePath());
    process->setProgram(app.executablePath);

    const QString appId = app.id();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, appId]() {
        appendLog(appId, QString::fromUtf8(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process, appId]() {
        appendLog(appId, QString::fromUtf8(process->readAllStandardError()));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, appId](int exitCode, QProcess::ExitStatus) {
                finishLaunchedApp(appId, exitCode);
            });

    launchedProcesses_[appId] = process;
    appendLog(appId, QString("Launching %1\nXR_RUNTIME_JSON=%2\n\n").arg(app.name, manifestPath));
    process->start();
    if (!process->waitForStarted(3000))
    {
        const QString error = process->errorString();
        cleanupLaunchState(appId);
        setErrorMessage(QString("Failed to launch %1: %2").arg(app.name, error));
        return;
    }

    activeLaunchedAppId_ = appId;
    selectedLogAppId_ = appId;
    setStatusMessage(QString("Launched %1.").arg(app.name));
    emit changed();
}

void HomeModel::runAppInTerminal(const LauncherApp& app)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(app);
    return;
#else
    const QString manifestPath = activeLaunchRuntimeManifestPath();
    if (!QFileInfo(manifestPath).isFile())
    {
        setErrorMessage("Runtime JSON not found at " + manifestPath);
        return;
    }
    if (!isExecutableFile(app.executablePath))
    {
        setErrorMessage("Executable not found: " + app.executablePath);
        return;
    }

    const QString scriptsRoot = QDir(paths_.configRoot).filePath("TerminalLaunchers");
    if (!QDir().mkpath(scriptsRoot))
    {
        setErrorMessage("Failed to create terminal launcher directory: " + scriptsRoot);
        return;
    }

#if defined(Q_OS_MACOS)
    constexpr auto scriptSuffix = ".command";
#else
    constexpr auto scriptSuffix = ".sh";
#endif
    const QString scriptPath =
        QDir(scriptsRoot).filePath(terminalSafeName(app.name) + scriptSuffix);
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        setErrorMessage("Failed to write terminal launcher script: " + scriptPath);
        return;
    }
    scriptFile.write(terminalLaunchScript(app, manifestPath).toUtf8());
    scriptFile.close();
    QFile::setPermissions(scriptPath,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                              QFile::ReadGroup | QFile::ExeGroup |
                              QFile::ReadOther | QFile::ExeOther);

    bool started = false;
#if defined(Q_OS_MACOS)
    started = QProcess::startDetached("open", {scriptPath});
#else
    struct TerminalCommand
    {
        const char* program;
        QStringList arguments;
    };
    const QList<TerminalCommand> terminalCommands = {
        {"x-terminal-emulator", {"-e", scriptPath}},
        {"gnome-terminal", {"--", scriptPath}},
        {"konsole", {"-e", scriptPath}},
        {"xterm", {"-e", scriptPath}},
    };
    for (const TerminalCommand& command : terminalCommands)
    {
        if (QProcess::startDetached(command.program, command.arguments))
        {
            started = true;
            break;
        }
    }
#endif

    if (!started)
    {
        setErrorMessage("Failed to open a terminal for " + app.name);
        return;
    }

    selectedLogAppId_ = app.id();
    appendLog(app.id(),
              QString("Terminal launcher: %1\nXR_RUNTIME_JSON=%2\n\n")
                  .arg(scriptPath, manifestPath));
    setStatusMessage(QString("Opened %1 in a terminal.").arg(app.name));
    emit changed();
#endif
}

void HomeModel::stopApp(const LauncherApp& app)
{
    QProcess* process = launchedProcesses_.value(app.id(), nullptr);
    if (process == nullptr || process->state() == QProcess::NotRunning)
    {
        return;
    }
    process->terminate();
    setStatusMessage(QString("Stopping %1.").arg(app.name));
}

void HomeModel::clearLog(const LauncherApp& app)
{
    appLogs_[app.id()].clear();
    emit changed();
}

void HomeModel::showLogs(const LauncherApp& app)
{
    selectedLogAppId_ = app.id();
    emit changed();
}

void HomeModel::refreshQuestUsbDevices()
{
    requestTransportRefresh(true);
}

void HomeModel::configureQuestUsbReverse()
{
    if (!adbStatus_.isAvailable())
    {
        questUsbStatus_ = adbStatus_.message;
        setErrorMessage(questUsbStatus_);
        return;
    }
    if (selectedQuestUsbSerial_.isEmpty() ||
        !containsUsableDevice(questUsbDevices_, selectedQuestUsbSerial_))
    {
        questUsbStatus_ = "Select an authorized Quest device before configuring USB.";
        emit changed();
        return;
    }

    requestUsbReverseConfigure();
}

void HomeModel::refreshRuntimeActivity()
{
#if defined(Q_OS_UNIX)
    constexpr bool validateProcess = true;
#else
    constexpr bool validateProcess = false;
#endif
    const RuntimeActivity activity =
        RuntimeActivity::readFromFile(paths_.runtimeStatusPath, validateProcess);
    updateRuntimeStatsHistory(activity);
    runtimeActivity_ = activity;
    emit changed();
}

void HomeModel::loadConfigFromDisk()
{
    QFile file(paths_.configFilePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        currentConfigText_ = QString::fromUtf8(file.readAll());
        lastKnownConfigModificationDate_ = modificationDate(paths_.configFilePath);
    }
    else
    {
        currentConfigText_ = ServerConfig::defaultText();
        lastKnownConfigModificationDate_ = QDateTime();
    }
    serverConfig_ = ServerConfig::parse(currentConfigText_);
}

void HomeModel::appendLog(const QString& appId, const QString& text)
{
    const QString filteredText = filteredApplicationLogText(text);
    if (filteredText.isEmpty())
    {
        return;
    }

    QString combined = appLogs_.value(appId) + filteredText;
    if (combined.size() > MaxLogCharacters)
    {
        combined = combined.right(MaxLogCharacters);
    }
    appLogs_[appId] = combined;
    emit changed();
}

void HomeModel::finishLaunchedApp(const QString& appId, int exitCode)
{
    appendLog(appId, QString("\nProcess exited with code %1.\n").arg(exitCode));
    cleanupLaunchState(appId);
    emit changed();
}

void HomeModel::cleanupLaunchState(const QString& appId)
{
    QProcess* process = launchedProcesses_.take(appId);
    if (process != nullptr)
    {
        process->deleteLater();
    }
    if (activeLaunchedAppId_ == appId)
    {
        activeLaunchedAppId_.clear();
        for (auto it = launchedProcesses_.cbegin(); it != launchedProcesses_.cend(); ++it)
        {
            if (it.value()->state() != QProcess::NotRunning)
            {
                activeLaunchedAppId_ = it.key();
                break;
            }
        }
    }
}

void HomeModel::refreshTransportHealth(bool force)
{
    requestTransportRefresh(force);
}

void HomeModel::requestTransportRefresh(bool force, bool validateUsbSelection)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!force && lastTransportHealthRefreshDate_.isValid() &&
        lastTransportHealthRefreshDate_.secsTo(now) < 5)
    {
        return;
    }
    if (!force && transportRefreshPending_)
    {
        return;
    }
    lastTransportHealthRefreshDate_ = now;
    if (transportWorker_ == nullptr)
    {
        return;
    }

    const int requestId = ++nextTransportRequestId_;
    latestTransportRefreshRequestId_ = requestId;
    transportRefreshPending_ = true;
    wifiStatus_ = "Checking WiFi transport readiness...";
    questUsbStatus_ = validateUsbSelection
        ? "Checking USB ADB transport..."
        : "Checking USB ADB devices...";

    const QString adbPath = customAdbPath_;
    const QString requestedSerial = selectedQuestUsbSerial_;
    QPointer<HomeModel> model(this);
    QMetaObject::invokeMethod(transportWorker_,
                              [model, requestId, adbPath, requestedSerial, validateUsbSelection]() {
                                  const TransportRefreshResult result =
                                      collectTransportRefresh(requestId,
                                                              adbPath,
                                                              requestedSerial,
                                                              validateUsbSelection);
                                  HomeModel* target = model.data();
                                  if (target == nullptr)
                                  {
                                      return;
                                  }
                                  QMetaObject::invokeMethod(
                                      target,
                                      [model, result]() {
                                          HomeModel* target = model.data();
                                          if (target == nullptr ||
                                              result.requestId !=
                                                  target->latestTransportRefreshRequestId_)
                                          {
                                              return;
                                          }

                                          target->transportRefreshPending_ = false;
                                          target->wifiReady_ = result.wifi.ready;
                                          target->wifiStatus_ = result.wifi.message;
                                          target->adbStatus_ = result.adbStatus;
                                          target->questUsbDevices_ = result.devices;
                                          target->selectedQuestUsbSerial_ =
                                              result.selectedSerial;
                                          target->selectedQuestUsbReversePorts_ =
                                              result.reversePorts;
                                          target->questUsbStatus_ = result.usbStatus;

                                          if (result.validateUsbSelection &&
                                              target->pendingMainTransportSelection_ == "usb_adb")
                                          {
                                              target->pendingMainTransportSelection_.clear();
                                              if (target->adbStatus_.isAvailable())
                                              {
                                                  target->mainTransportOverride_ = "usb_adb";
                                                  target->serverConfig_.transport = "usb_adb";
                                                  target->saveStructuredConfig();
                                                  target->setStatusMessage(
                                                      "Selected USB ADB transport.");
                                              }
                                              else
                                              {
                                                  target->setErrorMessage(target->questUsbStatus_);
                                              }
                                          }
                                          emit target->changed();
                                      },
                                      Qt::QueuedConnection);
                              },
                              Qt::QueuedConnection);
    emit changed();
}

void HomeModel::requestUsbReverseConfigure()
{
    if (transportWorker_ == nullptr)
    {
        return;
    }

    const int requestId = ++nextTransportRequestId_;
    latestUsbConfigureRequestId_ = requestId;
    usbConfigurePending_ = true;
    questUsbStatus_ = "Configuring USB ADB reverse...";

    const QString adbPath = customAdbPath_;
    const QString selectedSerial = selectedQuestUsbSerial_;
    QPointer<HomeModel> model(this);
    QMetaObject::invokeMethod(transportWorker_,
                              [model, requestId, adbPath, selectedSerial]() {
                                  const UsbConfigureResult result =
                                      collectUsbConfigure(requestId, adbPath, selectedSerial);
                                  HomeModel* target = model.data();
                                  if (target == nullptr)
                                  {
                                      return;
                                  }
                                  QMetaObject::invokeMethod(
                                      target,
                                      [model, result]() {
                                          HomeModel* target = model.data();
                                          if (target == nullptr ||
                                              result.requestId !=
                                                  target->latestUsbConfigureRequestId_ ||
                                              result.adbPath != target->customAdbPath_ ||
                                              result.selectedSerial !=
                                                  target->selectedQuestUsbSerial_)
                                          {
                                              return;
                                          }

                                          target->usbConfigurePending_ = false;
                                          target->adbStatus_ = result.adbStatus;
                                          if (!result.error.isEmpty())
                                          {
                                              target->questUsbStatus_ = result.error;
                                              target->setErrorMessage(target->questUsbStatus_);
                                              return;
                                          }

                                          target->selectedQuestUsbReversePorts_ =
                                              result.reversePorts;
                                          target->questUsbStatus_ =
                                              QString("Verified adb reverse for %1 on ports %2.")
                                                  .arg(result.selectedSerial,
                                                       portsText(result.reversePorts));
                                          target->setStatusMessage(
                                              "Configured Quest USB ADB transport.");
                                          emit target->changed();
                                      },
                                      Qt::QueuedConnection);
                              },
                              Qt::QueuedConnection);
    emit changed();
}

void HomeModel::pollConfigChangesIfNeeded()
{
    const QDateTime currentModificationDate = modificationDate(paths_.configFilePath);
    if (currentModificationDate == lastKnownConfigModificationDate_)
    {
        return;
    }
    loadConfigFromDisk();
    emit changed();
}

void HomeModel::updateRuntimeStatsHistory(const RuntimeActivity& activity)
{
    if (!activity.isStreaming())
    {
        runtimeStatsIdentity_.clear();
        runtimeStatsHistory_.clear();
        return;
    }

    const QString identity = runtimeStatsIdentity(activity);
    if (runtimeStatsIdentity_ != identity)
    {
        runtimeStatsIdentity_ = identity;
        runtimeStatsHistory_.clear();
    }

    if (!activity.hasStreamingStats)
    {
        return;
    }
    if (!runtimeStatsHistory_.isEmpty() &&
        runtimeStatsHistory_.last().sampleUnixMilliseconds ==
            activity.streamingStats.sampleUnixMilliseconds)
    {
        runtimeStatsHistory_.last() = activity.streamingStats;
    }
    else
    {
        runtimeStatsHistory_.append(activity.streamingStats);
        while (runtimeStatsHistory_.size() > MaxRuntimeStatsSamples)
        {
            runtimeStatsHistory_.removeFirst();
        }
    }
}

void HomeModel::setStatusMessage(const QString& message)
{
    statusMessage_ = message;
    emit statusMessageChanged(message);
    emit changed();
}

void HomeModel::setErrorMessage(const QString& message)
{
    emit errorOccurred(message);
    emit changed();
}
