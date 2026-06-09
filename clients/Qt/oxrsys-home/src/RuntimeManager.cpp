// SPDX-License-Identifier: MPL-2.0

#include "RuntimeManager.h"

#include "ServerConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <string>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shellapi.h>
#include <windows.h>
#endif

namespace
{

constexpr const char* WindowsPreviousRuntimeKey = "windowsPreviousActiveRuntime";
constexpr const char* WindowsRegisteredRuntimeKey = "windowsRegisteredRuntimeManifest";

bool sameRuntimePath(const QString& lhs, const QString& rhs)
{
    return normalizedPath(lhs).compare(normalizedPath(rhs), Qt::CaseInsensitive) == 0;
}

#if defined(Q_OS_WIN)
constexpr const wchar_t* OpenXrRegistryKey = L"SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const wchar_t* ActiveRuntimeValueName = L"ActiveRuntime";

QString fromWideString(const wchar_t* value, DWORD byteCount)
{
    if (value == nullptr || byteCount < sizeof(wchar_t))
    {
        return {};
    }
    qsizetype charCount = static_cast<qsizetype>(byteCount / sizeof(wchar_t));
    if (charCount > 0 && value[charCount - 1] == L'\0')
    {
        charCount--;
    }
    return QString::fromWCharArray(value, charCount);
}

QString readWindowsActiveRuntime()
{
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                OpenXrRegistryKey,
                                0,
                                KEY_READ | KEY_WOW64_64KEY,
                                &key);
    if (result != ERROR_SUCCESS)
    {
        return {};
    }

    DWORD type = 0;
    DWORD byteCount = 0;
    result = RegQueryValueExW(key, ActiveRuntimeValueName, nullptr, &type, nullptr, &byteCount);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || byteCount == 0)
    {
        RegCloseKey(key);
        return {};
    }

    std::wstring buffer(byteCount / sizeof(wchar_t) + 1, L'\0');
    result = RegQueryValueExW(key, ActiveRuntimeValueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buffer.data()), &byteCount);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        return {};
    }
    return fromWideString(buffer.data(), byteCount);
}

bool writeWindowsActiveRuntime(const QString& manifestPath, QString* errorMessage)
{
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                  OpenXrRegistryKey,
                                  0,
                                  nullptr,
                                  REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr,
                                  &key,
                                  nullptr);
    if (result != ERROR_SUCCESS)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Failed to open HKLM OpenXR runtime registry key: 0x%1")
                                .arg(result, 0, 16);
        }
        return false;
    }

    const std::wstring value = QDir::toNativeSeparators(manifestPath).toStdWString();
    result = RegSetValueExW(key,
                            ActiveRuntimeValueName,
                            0,
                            REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Failed to write HKLM OpenXR ActiveRuntime: 0x%1")
                                .arg(result, 0, 16);
        }
        return false;
    }
    return true;
}

bool clearWindowsActiveRuntime(QString* errorMessage)
{
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                OpenXrRegistryKey,
                                0,
                                KEY_SET_VALUE | KEY_WOW64_64KEY,
                                &key);
    if (result != ERROR_SUCCESS)
    {
        return true;
    }

    result = RegDeleteValueW(key, ActiveRuntimeValueName);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Failed to clear HKLM OpenXR ActiveRuntime: 0x%1")
                                .arg(result, 0, 16);
        }
        return false;
    }
    return true;
}

bool isWindowsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token,
                                        TokenElevation,
                                        &elevation,
                                        sizeof(elevation),
                                        &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

QString quoteWindowsArgument(QString value)
{
    value.replace("\"", "\\\"");
    return QString("\"%1\"").arg(value);
}

bool runElevatedWindowsCommand(const QStringList& arguments, QString* errorMessage)
{
    QStringList quoted;
    for (const QString& argument : arguments)
    {
        quoted.append(quoteWindowsArgument(argument));
    }
    const std::wstring executable = QCoreApplication::applicationFilePath().toStdWString();
    const std::wstring parameters = quoted.join(' ').toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Windows UAC registration command was cancelled or failed.";
        }
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (exitCode != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("Elevated Windows registration command exited with code %1.")
                                .arg(exitCode);
        }
        return false;
    }
    return true;
}
#endif

} // namespace

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
#if defined(Q_OS_WIN)
    status.activeRuntimeTarget = readWindowsActiveRuntime();
    status.activeRuntimeExists = !status.activeRuntimeTarget.isEmpty();
#else
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
#endif
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

QString RuntimeManager::activeLaunchRuntimeManifestPath(const QString& selectedManifestPath,
                                                        bool preferInstalledRuntime) const
{
    const RuntimeInstallStatus status = installStatus();
    const QFileInfo selected(selectedManifestPath);
    if (!preferInstalledRuntime && selected.isFile())
    {
        return selected.absoluteFilePath();
    }
    if (status.installedRuntimeExists && status.installedManifestExists)
    {
        return status.installedManifestPath;
    }
    if (selected.isFile())
    {
        return selected.absoluteFilePath();
    }
    return defaultRuntimeManifestPath();
}

