// SPDX-License-Identifier: MPL-2.0

#include "PlatformSupport.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

QString homeDirectory()
{
    const QString home = QDir::homePath();
    return home.isEmpty() ? QDir::currentPath() : home;
}

QString envValue(const QString& key)
{
    return QProcessEnvironment::systemEnvironment().value(key);
}

QString firstWritableStandardPath(QStandardPaths::StandardLocation location,
                                  const QString& fallback)
{
    const QString path = QStandardPaths::writableLocation(location);
    return path.isEmpty() ? fallback : path;
}

QString appSupportDirectory()
{
#if defined(Q_OS_MACOS)
    return QDir(homeDirectory()).filePath("Library/Application Support/OXRSys");
#elif defined(Q_OS_WIN)
    const QString appData = envValue("APPDATA");
    if (!appData.isEmpty())
    {
        return QDir(appData).filePath("OXRSys");
    }
    return firstWritableStandardPath(QStandardPaths::AppDataLocation,
                                     QDir(homeDirectory()).filePath("AppData/Roaming/OXRSys"));
#else
    const QString xdgConfig = envValue("XDG_CONFIG_HOME");
    if (!xdgConfig.isEmpty())
    {
        return QDir(xdgConfig).filePath("oxrsys");
    }
    return QDir(homeDirectory()).filePath(".config/oxrsys");
#endif
}

QString dataDirectory()
{
#if defined(Q_OS_MACOS)
    return appSupportDirectory();
#elif defined(Q_OS_WIN)
    const QString localAppData = envValue("LOCALAPPDATA");
    if (!localAppData.isEmpty())
    {
        return QDir(localAppData).filePath("OXRSys");
    }
    return firstWritableStandardPath(QStandardPaths::AppLocalDataLocation,
                                     appSupportDirectory());
#else
    const QString xdgData = envValue("XDG_DATA_HOME");
    if (!xdgData.isEmpty())
    {
        return QDir(xdgData).filePath("oxrsys");
    }
    return QDir(homeDirectory()).filePath(".local/share/oxrsys");
#endif
}

QString stateDirectory()
{
#if defined(Q_OS_MACOS)
    return appSupportDirectory();
#elif defined(Q_OS_WIN)
    return dataDirectory();
#else
    const QString xdgState = envValue("XDG_STATE_HOME");
    if (!xdgState.isEmpty())
    {
        return QDir(xdgState).filePath("oxrsys");
    }
    return QDir(homeDirectory()).filePath(".local/state/oxrsys");
#endif
}

QString openXrConfigDirectory()
{
#if defined(Q_OS_WIN)
    return QDir(appSupportDirectory()).filePath("openxr/1");
#else
    const QString xdgConfig = envValue("XDG_CONFIG_HOME");
    const QString configRoot = xdgConfig.isEmpty()
        ? QDir(homeDirectory()).filePath(".config")
        : xdgConfig;
    return QDir(configRoot).filePath("openxr/1");
#endif
}

QString cpuPresetSuffix()
{
    const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
    if (architecture.contains("arm64") || architecture.contains("aarch64"))
    {
        return "arm64";
    }
    return "x64";
}

QStringList runtimePresetCandidates()
{
#if defined(Q_OS_MACOS)
    return {QString("macos-%1").arg(cpuPresetSuffix()), "macos-universal", "macos-arm64", "macos-x64"};
#elif defined(Q_OS_WIN)
    return {QString("windows-%1").arg(cpuPresetSuffix()), "windows-x64"};
#else
    return {"linux-native"};
#endif
}

} // namespace

QString platformName()
{
#if defined(Q_OS_MACOS)
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return QSysInfo::prettyProductName();
#endif
}

QString runtimeLibraryFileName()
{
#if defined(Q_OS_MACOS)
    return "liboxrsys-runtime.dylib";
#elif defined(Q_OS_WIN)
    return "oxrsys-runtime.dll";
#else
    return "liboxrsys-runtime.so";
#endif
}

QString runtimeBuildPresetName()
{
    return runtimePresetCandidates().first();
}

QString runtimeManifestFileName()
{
    return "oxrsys-runtime.json";
}

QString simulatorExecutableName()
{
#if defined(Q_OS_WIN)
    return "oxrsys-simulator.exe";
#else
    return "oxrsys-simulator";
#endif
}

QString pathListSeparator()
{
    return QString(QDir::listSeparator());
}

QString normalizedPath(const QString& path)
{
    if (path.isEmpty())
    {
        return {};
    }
    return QFileInfo(path).absoluteFilePath();
}

