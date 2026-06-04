// SPDX-License-Identifier: MPL-2.0

#include "Launcher.h"

#include "PlatformSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <algorithm>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#endif

namespace
{

QString normalizedKey(const QString& path)
{
    return normalizedPath(path).toLower();
}

QString appNameFromExecutable(const QString& path)
{
    QFileInfo info(path);
    QString name = info.completeBaseName();
    if (name.isEmpty())
    {
        name = info.fileName();
    }
    return name;
}

QString stripDesktopFieldCodes(const QString& value)
{
    QString output = value;
    output.replace(QRegularExpression("%[fFuUdDnNickvm]"), "");
    output.replace("%%", "%");
    return output.simplified();
}

QMap<QString, QString> parseDesktopFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }

    QMap<QString, QString> values;
    bool inDesktopEntry = false;
    while (!file.atEnd())
    {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
        {
            continue;
        }
        if (line.startsWith('[') && line.endsWith(']'))
        {
            inDesktopEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inDesktopEntry)
        {
            continue;
        }
        const int equalsIndex = line.indexOf('=');
        if (equalsIndex < 0)
        {
            continue;
        }
        values.insert(line.left(equalsIndex), line.mid(equalsIndex + 1));
    }
    return values;
}

LauncherApp inspectDesktopFile(const QString& path, const QString& source, bool allowUnknown)
{
    const QMap<QString, QString> values = parseDesktopFile(path);
    if (values.value("Type") != "Application" || values.value("NoDisplay").toLower() == "true")
    {
        return {};
    }

    const QString execLine = stripDesktopFieldCodes(values.value("Exec"));
    const QStringList tokens = splitCommandLine(execLine);
    if (tokens.isEmpty())
    {
        return {};
    }

    const QString executablePath = resolveExecutableFromPath(tokens.first());
    if (executablePath.isEmpty())
    {
        return {};
    }

    LauncherApp app;
    app.name = values.value("Name", appNameFromExecutable(executablePath));
    app.path = QFileInfo(path).absoluteFilePath();
    app.executablePath = executablePath;
    app.kind = launcherKindForMetadata(app.name, values.value("StartupWMClass"), path, executablePath);
    if (!allowUnknown && app.kind == "custom")
    {
        return {};
    }
    app.source = source;
    app.lastSeen = QDateTime::currentDateTimeUtc();
    return app;
}

