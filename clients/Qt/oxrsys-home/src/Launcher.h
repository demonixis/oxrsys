// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct LauncherApp
{
    QString name;
    QString path;
    QString executablePath;
    QString kind = "custom";
    QString source = "manual";
    QDateTime lastSeen;

    QString id() const;
    QString kindDisplayName() const;
    QString sourceDisplayName() const;
    QJsonObject toJson() const;
    static LauncherApp fromJson(const QJsonObject& object);
};

struct LauncherStore
{
    QList<LauncherApp> manualApps;
    QStringList hiddenAutomaticAppPaths;

    static LauncherStore load(const QString& path);
    void save(const QString& path) const;
};

class LauncherScanner
{
public:
    static LauncherApp inspectPath(const QString& path,
                                   const QString& source,
                                   bool allowUnknown);
    static QList<LauncherApp> scanAutomaticApps();
    static QList<LauncherApp> merge(const QList<LauncherApp>& automaticApps,
                                    const LauncherStore& store);
};

QString launcherKindForMetadata(const QString& name,
                                const QString& identifier,
                                const QString& path,
                                const QString& executablePath);
QString terminalSafeName(const QString& value);
QString terminalLaunchScript(const LauncherApp& app, const QString& runtimeManifestPath);