bool RuntimeManager::registerRuntimeManifest(const QString& manifestPath, QString* errorMessage) const
{
    if (!supportsRuntimeGlobalRegistration())
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

#if defined(Q_OS_WIN)
    const QString manifestTarget = manifest.absoluteFilePath();
    const QString currentTarget = readWindowsActiveRuntime();
    QSettings settings("OXRSys", "HomeQt");
    const QString registeredTarget = settings.value(WindowsRegisteredRuntimeKey).toString();
    if (!currentTarget.isEmpty() && !sameRuntimePath(currentTarget, manifestTarget))
    {
        if (registeredTarget.isEmpty() || !sameRuntimePath(currentTarget, registeredTarget))
        {
            settings.setValue(WindowsPreviousRuntimeKey, currentTarget);
        }
    }
    else if (currentTarget.isEmpty())
    {
        settings.remove(WindowsPreviousRuntimeKey);
    }
    settings.sync();

    const bool registered = isWindowsProcessElevated()
        ? writeWindowsActiveRuntime(manifestTarget, errorMessage)
        : runElevatedWindowsCommand({"--oxrsys-register-runtime", manifestTarget}, errorMessage);
    if (registered)
    {
        settings.setValue(WindowsRegisteredRuntimeKey, manifestTarget);
        settings.sync();
    }
    return registered;
#else
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
#endif
}

bool RuntimeManager::unregisterRuntime(QString* errorMessage) const
{
    if (!supportsRuntimeGlobalRegistration())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("OpenXR runtime registration is not implemented for %1 in the Qt Home yet.")
                                .arg(platformName());
        }
        return false;
    }

#if defined(Q_OS_WIN)
    QSettings settings("OXRSys", "HomeQt");
    const QString expectedTarget =
        settings.value(WindowsRegisteredRuntimeKey, paths_.installedRuntimeManifestPath).toString();
    const QString previousTarget = settings.value(WindowsPreviousRuntimeKey).toString();
    const QString currentTarget = readWindowsActiveRuntime();
    if (currentTarget.isEmpty())
    {
        settings.remove(WindowsRegisteredRuntimeKey);
        settings.remove(WindowsPreviousRuntimeKey);
        return true;
    }
    if (!expectedTarget.isEmpty() && !sameRuntimePath(currentTarget, expectedTarget))
    {
        return true;
    }

    const bool unregistered = isWindowsProcessElevated()
        ? (previousTarget.isEmpty()
               ? clearWindowsActiveRuntime(errorMessage)
               : writeWindowsActiveRuntime(previousTarget, errorMessage))
        : runElevatedWindowsCommand(
              {"--oxrsys-unregister-runtime", expectedTarget, previousTarget}, errorMessage);
    if (unregistered)
    {
        settings.remove(WindowsRegisteredRuntimeKey);
        settings.remove(WindowsPreviousRuntimeKey);
        settings.sync();
    }
    return unregistered;
#else
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
#endif
}

bool RuntimeManager::installBundledRuntime(QString* installedManifestPath, QString* errorMessage) const
{
    if (!supportsRuntimeInstall())
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

int RuntimeManager::handleElevatedWindowsCommand(const QStringList& arguments, QString* errorMessage)
{
#if defined(Q_OS_WIN)
    if (arguments.size() < 2)
    {
        return -1;
    }
    const QString command = arguments.at(1);
    if (command == "--oxrsys-register-runtime")
    {
        if (arguments.size() < 3)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Missing runtime manifest path for elevated registration.";
            }
            return 1;
        }
        return writeWindowsActiveRuntime(arguments.at(2), errorMessage) ? 0 : 1;
    }
    if (command == "--oxrsys-unregister-runtime")
    {
        if (arguments.size() < 3)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Missing expected runtime manifest path for elevated unregistration.";
            }
            return 1;
        }
        const QString expectedTarget = arguments.at(2);
        const QString previousTarget = arguments.value(3);
        const QString currentTarget = readWindowsActiveRuntime();
        if (!currentTarget.isEmpty() && !expectedTarget.isEmpty() &&
            !sameRuntimePath(currentTarget, expectedTarget))
        {
            return 0;
        }
        if (previousTarget.isEmpty())
        {
            return clearWindowsActiveRuntime(errorMessage) ? 0 : 1;
        }
        return writeWindowsActiveRuntime(previousTarget, errorMessage) ? 0 : 1;
    }
#else
    Q_UNUSED(arguments);
    Q_UNUSED(errorMessage);
#endif
    return -1;
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