QString plistStringValue(const QString& text, const QString& key)
{
    const QRegularExpression regex(
        QStringLiteral("<key>\\s*%1\\s*</key>\\s*<string>([^<]+)</string>")
            .arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch match = regex.match(text);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

LauncherApp inspectMacAppBundle(const QString& path, const QString& source, bool allowUnknown)
{
    const QFileInfo bundleInfo(path);
    if (!bundleInfo.isDir() || bundleInfo.suffix().toLower() != "app")
    {
        return {};
    }

    const QString infoPath = QDir(bundleInfo.absoluteFilePath()).filePath("Contents/Info.plist");
    QFile file(infoPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }
    const QString plist = QString::fromUtf8(file.readAll());
    const QString executableName = plistStringValue(plist, "CFBundleExecutable");
    if (executableName.isEmpty())
    {
        return {};
    }

    const QString executablePath =
        QDir(bundleInfo.absoluteFilePath()).filePath(QString("Contents/MacOS/%1").arg(executableName));
    if (!isExecutableFile(executablePath))
    {
        return {};
    }

    LauncherApp app;
    app.name = plistStringValue(plist, "CFBundleDisplayName");
    if (app.name.isEmpty())
    {
        app.name = plistStringValue(plist, "CFBundleName");
    }
    if (app.name.isEmpty())
    {
        app.name = bundleInfo.completeBaseName();
    }
    const QString identifier = plistStringValue(plist, "CFBundleIdentifier");
    app.path = bundleInfo.absoluteFilePath();
    app.executablePath = executablePath;
    app.kind = launcherKindForMetadata(app.name, identifier, app.path, executablePath);
    if (!allowUnknown && app.kind == "custom")
    {
        return {};
    }
    app.source = source;
    app.lastSeen = QDateTime::currentDateTimeUtc();
    return app;
}

LauncherApp inspectExecutable(const QString& path, const QString& source, bool allowUnknown)
{
    if (!isExecutableFile(path))
    {
        return {};
    }

    LauncherApp app;
    app.name = appNameFromExecutable(path);
    app.path = QFileInfo(path).absoluteFilePath();
    app.executablePath = app.path;
    app.kind = launcherKindForMetadata(app.name, QString(), app.path, app.executablePath);
    if (!allowUnknown && app.kind == "custom")
    {
        return {};
    }
    app.source = source;
    app.lastSeen = QDateTime::currentDateTimeUtc();
    return app;
}

#if defined(Q_OS_WIN)
QString windowsShortcutTarget(const QString& path)
{
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
    {
        return {};
    }

    IShellLinkW* shellLink = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW,
                                      reinterpret_cast<void**>(&shellLink));
    if (FAILED(result) || shellLink == nullptr)
    {
        if (shouldUninitialize)
        {
            CoUninitialize();
        }
        return {};
    }

    IPersistFile* persistFile = nullptr;
    result = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
    if (FAILED(result) || persistFile == nullptr)
    {
        shellLink->Release();
        if (shouldUninitialize)
        {
            CoUninitialize();
        }
        return {};
    }

    const std::wstring shortcutPath = QDir::toNativeSeparators(path).toStdWString();
    result = persistFile->Load(shortcutPath.c_str(), STGM_READ);
    QString target;
    if (SUCCEEDED(result))
    {
        wchar_t targetPath[MAX_PATH] = {};
        WIN32_FIND_DATAW findData = {};
        result = shellLink->GetPath(targetPath, MAX_PATH, &findData, SLGP_UNCPRIORITY);
        if (SUCCEEDED(result) && targetPath[0] != L'\0')
        {
            target = QString::fromWCharArray(targetPath);
        }
    }

    persistFile->Release();
    shellLink->Release();
    if (shouldUninitialize)
    {
        CoUninitialize();
    }
    return target;
}

LauncherApp inspectWindowsShortcut(const QString& path, const QString& source, bool allowUnknown)
{
    const QString target = windowsShortcutTarget(path);
    if (target.isEmpty() || !isExecutableFile(target))
    {
        return {};
    }

    LauncherApp app;
    app.name = QFileInfo(path).completeBaseName();
    app.path = QFileInfo(path).absoluteFilePath();
    app.executablePath = QFileInfo(target).absoluteFilePath();
    app.kind = launcherKindForMetadata(app.name, QString(), app.path, app.executablePath);
    if (!allowUnknown && app.kind == "custom")
    {
        return {};
    }
    app.source = source;
    app.lastSeen = QDateTime::currentDateTimeUtc();
    return app;
}

QList<LauncherApp> scanWindowsStartMenu()
{
    QList<LauncherApp> apps;
    QStringList roots;
    const QString programData = qEnvironmentVariable("ProgramData");
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!programData.isEmpty())
    {
        roots << QDir(programData).filePath("Microsoft/Windows/Start Menu/Programs");
    }
    if (!appData.isEmpty())
    {
        roots << QDir(appData).filePath("Microsoft/Windows/Start Menu/Programs");
    }

    for (const QString& root : roots)
    {
        QDirIterator iterator(root, {"*.lnk"}, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext())
        {
            const LauncherApp app = inspectWindowsShortcut(iterator.next(), "automatic", false);
            if (!app.name.isEmpty())
            {
                apps.append(app);
            }
        }
    }
    return apps;
}
#endif

