// SPDX-License-Identifier: MPL-2.0

#include "HomeModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>

namespace
{

constexpr int MaxLogCharacters = 30000;
constexpr int MaxRuntimeStatsSamples = 60;

QDateTime modificationDate(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? info.lastModified() : QDateTime();
}

QString runtimeStatsIdentity(const RuntimeActivity& activity)
{
    return QString("%1|%2|%3").arg(activity.processId).arg(activity.transport, activity.clientName);
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

} // namespace

HomeModel::HomeModel(QObject* parent)
    : QObject(parent)
    , paths_(homePaths())
    , runtimeManager_(paths_)
    , settings_("OXRSys", "HomeQt")
{
    runtimeManifestPath_ =
        settings_.value("runtimeManifestPath", defaultRuntimeManifestPath()).toString();
    loadAll();

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

const RuntimeInstallStatus& HomeModel::runtimeInstallStatus() const
{
    return runtimeInstallStatus_;
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
        return {
            true,
            false,
            QString("%1 will use the WiFi transport.").arg(platformName()),
        };
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
    refreshRuntimeInstallStatus();
    refreshRuntimeActivity();
    reloadLauncherApps();
    refreshTransportHealth(true);
    emit changed();
}

void HomeModel::setRuntimeManifestPath(const QString& path)
{
    runtimeManifestPath_ = path;
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
    if (!selectedQuestUsbSerial_.isEmpty())
    {
        QString error;
        selectedQuestUsbReversePorts_ =
            AdbBridge::reverseMappings(selectedQuestUsbSerial_, &error);
        if (!error.isEmpty())
        {
            questUsbStatus_ = "Failed to read adb reverse mappings: " + error;
        }
    }
    emit changed();
}

void HomeModel::setSelectedLogAppId(const QString& appId)
{
    selectedLogAppId_ = appId;
    emit changed();
}

void HomeModel::setMainTransportSelection(const QString& transport)
{
    if (transport == "usb_adb" && !AdbBridge::status().isAvailable())
    {
        questUsbStatus_ = "ADB is unavailable. USB mode needs adb.";
        setErrorMessage(questUsbStatus_);
        return;
    }
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

void HomeModel::resetConfigFromDisk()
{
    loadConfigFromDisk();
    setStatusMessage("Reloaded configuration from disk.");
    emit changed();
}

void HomeModel::installBundledRuntimeAndRegister()
{
    QString installedManifestPath;
    QString error;
    if (!runtimeManager_.installBundledRuntime(&installedManifestPath, &error))
    {
        setErrorMessage("Failed to install runtime: " + error);
        return;
    }
    runtimeManifestPath_ = installedManifestPath;
    settings_.setValue("runtimeManifestPath", runtimeManifestPath_);
    loadConfigFromDisk();
    refreshRuntimeInstallStatus();
    if (!runtimeManager_.registerRuntimeManifest(runtimeManifestPath_, &error))
    {
        setErrorMessage("Runtime installed but registration failed: " + error);
        return;
    }
    refreshRuntimeStatus();
    setStatusMessage("Installed and registered the bundled OXRSys runtime.");
    emit changed();
}

void HomeModel::useInstalledRuntimeManifest()
{
    runtimeManifestPath_ = runtimeInstallStatus_.installedManifestPath;
    settings_.setValue("runtimeManifestPath", runtimeManifestPath_);
    setStatusMessage("Selected the installed runtime manifest.");
    emit changed();
}

void HomeModel::refreshRuntimeStatus()
{
    runtimeRegistrationStatus_ = runtimeManager_.registrationStatus();
    emit changed();
}

void HomeModel::refreshRuntimeInstallStatus()
{
    runtimeInstallStatus_ = runtimeManager_.installStatus();
    emit changed();
}

void HomeModel::registerRuntime()
{
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
    adbStatus_ = AdbBridge::status();
    if (!adbStatus_.isAvailable())
    {
        questUsbDevices_.clear();
        selectedQuestUsbSerial_.clear();
        selectedQuestUsbReversePorts_.clear();
        questUsbStatus_ = "ADB is unavailable. Install Android Platform Tools before using USB mode.";
        emit changed();
        return;
    }

    QString error;
    questUsbDevices_ = AdbBridge::devices(&error);
    if (!error.isEmpty())
    {
        questUsbDevices_.clear();
        selectedQuestUsbSerial_.clear();
        selectedQuestUsbReversePorts_.clear();
        questUsbStatus_ = "adb is unavailable or failed: " + error;
        emit changed();
        return;
    }

    QList<AdbDevice> usableDevices;
    for (const AdbDevice& device : questUsbDevices_)
    {
        if (device.isUsable())
        {
            usableDevices.append(device);
        }
    }
    if (selectedQuestUsbSerial_.isEmpty() ||
        !containsUsableDevice(usableDevices, selectedQuestUsbSerial_))
    {
        selectedQuestUsbSerial_ = usableDevices.isEmpty() ? QString() : usableDevices.first().serial;
    }

    selectedQuestUsbReversePorts_.clear();
    if (!selectedQuestUsbSerial_.isEmpty())
    {
        selectedQuestUsbReversePorts_ =
            AdbBridge::reverseMappings(selectedQuestUsbSerial_, &error);
    }

    if (questUsbDevices_.isEmpty())
    {
        questUsbStatus_ = "No Quest device reported by adb.";
    }
    else if (usableDevices.isEmpty())
    {
        questUsbStatus_ = "ADB sees device(s), but none are authorized for reverse tunneling.";
    }
    else
    {
        questUsbStatus_ =
            QString("Ready to configure USB ADB reverse for %1 authorized device%2.")
                .arg(usableDevices.size())
                .arg(usableDevices.size() == 1 ? "" : "s");
        if (!selectedQuestUsbReversePorts_.isEmpty())
        {
            questUsbStatus_ += " Active reverse ports: " + portsText(selectedQuestUsbReversePorts_) + ".";
        }
    }
    emit changed();
}

void HomeModel::configureQuestUsbReverse()
{
    adbStatus_ = AdbBridge::status();
    if (!adbStatus_.isAvailable())
    {
        questUsbStatus_ = "ADB is unavailable. Install Android Platform Tools before configuring USB.";
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

    QString error;
    selectedQuestUsbReversePorts_ = AdbBridge::configureReverse(selectedQuestUsbSerial_, &error);
    if (!error.isEmpty())
    {
        questUsbStatus_ = "Failed to configure adb reverse: " + error;
        setErrorMessage(questUsbStatus_);
        return;
    }
    questUsbStatus_ =
        QString("Verified adb reverse for %1 on ports %2.")
            .arg(selectedQuestUsbSerial_, portsText(selectedQuestUsbReversePorts_));
    setStatusMessage("Configured Quest USB ADB transport.");
    emit changed();
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
    QString combined = appLogs_.value(appId) + text;
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
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!force && lastTransportHealthRefreshDate_.isValid() &&
        lastTransportHealthRefreshDate_.secsTo(now) < 5)
    {
        return;
    }
    lastTransportHealthRefreshDate_ = now;
    refreshQuestUsbDevices();
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