QString shellQuoted(const QString& value)
{
#if defined(Q_OS_WIN)
    QString escaped = value;
    escaped.replace('"', "\\\"");
    return QString("\"%1\"").arg(escaped);
#else
    QString escaped = value;
    escaped.replace('\'', "'\\''");
    return QString("'%1'").arg(escaped);
#endif
}

QStringList splitCommandLine(const QString& commandLine)
{
    QStringList result;
    QString current;
    QChar quote;
    bool escaping = false;

    for (const QChar ch : commandLine)
    {
        if (escaping)
        {
            current.append(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\')
        {
            escaping = true;
            continue;
        }
        if (!quote.isNull())
        {
            if (ch == quote)
            {
                quote = QChar();
            }
            else
            {
                current.append(ch);
            }
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            continue;
        }
        if (ch.isSpace())
        {
            if (!current.isEmpty())
            {
                result.append(current);
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }

    if (escaping)
    {
        current.append('\\');
    }
    if (!current.isEmpty())
    {
        result.append(current);
    }
    return result;
}

QString resolveExecutableFromPath(const QString& executableName)
{
    if (executableName.isEmpty())
    {
        return {};
    }

    QFileInfo directInfo(executableName);
    if (directInfo.isAbsolute() || executableName.contains(QDir::separator()) ||
        executableName.contains('/'))
    {
        return isExecutableFile(executableName) ? directInfo.absoluteFilePath() : QString();
    }

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList pathEntries = env.value("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString& entry : pathEntries)
    {
        const QString candidate = QDir(entry).filePath(executableName);
        if (isExecutableFile(candidate))
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
#if defined(Q_OS_WIN)
        const QString exeCandidate = QDir(entry).filePath(executableName + ".exe");
        if (isExecutableFile(exeCandidate))
        {
            return QFileInfo(exeCandidate).absoluteFilePath();
        }
#endif
    }
    return {};
}

bool isExecutableFile(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.isExecutable();
}

bool isProcessRunning(qint64 processId)
{
    if (processId <= 0)
    {
        return false;
    }
#if defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(processId), 0) == 0)
    {
        return true;
    }
    return errno == EPERM;
#elif defined(Q_OS_WIN)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
    {
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return waitResult == WAIT_TIMEOUT;
#else
    Q_UNUSED(processId);
    return true;
#endif
}

bool revealInFileManager(const QString& path)
{
    if (path.isEmpty())
    {
        return false;
    }

    QFileInfo info(path);
    QString directory = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    QDir revealDirectory(directory);
    while (!revealDirectory.exists())
    {
        const QString before = revealDirectory.absolutePath();
        if (!revealDirectory.cdUp() || revealDirectory.absolutePath() == before)
        {
            return false;
        }
    }

    const QString target = info.exists()
        ? info.absoluteFilePath()
        : revealDirectory.absolutePath();

#if defined(Q_OS_MACOS)
    return QProcess::startDetached("open", {"-R", target});
#elif defined(Q_OS_WIN)
    return QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(target)});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(revealDirectory.absolutePath()));
#endif
}

HomePaths homePaths()
{
    HomePaths paths;
    paths.configRoot = appSupportDirectory();
    paths.dataRoot = dataDirectory();
    paths.stateRoot = stateDirectory();
    paths.activeRuntimeDirectory = openXrConfigDirectory();
    paths.activeRuntimePath = QDir(paths.activeRuntimeDirectory).filePath("active_runtime.json");
    paths.configFilePath = QDir(paths.configRoot).filePath("oxrsys-runtime.toml");
    paths.launcherAppsPath = QDir(paths.configRoot).filePath("launcher_apps.json");
    paths.runtimeStatusPath = QDir(paths.stateRoot).filePath("runtime_status.json");
    return paths;
}

QStringList runtimeBuildDirectoryCandidates()
{
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    candidates << QDir(appDir).filePath("../../../runtime");
    candidates << QDir(appDir).filePath("../../../../runtime");
    candidates << QDir(cwd).filePath("runtime");
    candidates << QDir(cwd).filePath("build/runtime");
    for (const QString& preset : runtimePresetCandidates())
    {
        const QString relative = QString("build/%1/runtime").arg(preset);
        candidates << QDir(cwd).filePath(relative);
    }
    candidates.removeDuplicates();
    return candidates;
}

QString defaultRuntimeManifestPath()
{
    for (const QString& directory : runtimeBuildDirectoryCandidates())
    {
        const QString candidate = QDir(directory).filePath(runtimeManifestFileName());
        if (QFileInfo(candidate).isFile())
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    const QString fallbackDirectory = runtimeBuildDirectoryCandidates().value(0);
    return QDir(fallbackDirectory).filePath(runtimeManifestFileName());
}

bool supportsRuntimeRegistration()
{
#if defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}
