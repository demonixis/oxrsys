// SPDX-License-Identifier: MPL-2.0

#include "AdbBridge.h"

#include "PlatformSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTcpSocket>

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

QString cleanedPath(QString path)
{
    path = path.trimmed();
    if (path.size() >= 2)
    {
        const QChar first = path.front();
        const QChar last = path.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
        {
            path = path.mid(1, path.size() - 2).trimmed();
        }
    }
    return path;
}

QByteArray adbServerReadExact(QTcpSocket& socket, qsizetype size, QString* errorMessage)
{
    QByteArray data;
    while (data.size() < size)
    {
        if (!socket.waitForReadyRead(1000) && socket.bytesAvailable() <= 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = socket.errorString();
            }
            return {};
        }
        data += socket.read(size - data.size());
    }
    return data;
}

bool adbServerReadPayload(QTcpSocket& socket, QString* output, QString* errorMessage)
{
    const QByteArray lengthBytes = adbServerReadExact(socket, 4, errorMessage);
    if (lengthBytes.size() != 4)
    {
        return false;
    }
    bool ok = false;
    const int length = QString::fromLatin1(lengthBytes).toInt(&ok, 16);
    if (!ok || length < 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ADB server returned an invalid payload length.";
        }
        return false;
    }
    const QByteArray payload = adbServerReadExact(socket, length, errorMessage);
    if (payload.size() != length)
    {
        return false;
    }
    if (output != nullptr)
    {
        *output = QString::fromUtf8(payload).trimmed();
    }
    return true;
}

bool adbServerRequest(const QString& service,
                      bool expectsPayload,
                      QString* output,
                      QString* errorMessage)
{
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 5037);
    if (!socket.waitForConnected(750))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ADB server is not available on 127.0.0.1:5037.";
        }
        return false;
    }

    const QByteArray serviceBytes = service.toUtf8();
    const QByteArray command =
        QByteArray::number(serviceBytes.size(), 16).rightJustified(4, '0') + serviceBytes;
    if (socket.write(command) != command.size() || !socket.waitForBytesWritten(1000))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = socket.errorString();
        }
        return false;
    }

    const QByteArray status = adbServerReadExact(socket, 4, errorMessage);
    if (status == "FAIL")
    {
        QString failure;
        adbServerReadPayload(socket, &failure, nullptr);
        if (errorMessage != nullptr)
        {
            *errorMessage = failure.isEmpty()
                ? QString("ADB server rejected %1.").arg(service)
                : failure;
        }
        return false;
    }
    if (status != "OKAY")
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ADB server returned an unexpected status.";
        }
        return false;
    }

    if (!expectsPayload)
    {
        if (output != nullptr)
        {
            output->clear();
        }
        return true;
    }
    return adbServerReadPayload(socket, output, errorMessage);
}

bool adbServerAvailable()
{
    QString output;
    QString error;
    return adbServerRequest("host:version", true, &output, &error);
}

bool useAdbServer(const QString& customPath)
{
    return cleanedPath(customPath).isEmpty();
}

bool runProcess(const QString& program,
                const QStringList& arguments,
                int timeoutMs,
                QString* output,
                QString* errorMessage)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(timeoutMs))
    {
        process.kill();
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("%1 timed out.").arg(QFileInfo(program).fileName());
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
                ? QString("%1 exited with code %2.")
                      .arg(QFileInfo(program).fileName())
                      .arg(process.exitCode())
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

bool runAdb(const QStringList& arguments,
            QString* output,
            QString* errorMessage,
            const QString& customPath)
{
    const QString adb = AdbBridge::resolveExecutablePath(customPath);
    if (adb.isEmpty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = customPath.trimmed().isEmpty()
                ? "adb was not found in Android SDK paths or PATH."
                : "custom adb path is invalid or unavailable.";
        }
        return false;
    }

    return runProcess(adb, arguments, 10000, output, errorMessage);
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
    return {9944, 9945, 9946, 9948};
}

