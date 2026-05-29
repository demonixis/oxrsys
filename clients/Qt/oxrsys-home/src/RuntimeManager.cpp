// SPDX-License-Identifier: MPL-2.0

#include "RuntimeManager.h"

#include "ServerConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

RuntimeManager::RuntimeManager(HomePaths paths)
    : paths_(std::move(paths))
{
}

const HomePaths& RuntimeManager::paths() const
{
    return paths_;
}

RuntimeRegistrationStatus RuntimeManager::registrationStatus() const
{
    RuntimeRegistrationStatus status;
    const QFileInfo active(paths_.activeRuntimePath);
    status.activeRuntimeExists = active.exists() || !active.symLinkTarget().isEmpty();
    if (status.activeRuntimeExists)
    {
        status.activeRuntimeTarget = active.symLinkTarget();
        if (status.activeRuntimeTarget.isEmpty())
        {
            status.activeRuntimeTarget = active.absoluteFilePath();
        }
    }
    return status;
}

RuntimeInstallStatus RuntimeManager::installStatus() const
{
    RuntimeInstallStatus status;
    status.bundledRuntimePath = bundledRuntimeDirectory();
    status.installedManifestPath = paths_.installedRuntimeManifestPath;

    const QString bundledLibraryPath =
        QDir(status.bundledRuntimePath).filePath(runtimeLibraryFileName());
    const QString installedLibraryPath =
        QDir(paths_.installedRuntimeDirectory).filePath(runtimeLibraryFileName());

    status.bundledRuntimeExists = QFileInfo(bundledLibraryPath).isFile();
    status.installedRuntimeExists = QFileInfo(installedLibraryPath).isFile();
    status.installedManifestExists = QFileInfo(paths_.installedRuntimeManifestPath).isFile();
    status.installedRuntimeNeedsUpdate =
        status.bundledRuntimeExists &&
        status.installedRuntimeExists &&
        !filesHaveSameContents(bundledLibraryPath, installedLibraryPath);
    return status;
}

QString RuntimeManager::activeRuntimeTarget() const
{
    return registrationStatus().activeRuntimeTarget;
}

QString RuntimeManager::activeLaunchRuntimeManifestPath(const QString& selectedManifestPath) const
{
    const RuntimeInstallStatus status = installStatus();
    if (status.installedRuntimeExists && status.installedManifestExists)
    {
        return status.installedManifestPath;
    }
    if (QFileInfo(selectedManifestPath).isFile())
    {
        return QFileInfo(selectedManifestPath).absoluteFilePath();
    }
    return defaultRuntimeManifestPath();
}

bool RuntimeManager::registerRuntimeManifest(const QString& manifestPath, QString* errorMessage) const
{
    if (!supportsRuntimeInstallAndRegistration())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("OpenXR runtime registration is not implemented for %1 in the Qt Home yet.")
                                .arg(platformName());
        }
        return false;
    }

    const QFileInfo manifest(manifestPath);
    if (!manifest.isFile())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Runtime manifest does not exist: " + manifestPath;
        }
        return false;
    }

    QDir().mkpath(paths_.activeRuntimeDirectory);
    QFile::remove(paths_.activeRuntimePath);
    if (!QFile::link(manifest.absoluteFilePath(), paths_.activeRuntimePath) &&
        !QFile::copy(manifest.absoluteFilePath(), paths_.activeRuntimePath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to write " + paths_.activeRuntimePath;
        }
        return false;
    }
    return true;
}

bool RuntimeManager::unregisterRuntime(QString* errorMessage) const
{
    if (!supportsRuntimeInstallAndRegistration())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("OpenXR runtime registration is not implemented for %1 in the Qt Home yet.")
                                .arg(platformName());
        }
        return false;
    }

    if (!QFile::exists(paths_.activeRuntimePath))
    {
        return true;
    }
    if (!QFile::remove(paths_.activeRuntimePath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to remove " + paths_.activeRuntimePath;
        }
        return false;
    }
    return true;
}

