// SPDX-License-Identifier: MPL-2.0

#include "AdbBridge.h"

#include "PlatformSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace
{

QString adbExecutableName()
{
#if defined(Q_OS_WIN)
    return "adb.exe";
#else
    return "adb";
#endif
}

void appendUnique(QStringList& paths, const QString& path)
{
    if (!path.isEmpty() && !paths.contains(path))
    {
        paths.append(path);
    }
}

bool runAdb(const QStringList& arguments, QString* output, QString* errorMessage)
{
    const QString adb = AdbBridge::resolveExecutablePath();
    if (adb.isEmpty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "adb was not found in Android SDK paths or PATH.";
        }
        return false;
    }

    QProcess process;
    process.start(adb, arguments);
    if (!process.waitForFinished(10000))
    {
        process.kill();
        if (errorMessage != nullptr)
        {
            *errorMessage = "adb timed out.";
        }
        return false;
    }

    const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = stderrText.isEmpty()
                ? QString("adb exited with code %1.").arg(process.exitCode())
                : stderrText;
        }
        return false;
    }

    if (output != nullptr)
    {
        *output = stdoutText;
    }
    return true;
}

} // namespace

bool AdbDevice::isUsable() const
{
    return state == "device";
}

QString AdbDevice::displayName() const
{
    if (details.isEmpty())
    {
        return QString("%1 (%2)").arg(serial, state);
    }
    return QString("%1 (%2) %3").arg(serial, state, details);
}

bool AdbStatus::isAvailable() const
{
    return !executablePath.isEmpty();
}

QList<int> AdbBridge::reversePorts()
{
    return {9944, 9945, 9946};
}

QStringList AdbBridge::candidatePaths()
{
    QStringList paths;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString executable = adbExecutableName();

    const auto appendAndroidSdk = [&](const QString& root) {
        if (!root.isEmpty())
        {
            appendUnique(paths, QDir(root).filePath(QString("platform-tools/%1").arg(executable)));
        }
    };

    appendAndroidSdk(env.value("ANDROID_HOME"));
    appendAndroidSdk(env.value("ANDROID_SDK_ROOT"));

#if defined(Q_OS_MACOS)
    appendAndroidSdk(QDir(QDir::homePath()).filePath("Library/Android/sdk"));
    appendUnique(paths, "/opt/homebrew/bin/adb");
    appendUnique(paths, "/usr/local/bin/adb");
#elif defined(Q_OS_WIN)
    appendAndroidSdk(env.value("LOCALAPPDATA").isEmpty()
                         ? QString()
                         : QDir(env.value("LOCALAPPDATA")).filePath("Android/Sdk"));
#else
    appendUnique(paths, "/usr/bin/adb");
    appendUnique(paths, "/usr/local/bin/adb");
#endif

    for (const QString& pathEntry : env.value("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts))
    {
        appendUnique(paths, QDir(pathEntry).filePath(executable));
    }

    return paths;
}

QString AdbBridge::resolveExecutablePath()
{
    for (const QString& candidate : candidatePaths())
    {
        if (isExecutableFile(candidate))
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

AdbStatus AdbBridge::status()
{
    const QString adb = resolveExecutablePath();
    if (adb.isEmpty())
    {
        const QString paths = candidatePaths().join(", ");
#if defined(Q_OS_MACOS)
        return {QString(),
                QString("ADB is required for USB mode. Install adb-enhanced with Homebrew: "
                        "brew install adb-enhanced. Checked: %1.")
                    .arg(paths)};
#elif defined(Q_OS_WIN)
        return {QString(),
                QString("ADB is required for USB mode. Install Android Platform Tools "
                        "or add adb.exe to PATH. Checked: %1.")
                    .arg(paths)};
#else
        return {QString(),
                QString("ADB is required for USB mode. Install Android Platform Tools "
                        "or make sure adb is in PATH. Checked: %1.")
                    .arg(paths)};
#endif
    }
    return {adb, QString("ADB found at %1.").arg(adb)};
}

QList<AdbDevice> AdbBridge::parseDevices(const QString& output)
{
    QList<AdbDevice> devices;
    const QStringList lines = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i)
    {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() < 2)
        {
            continue;
        }
        const QString details = parts.mid(2).join(' ');
        devices.append({parts.at(0), parts.at(1), details});
    }
    return devices;
}

QSet<int> AdbBridge::parseReversePorts(const QString& output)
{
    QSet<int> ports;
    const QStringList lines = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    for (const QString& line : lines)
    {
        for (int port : reversePorts())
        {
            if (line.contains(QString("tcp:%1 tcp:%1").arg(port)))
            {
                ports.insert(port);
            }
        }
    }
    return ports;
}

QList<AdbDevice> AdbBridge::devices(QString* errorMessage)
{
    QString output;
    if (!runAdb({"devices", "-l"}, &output, errorMessage))
    {
        return {};
    }
    return parseDevices(output);
}

QSet<int> AdbBridge::reverseMappings(const QString& serial, QString* errorMessage)
{
    QString output;
    if (!runAdb({"-s", serial, "reverse", "--list"}, &output, errorMessage))
    {
        return {};
    }
    return parseReversePorts(output);
}

QSet<int> AdbBridge::configureReverse(const QString& serial, QString* errorMessage)
{
    for (int port : reversePorts())
    {
        QString ignoredOutput;
        QString ignoredError;
        runAdb({"-s", serial, "reverse", "--remove", QString("tcp:%1").arg(port)},
               &ignoredOutput,
               &ignoredError);
    }

    for (int port : reversePorts())
    {
        QString output;
        if (!runAdb({"-s", serial, "reverse", QString("tcp:%1").arg(port),
                     QString("tcp:%1").arg(port)},
                    &output,
                    errorMessage))
        {
            return {};
        }
    }

    const QSet<int> configuredPorts = reverseMappings(serial, errorMessage);
    for (int port : reversePorts())
    {
        if (!configuredPorts.contains(port))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = QString("adb reverse did not report port %1.").arg(port);
            }
            return configuredPorts;
        }
    }
    return configuredPorts;
}