QStringList AdbBridge::candidatePaths(const QString& customPath)
{
    QStringList paths;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString executable = adbExecutableName();

    appendUnique(paths, cleanedPath(customPath));

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

QString AdbBridge::resolveExecutablePath(const QString& customPath)
{
    const QString custom = cleanedPath(customPath);
    if (!custom.isEmpty())
    {
        return validateExecutable(custom) ? QFileInfo(custom).absoluteFilePath() : QString();
    }

    for (const QString& candidate : candidatePaths())
    {
        if (isExecutableFile(candidate))
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

bool AdbBridge::validateExecutable(const QString& path, QString* errorMessage)
{
    const QString cleanPath = cleanedPath(path);
    if (!isExecutableFile(cleanPath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "file is not executable";
        }
        return false;
    }

    QString ignoredOutput;
    return runProcess(cleanPath, {"version"}, 5000, &ignoredOutput, errorMessage);
}

AdbStatus AdbBridge::status(const QString& customPath)
{
    const QString custom = cleanedPath(customPath);
    if (!custom.isEmpty())
    {
        QString error;
        if (!validateExecutable(custom, &error))
        {
            return {QString(),
                    QString("Custom ADB path is invalid: %1. Select an executable adb that "
                            "passes `adb version`, or clear the custom path to auto-detect.%2")
                        .arg(custom,
                             error.isEmpty() ? QString() : QString(" %1").arg(error))};
        }
        return {QFileInfo(custom).absoluteFilePath(),
                QString("Custom ADB found at %1.").arg(QFileInfo(custom).absoluteFilePath())};
    }

    if (adbServerAvailable())
    {
        return {"adb-server://127.0.0.1:5037",
                "ADB server found on 127.0.0.1:5037; no adb executable is required for USB reverse setup."};
    }

    const QString adb = resolveExecutablePath();
    if (adb.isEmpty())
    {
        const QString paths = candidatePaths().join(", ");
#if defined(Q_OS_MACOS)
        return {QString(),
                QString("No ADB server was available on 127.0.0.1:5037. Qt Home needs an external "
                        "adb fallback on macOS; install adb-enhanced with Homebrew: "
                        "brew install adb-enhanced. Checked executables: %1.")
                    .arg(paths)};
#elif defined(Q_OS_WIN)
        return {QString(),
                QString("No ADB server was available on 127.0.0.1:5037. Install Android Platform "
                        "Tools or add adb.exe to PATH. Checked executables: %1.")
                    .arg(paths)};
#else
        return {QString(),
                QString("No ADB server was available on 127.0.0.1:5037. Install Android Platform "
                        "Tools or make sure adb is in PATH. Checked executables: %1.")
                    .arg(paths)};
#endif
    }
    return {adb, QString("ADB found at %1.").arg(adb)};
}

QList<AdbDevice> AdbBridge::parseDevices(const QString& output)
{
    QList<AdbDevice> devices;
    const QStringList lines = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    const int firstDeviceLine =
        !lines.isEmpty() && lines.first().contains("List of devices", Qt::CaseInsensitive) ? 1 : 0;
    for (int i = firstDeviceLine; i < lines.size(); ++i)
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

QList<AdbDevice> AdbBridge::devices(QString* errorMessage, const QString& customPath)
{
    if (useAdbServer(customPath))
    {
        QString output;
        QString serverError;
        if (adbServerRequest("host:devices-l", true, &output, &serverError))
        {
            return parseDevices(output);
        }
    }

    QString output;
    if (!runAdb({"devices", "-l"}, &output, errorMessage, customPath))
    {
        return {};
    }
    return parseDevices(output);
}

QSet<int> AdbBridge::reverseMappings(const QString& serial,
                                     QString* errorMessage,
                                     const QString& customPath)
{
    if (useAdbServer(customPath))
    {
        QString output;
        QString serverError;
        if (adbServerRequest(QString("host-serial:%1:reverse:list-forward").arg(serial),
                             true,
                             &output,
                             &serverError))
        {
            return parseReversePorts(output);
        }
    }

    QString output;
    if (!runAdb({"-s", serial, "reverse", "--list"}, &output, errorMessage, customPath))
    {
        return {};
    }
    return parseReversePorts(output);
}

QSet<int> AdbBridge::configureReverse(const QString& serial,
                                      QString* errorMessage,
                                      const QString& customPath)
{
    if (useAdbServer(customPath))
    {
        bool configuredWithServer = true;
        for (int port : reversePorts())
        {
            QString ignoredOutput;
            QString ignoredError;
            adbServerRequest(QString("host-serial:%1:reverse:killforward:tcp:%2")
                                 .arg(serial)
                                 .arg(port),
                             false,
                             &ignoredOutput,
                             &ignoredError);
        }
        for (int port : reversePorts())
        {
            QString output;
            QString serverError;
            if (!adbServerRequest(QString("host-serial:%1:reverse:forward:tcp:%2;tcp:%2")
                                      .arg(serial)
                                      .arg(port),
                                  false,
                                  &output,
                                  &serverError))
            {
                configuredWithServer = false;
                break;
            }
        }
        if (configuredWithServer)
        {
            const QSet<int> configuredPorts = reverseMappings(serial, errorMessage, customPath);
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
    }

    for (int port : reversePorts())
    {
        QString ignoredOutput;
        QString ignoredError;
        runAdb({"-s", serial, "reverse", "--remove", QString("tcp:%1").arg(port)},
               &ignoredOutput,
               &ignoredError,
               customPath);
    }

    for (int port : reversePorts())
    {
        QString output;
        if (!runAdb({"-s", serial, "reverse", QString("tcp:%1").arg(port),
                     QString("tcp:%1").arg(port)},
                    &output,
                    errorMessage,
                    customPath))
        {
            return {};
        }
    }

    const QSet<int> configuredPorts = reverseMappings(serial, errorMessage, customPath);
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