QList<LauncherApp> scanDesktopFiles()
{
    QList<LauncherApp> apps;
    QStringList roots;
    roots << "/usr/share/applications";
    roots << "/usr/local/share/applications";
    roots << QDir(homePaths().dataRoot).filePath("applications");
    roots << QDir(QDir::homePath()).filePath(".local/share/applications");
    roots << "/var/lib/flatpak/exports/share/applications";

    for (const QString& root : roots)
    {
        const QDir directory(root);
        const QFileInfoList entries = directory.entryInfoList({"*.desktop"}, QDir::Files);
        for (const QFileInfo& entry : entries)
        {
            const LauncherApp app = inspectDesktopFile(entry.absoluteFilePath(), "automatic", false);
            if (!app.name.isEmpty())
            {
                apps.append(app);
            }
        }
    }
    return apps;
}

QList<LauncherApp> scanMacApps()
{
    QList<LauncherApp> apps;
    const QStringList roots = {
        "/Applications",
        QDir(QDir::homePath()).filePath("Applications"),
    };
    for (const QString& root : roots)
    {
        const QDir directory(root);
        const QFileInfoList entries = directory.entryInfoList({"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries)
        {
            const LauncherApp app = inspectMacAppBundle(entry.absoluteFilePath(), "automatic", false);
            if (!app.name.isEmpty())
            {
                apps.append(app);
            }
        }
    }

    const QDir unityRoot("/Applications/Unity/Hub/Editor");
    const QFileInfoList versions = unityRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& version : versions)
    {
        const LauncherApp app = inspectMacAppBundle(
            QDir(version.absoluteFilePath()).filePath("Unity.app"),
            "automatic",
            false);
        if (!app.name.isEmpty())
        {
            apps.append(app);
        }
    }
    return apps;
}

bool launcherLessThan(const LauncherApp& lhs, const LauncherApp& rhs)
{
    if (lhs.source != rhs.source)
    {
        return lhs.source == "manual";
    }
    if (lhs.kind != rhs.kind)
    {
        return lhs.kindDisplayName().localeAwareCompare(rhs.kindDisplayName()) < 0;
    }
    return lhs.name.localeAwareCompare(rhs.name) < 0;
}

} // namespace

QString LauncherApp::id() const
{
    return normalizedKey(path);
}

QString LauncherApp::kindDisplayName() const
{
    if (kind == "godot")
    {
        return "Godot";
    }
    if (kind == "unity")
    {
        return "Unity";
    }
    return "Custom";
}

QString LauncherApp::sourceDisplayName() const
{
    return source == "automatic" ? "Detected" : "Manual";
}

QJsonObject LauncherApp::toJson() const
{
    return {
        {"name", name},
        {"path", path},
        {"executablePath", executablePath},
        {"kind", kind},
        {"source", source},
        {"lastSeen", lastSeen.toUTC().toString(Qt::ISODateWithMs)},
    };
}

LauncherApp LauncherApp::fromJson(const QJsonObject& object)
{
    LauncherApp app;
    app.name = object.value("name").toString();
    app.path = object.value("path").toString();
    app.executablePath = object.value("executablePath").toString();
    app.kind = object.value("kind").toString("custom");
    app.source = object.value("source").toString("manual");
    app.lastSeen = QDateTime::fromString(object.value("lastSeen").toString(), Qt::ISODateWithMs);
    if (!app.lastSeen.isValid())
    {
        app.lastSeen = QDateTime::currentDateTimeUtc();
    }
    return app;
}

LauncherStore LauncherStore::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
    {
        return {};
    }

    LauncherStore store;
    const QJsonObject object = document.object();
    for (const QJsonValue& value : object.value("manualApps").toArray())
    {
        if (value.isObject())
        {
            const LauncherApp app = LauncherApp::fromJson(value.toObject());
            if (!app.name.isEmpty() && !app.path.isEmpty())
            {
                store.manualApps.append(app);
            }
        }
    }
    for (const QJsonValue& value : object.value("hiddenAutomaticAppPaths").toArray())
    {
        const QString pathValue = value.toString();
        if (!pathValue.isEmpty())
        {
            store.hiddenAutomaticAppPaths.append(pathValue);
        }
    }
    return store;
}

