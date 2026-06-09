// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "PlatformSupport.h"

#include <QString>
#include <QStringList>

struct RuntimeRegistrationStatus
{
    bool activeRuntimeExists = false;
    QString activeRuntimeTarget;
};

struct RuntimeInstallStatus
{
    bool bundledRuntimeExists = false;
    bool installedRuntimeExists = false;
    bool installedManifestExists = false;
    bool installedRuntimeNeedsUpdate = false;
    QString bundledRuntimePath;
    QString installedManifestPath;
};

class RuntimeManager
{
public:
    explicit RuntimeManager(HomePaths paths = homePaths());

    const HomePaths& paths() const;
    RuntimeRegistrationStatus registrationStatus() const;
    RuntimeInstallStatus installStatus() const;
    QString activeRuntimeTarget() const;
    QString activeLaunchRuntimeManifestPath(const QString& selectedManifestPath,
                                            bool preferInstalledRuntime = false) const;

    bool registerRuntimeManifest(const QString& manifestPath, QString* errorMessage) const;
    bool unregisterRuntime(QString* errorMessage) const;
    bool installBundledRuntime(QString* installedManifestPath, QString* errorMessage) const;

    static QString runtimeManifestJson(const QString& libraryPath);
    static int handleElevatedWindowsCommand(const QStringList& arguments, QString* errorMessage);

private:
    QString bundledRuntimeDirectory() const;
    bool replaceFile(const QString& source, const QString& destination, QString* errorMessage) const;
    bool filesHaveSameContents(const QString& lhs, const QString& rhs) const;

    HomePaths paths_;
};
