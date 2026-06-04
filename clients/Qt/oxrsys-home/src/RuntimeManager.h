// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "PlatformSupport.h"

#include <QString>

struct RuntimeRegistrationStatus
{
    bool activeRuntimeExists = false;
    QString activeRuntimeTarget;
};

class RuntimeManager
{
public:
    explicit RuntimeManager(HomePaths paths = homePaths());

    const HomePaths& paths() const;
    RuntimeRegistrationStatus registrationStatus() const;
    QString activeRuntimeTarget() const;
    QString activeLaunchRuntimeManifestPath(const QString& selectedManifestPath) const;

    bool registerRuntimeManifest(const QString& manifestPath, QString* errorMessage) const;
    bool unregisterRuntime(QString* errorMessage) const;

private:
    HomePaths paths_;
};
