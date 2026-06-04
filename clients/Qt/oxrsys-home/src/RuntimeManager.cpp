// SPDX-License-Identifier: MPL-2.0

#include "RuntimeManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

QString RuntimeManager::activeRuntimeTarget() const
{
    return registrationStatus().activeRuntimeTarget;
}

QString RuntimeManager::activeLaunchRuntimeManifestPath(const QString& selectedManifestPath) const
{
    if (selectedManifestPath.trimmed().isEmpty())
    {
        return {};
    }
    const QFileInfo selected(selectedManifestPath);
    return selected.absoluteFilePath();
}

bool RuntimeManager::registerRuntimeManifest(const QString& manifestPath, QString* errorMessage) const
{
    if (!supportsRuntimeRegistration())
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

    if (!QDir().mkpath(paths_.activeRuntimeDirectory))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create " + paths_.activeRuntimeDirectory;
        }
        return false;
    }

    const QFileInfo active(paths_.activeRuntimePath);
    if ((active.exists() || !active.symLinkTarget().isEmpty()) &&
        !QFile::remove(paths_.activeRuntimePath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to replace " + paths_.activeRuntimePath;
        }
        return false;
    }

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
    if (!supportsRuntimeRegistration())
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
