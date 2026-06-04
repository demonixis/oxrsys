// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>
#include <QStringList>

struct HomePaths
{
    QString configRoot;
    QString dataRoot;
    QString stateRoot;
    QString activeRuntimeDirectory;
    QString activeRuntimePath;
    QString configFilePath;
    QString launcherAppsPath;
    QString installedRuntimeDirectory;
    QString installedRuntimeManifestPath;
    QString runtimeStatusPath;
};

QString platformName();
QString runtimeLibraryFileName();
QString runtimeBuildPresetName();
QString runtimeManifestFileName();
QString simulatorExecutableName();
QString pathListSeparator();
QString normalizedPath(const QString& path);
QString shellQuoted(const QString& value);
QStringList splitCommandLine(const QString& commandLine);
QString resolveExecutableFromPath(const QString& executableName);
bool isExecutableFile(const QString& path);
bool isProcessRunning(qint64 processId);
bool revealInFileManager(const QString& path);
HomePaths homePaths();
QStringList runtimeBuildDirectoryCandidates();
QString defaultRuntimeManifestPath();
QString defaultRuntimeDirectoryPath();
bool supportsRuntimeInstall();
bool supportsRuntimeGlobalRegistration();
bool supportsRuntimeInstallAndRegistration();
