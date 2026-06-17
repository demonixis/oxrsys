// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "AdbBridge.h"
#include "Launcher.h"
#include "RuntimeActivity.h"
#include "RuntimeManager.h"
#include "ServerConfig.h"

#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QTimer>

class QProcess;
class QThread;

struct TransportReadiness
{
    bool isReady = false;
    bool canConfigureUsb = false;
    QString message;
};

TransportReadiness questUsbTransportReadiness(const AdbStatus& adbStatus,
                                              const QList<AdbDevice>& devices,
                                              const QString& selectedSerial,
                                              const QSet<int>& reversePorts);

class HomeModel final : public QObject
{
    Q_OBJECT

public:
    explicit HomeModel(QObject* parent = nullptr,
                       const QString& settingsOrganization = "OXRSys",
                       const QString& settingsApplication = "HomeQt",
                       HomePaths paths = homePaths());
    HomeModel(QObject* parent,
              const QString& settingsOrganization,
              const QString& settingsApplication,
              HomePaths paths,
              bool enableTransportChecks);
    ~HomeModel() override;

    const HomePaths& paths() const;
    const ServerConfig& serverConfig() const;
    ServerConfig& mutableServerConfig();
    const RuntimeRegistrationStatus& runtimeRegistrationStatus() const;
    const RuntimeInstallStatus& runtimeInstallStatus() const;
    const RuntimeActivity& runtimeActivity() const;
    const QList<RuntimeStreamingStats>& runtimeStatsHistory() const;
    const QList<LauncherApp>& launcherApps() const;
    const QList<AdbDevice>& questUsbDevices() const;
    const QSet<int>& selectedQuestUsbReversePorts() const;
    const QMap<QString, QString>& appLogs() const;

    QString runtimeManifestPath() const;
    QString activeLaunchRuntimeManifestPath() const;
    QString statusMessage() const;
    QString questUsbStatus() const;
    const AdbStatus& adbStatus() const;
    QString customAdbPath() const;
    QString selectedQuestUsbSerial() const;
    QString selectedLogAppId() const;
    QString currentProfileAppDisplayName() const;
    QString mainTransportSelection() const;
    TransportReadiness mainTransportReadiness() const;
    bool developerModeEnabled() const;
    bool preferInstalledRuntimeForLaunches() const;
    bool runtimeOperationPending() const;
    bool isAppRunning(const LauncherApp& app) const;

public slots:
    void loadAll();
    void setRuntimeManifestPath(const QString& path);
    void setDeveloperModeEnabled(bool enabled);
    void setSelectedQuestUsbSerial(const QString& serial);
    void setCustomAdbPath(const QString& path);
    void clearCustomAdbPath();
    void setSelectedLogAppId(const QString& appId);
    void setMainTransportSelection(const QString& transport);
    void saveStructuredConfig();
    void scheduleStructuredConfigSave();
    void resetConfigFromDisk();
    void resetStreamingConfigToDefaults();
    void setPreferInstalledRuntimeForLaunches(bool enabled);
    void installBundledRuntimeAndRegister();
    void useInstalledRuntimeManifest();
    void refreshRuntimeStatus();
    void refreshRuntimeInstallStatus();
    void registerRuntime();
    void unregisterRuntime();
    void reloadLauncherApps();
    void addLauncherApp(const QString& path);
    void removeLauncherApp(const LauncherApp& app);
    void launchApp(const LauncherApp& app);
    void runAppInTerminal(const LauncherApp& app);
    void stopApp(const LauncherApp& app);
    void clearLog(const LauncherApp& app);
    void showLogs(const LauncherApp& app);
    void refreshQuestUsbDevices();
    void configureQuestUsbReverse();
    void refreshRuntimeActivity();

signals:
    void changed();
    void statusMessageChanged(const QString& message);
    void errorOccurred(const QString& message);

private:
    void loadAll(bool refreshTransport);
    void loadConfigFromDisk();
    void appendLog(const QString& appId, const QString& text);
    void finishLaunchedApp(const QString& appId, int exitCode);
    void cleanupLaunchState(const QString& appId);
    void refreshTransportHealth(bool force = false);
    void requestTransportRefresh(bool force, bool validateUsbSelection = false);
    void requestUsbReverseConfigure();
    void pollConfigChangesIfNeeded();
    void updateRuntimeStatsHistory(const RuntimeActivity& activity);
    void setStatusMessage(const QString& message);
    void setErrorMessage(const QString& message);

    HomePaths paths_;
    RuntimeManager runtimeManager_;
    QSettings settings_;
    QTimer pollTimer_;
    QTimer configSaveTimer_;
    QThread* transportWorkerThread_ = nullptr;
    QObject* transportWorker_ = nullptr;
    bool transportChecksEnabled_ = true;
    QString runtimeManifestPath_;
    bool preferInstalledRuntimeForLaunches_ = false;
    ServerConfig serverConfig_;
    QString currentConfigText_;
    QDateTime lastKnownConfigModificationDate_;
    RuntimeRegistrationStatus runtimeRegistrationStatus_;
    RuntimeInstallStatus runtimeInstallStatus_;
    RuntimeActivity runtimeActivity_;
    QList<RuntimeStreamingStats> runtimeStatsHistory_;
    QString runtimeStatsIdentity_;
    LauncherStore launcherStore_;
    QList<LauncherApp> launcherApps_;
    QMap<QString, QProcess*> launchedProcesses_;
    QMap<QString, QString> appLogs_;
    QString activeLaunchedAppId_;
    QString selectedLogAppId_;
    QList<AdbDevice> questUsbDevices_;
    QString selectedQuestUsbSerial_;
    QString questUsbStatus_ = "USB ADB transport is not configured.";
    QSet<int> selectedQuestUsbReversePorts_;
    AdbStatus adbStatus_;
    QString customAdbPath_;
    bool wifiReady_ = true;
    QString wifiStatus_ = "WiFi transport readiness has not been checked.";
    QString mainTransportOverride_;
    QDateTime lastTransportHealthRefreshDate_;
    int nextTransportRequestId_ = 0;
    int latestTransportRefreshRequestId_ = 0;
    int latestUsbConfigureRequestId_ = 0;
    bool transportRefreshPending_ = false;
    bool usbConfigurePending_ = false;
    bool runtimeOperationPending_ = false;
    QString pendingMainTransportSelection_;
    QString statusMessage_;
};