bool RuntimeManager::installBundledRuntime(QString* installedManifestPath, QString* errorMessage) const
{
    if (!supportsRuntimeInstallAndRegistration())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Runtime installation is not implemented for %1 in the Qt Home yet.")
                                .arg(platformName());
        }
        return false;
    }

    const QString sourceDirectory = bundledRuntimeDirectory();
    const QString sourceLibrary = QDir(sourceDirectory).filePath(runtimeLibraryFileName());
    const QString sourceConfig = QDir(sourceDirectory).filePath("oxrsys-runtime.toml");
    const QString installedLibrary =
        QDir(paths_.installedRuntimeDirectory).filePath(runtimeLibraryFileName());
    const QString installedConfig =
        QDir(paths_.installedRuntimeDirectory).filePath("oxrsys-runtime.toml");

    if (!QFileInfo(sourceLibrary).isFile())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Bundled runtime library not found at " + sourceLibrary;
        }
        return false;
    }

    QDir().mkpath(paths_.installedRuntimeDirectory);
    if (!replaceFile(sourceLibrary, installedLibrary, errorMessage))
    {
        return false;
    }

    if (QFileInfo(sourceConfig).isFile())
    {
        if (!replaceFile(sourceConfig, installedConfig, errorMessage))
        {
            return false;
        }
        if (!QFileInfo(paths_.configFilePath).exists())
        {
            QDir().mkpath(QFileInfo(paths_.configFilePath).absolutePath());
            QFile::copy(sourceConfig, paths_.configFilePath);
        }
    }
    else if (!QFileInfo(paths_.configFilePath).exists())
    {
        QDir().mkpath(QFileInfo(paths_.configFilePath).absolutePath());
        QFile file(paths_.configFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            file.write(ServerConfig::defaultText().toUtf8());
        }
    }

    QFile manifest(paths_.installedRuntimeManifestPath);
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to write " + paths_.installedRuntimeManifestPath;
        }
        return false;
    }
    manifest.write(runtimeManifestJson(installedLibrary).toUtf8());
    if (installedManifestPath != nullptr)
    {
        *installedManifestPath = paths_.installedRuntimeManifestPath;
    }
    return true;
}

QString RuntimeManager::runtimeManifestJson(const QString& libraryPath)
{
    const QJsonDocument document(QJsonObject{
        {"file_format_version", "1.0.0"},
        {"runtime", QJsonObject{
            {"name", "OXRSys Runtime"},
            {"library_path", QFileInfo(libraryPath).absoluteFilePath()},
        }},
    });
    return QString::fromUtf8(document.toJson(QJsonDocument::Indented));
}

QString RuntimeManager::bundledRuntimeDirectory() const
{
    const QString defaultDirectory = defaultRuntimeDirectoryPath();
    if (QFileInfo(QDir(defaultDirectory).filePath(runtimeLibraryFileName())).isFile())
    {
        return defaultDirectory;
    }

    const QString appResourceDirectory =
        QDir(QCoreApplication::applicationDirPath()).filePath("OXRSysRuntime");
    if (QFileInfo(QDir(appResourceDirectory).filePath(runtimeLibraryFileName())).isFile())
    {
        return appResourceDirectory;
    }
    return defaultDirectory;
}

bool RuntimeManager::replaceFile(const QString& source,
                                 const QString& destination,
                                 QString* errorMessage) const
{
    QFile::remove(destination);
    if (!QFile::copy(source, destination))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Failed to copy %1 to %2").arg(source, destination);
        }
        return false;
    }
    QFile::setPermissions(destination, QFile::permissions(source));
    return true;
}

bool RuntimeManager::filesHaveSameContents(const QString& lhs, const QString& rhs) const
{
    QFile lhsFile(lhs);
    QFile rhsFile(rhs);
    if (!lhsFile.open(QIODevice::ReadOnly) || !rhsFile.open(QIODevice::ReadOnly))
    {
        return false;
    }
    return lhsFile.readAll() == rhsFile.readAll();
}