void LauncherStore::save(const QString& path) const
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QJsonArray manualAppsArray;
    for (const LauncherApp& app : manualApps)
    {
        manualAppsArray.append(app.toJson());
    }

    QJsonArray hiddenArray;
    for (const QString& hiddenPath : hiddenAutomaticAppPaths)
    {
        hiddenArray.append(hiddenPath);
    }

    QJsonDocument document(QJsonObject{
        {"manualApps", manualAppsArray},
        {"hiddenAutomaticAppPaths", hiddenArray},
    });

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }
    file.write(document.toJson(QJsonDocument::Indented));
}

LauncherApp LauncherScanner::inspectPath(const QString& path,
                                         const QString& source,
                                         bool allowUnknown)
{
    const QFileInfo info(path);
    if (info.suffix().toLower() == "desktop")
    {
        return inspectDesktopFile(path, source, allowUnknown);
    }
    if (info.suffix().toLower() == "app")
    {
        return inspectMacAppBundle(path, source, allowUnknown);
    }
#if defined(Q_OS_WIN)
    if (info.suffix().toLower() == "lnk")
    {
        return inspectWindowsShortcut(path, source, allowUnknown);
    }
#endif
    return inspectExecutable(path, source, allowUnknown);
}

QList<LauncherApp> LauncherScanner::scanAutomaticApps()
{
#if defined(Q_OS_MACOS)
    return scanMacApps();
#elif defined(Q_OS_WIN)
    return scanWindowsStartMenu();
#else
    return scanDesktopFiles();
#endif
}

QList<LauncherApp> LauncherScanner::merge(const QList<LauncherApp>& automaticApps,
                                          const LauncherStore& store)
{
    const QSet<QString> hiddenPaths = [&store]() {
        QSet<QString> values;
        for (const QString& path : store.hiddenAutomaticAppPaths)
        {
            values.insert(normalizedKey(path));
        }
        return values;
    }();

    QMap<QString, LauncherApp> appsByPath;
    for (const LauncherApp& app : automaticApps)
    {
        if (!hiddenPaths.contains(normalizedKey(app.path)))
        {
            appsByPath.insert(normalizedKey(app.path), app);
        }
    }
    for (LauncherApp app : store.manualApps)
    {
        app.source = "manual";
        appsByPath.insert(normalizedKey(app.path), app);
    }

    QList<LauncherApp> apps = appsByPath.values();
    std::sort(apps.begin(), apps.end(), launcherLessThan);
    return apps;
}

QString launcherKindForMetadata(const QString& name,
                                const QString& identifier,
                                const QString& path,
                                const QString& executablePath)
{
    const QString lowerName = name.toLower();
    const QString lowerIdentifier = identifier.toLower();
    const QString lowerPath = path.toLower();
    const QString lowerExecutable = QFileInfo(executablePath).fileName().toLower();

    if (lowerIdentifier.contains("godotengine.godot") ||
        lowerName.contains("godot") ||
        lowerExecutable.contains("godot"))
    {
        return "godot";
    }
    if (lowerIdentifier.contains("unity") ||
        lowerName == "unity" ||
        lowerExecutable == "unity" ||
        lowerPath.contains("/unity/hub/editor/"))
    {
        return "unity";
    }
    return "custom";
}

QString terminalSafeName(const QString& value)
{
    QString output;
    for (const QChar ch : value)
    {
        if (ch.isLetterOrNumber() || ch == '-' || ch == '_')
        {
            output.append(ch);
        }
        else
        {
            output.append('_');
        }
    }
    while (output.startsWith('_'))
    {
        output.remove(0, 1);
    }
    while (output.endsWith('_'))
    {
        output.chop(1);
    }
    return output.isEmpty() ? "XR-App" : output;
}

QString terminalLaunchScript(const LauncherApp& app, const QString& runtimeManifestPath)
{
    const QString workingDirectory = QFileInfo(app.executablePath).absolutePath();
    return QString(
        "#!/bin/sh\n"
        "cd %1 || exit 1\n"
        "export XR_RUNTIME_JSON=%2\n"
        "exec %3\n")
        .arg(shellQuoted(workingDirectory),
             shellQuoted(runtimeManifestPath),
             shellQuoted(app.executablePath));
}
