// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

struct AdbDevice
{
    QString serial;
    QString state;
    QString details;

    bool isUsable() const;
    QString displayName() const;
};

struct AdbStatus
{
    QString executablePath;
    QString message;

    bool isAvailable() const;
};

class AdbBridge
{
public:
    static QList<int> reversePorts();
    static QStringList candidatePaths(const QString& customPath = {});
    static QString resolveExecutablePath(const QString& customPath = {});
    static AdbStatus status(const QString& customPath = {});
    static bool validateExecutable(const QString& path, QString* errorMessage = nullptr);
    static QList<AdbDevice> parseDevices(const QString& output);
    static QSet<int> parseReversePorts(const QString& output);

    static QList<AdbDevice> devices(QString* errorMessage = nullptr,
                                    const QString& customPath = {});
    static QSet<int> reverseMappings(const QString& serial,
                                     QString* errorMessage = nullptr,
                                     const QString& customPath = {});
    static QSet<int> configureReverse(const QString& serial,
                                      QString* errorMessage = nullptr,
                                      const QString& customPath = {});
};
