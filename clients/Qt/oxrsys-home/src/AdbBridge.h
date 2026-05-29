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
    static QStringList candidatePaths();
    static QString resolveExecutablePath();
    static AdbStatus status();
    static QList<AdbDevice> parseDevices(const QString& output);
    static QSet<int> parseReversePorts(const QString& output);

    static QList<AdbDevice> devices(QString* errorMessage = nullptr);
    static QSet<int> reverseMappings(const QString& serial, QString* errorMessage = nullptr);
    static QSet<int> configureReverse(const QString& serial, QString* errorMessage = nullptr);
};
