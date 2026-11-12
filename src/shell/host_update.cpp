#include "host_update.h"

#include "xrkit/core/http.h"
#include "xrkit/core/sha256.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xrkit::shell {
namespace {

bool startDetachedProcess(const QString& program, const QStringList& args,
                          const QString& workdir, QString* error) {
#ifdef _WIN32
    QString command = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(program));
    for (const QString& arg : args) {
        command += QStringLiteral(" \"%1\"").arg(QString(arg).replace(QLatin1Char('"'),
                                                                     QStringLiteral("\\\"")));
    }
    std::wstring cmd = command.toStdWString();
    std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
    buffer.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = QDir::toNativeSeparators(workdir).toStdWString();
    DWORD flags = CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(program.toStdWString().c_str(), buffer.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        // 某些 job 不允许 breakaway；退回不声明 job 行为的普通 detached。
        if (!CreateProcessW(program.toStdWString().c_str(), buffer.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr,
                            cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
            if (error != nullptr) *error = QStringLiteral("CreateProcess 失败");
            return false;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return QProcess::startDetached(program, args, workdir);
#endif
}

constexpr const char* kAppManifest =
    "https://raw.githubusercontent.com/ALittleHuman/"
    "STALKER-Localization-Toolkit-2.x-Network-Plugin/main/app_update.json";

QString version_key(const QString& text) {
    QStringList parts;
    for (const QString& part : text.split(QRegularExpression(QStringLiteral("[^0-9]+")),
                                          Qt::SkipEmptyParts)) {
        parts << part;
    }
    return parts.join(QLatin1Char('.'));
}

QString find_file(const QString& root, const QString& name) {
    QDir dir(root);
    const QFileInfoList hits =
        dir.entryInfoList({name}, QDir::Files, QDir::Name);
    if (!hits.isEmpty()) return hits.first().absoluteFilePath();
    for (const QFileInfo& sub :
         dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString found = find_file(sub.absoluteFilePath(), name);
        if (!found.isEmpty()) return found;
    }
    return {};
}

}  // namespace

QString manifest_url() {
    const QString override_url = qEnvironmentVariable("XRKIT_APP_UPDATE_URL");
    return override_url.isEmpty() ? QString::fromLatin1(kAppManifest) : override_url;
}

bool HostUpdate::networkAllowed(QString* reason) {
    if (xrkit::core::network_enabled()) return true;
    if (reason != nullptr) *reason = QStringLiteral("联网功能已关闭");
    return false;
}

bool HostUpdate::fetchAppInfo(AppUpdateInfo* info, QString* error) {
    if (info == nullptr) return false;
    QString why;
    if (!networkAllowed(&why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    const xrkit::core::HttpResult result = xrkit::core::http_get(manifest_url().toStdString(), 20,
                                                                1u << 20);
    if (!result.ok) {
        if (error != nullptr) *error = QString::fromStdString(result.error);
        return false;
    }
    QJsonParseError parse_error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(result.body), &parse_error);
    if (doc.isNull() || !doc.isObject()) {
        if (error != nullptr) *error = QStringLiteral("app_update.json 不是 JSON");
        return false;
    }
    const QJsonObject obj = doc.object();
    info->version = obj.value(QStringLiteral("version")).toString().trimmed();
    info->package = obj.value(QStringLiteral("package")).toString().trimmed();
    info->sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    if (!info->package.isEmpty() && !info->package.contains(QStringLiteral("://"))) {
        info->package = QUrl(manifest_url()).resolved(QUrl(info->package)).toString();
    }
    if (info->version.isEmpty() || info->package.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("app_update.json 缺 version/package");
        return false;
    }
    return true;
}

bool HostUpdate::applyAppUpdate(const AppUpdateInfo& info, QString* error) {
    QString why;
    if (!networkAllowed(&why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir root(QDir::temp().filePath(QStringLiteral("xrkit_update")));
    root.mkpath(QStringLiteral("app-%1").arg(stamp));
    const QString work = root.filePath(QStringLiteral("app-%1").arg(stamp));
    const QString zip = QDir(work).filePath(QStringLiteral("update.zip"));
    const xrkit::core::HttpResult downloaded =
        xrkit::core::http_download(info.package.toStdString(), zip.toStdString(), 300,
                                   256u << 20);
    if (!downloaded.ok) {
        if (error != nullptr) *error = QString::fromStdString(downloaded.error);
        return false;
    }
    if (!info.sha256.isEmpty()) {
        std::string sha_error;
        const std::string actual = xrkit::core::sha256_file(zip.toStdString(), sha_error);
        if (actual != info.sha256.toStdString()) {
            if (error != nullptr) *error = QStringLiteral("更新包 sha256 不匹配");
            return false;
        }
    }
    QDir stage(work);
    stage.mkpath(QStringLiteral("stage"));
    const QString staged = stage.filePath(QStringLiteral("stage"));
    QProcess tar;
    tar.start(QStringLiteral("tar"), {QStringLiteral("-xf"), zip, QStringLiteral("-C"), staged});
    if (!tar.waitForFinished(120000) || tar.exitCode() != 0) {
        QProcess powershell;
        powershell.start(QStringLiteral("powershell"),
                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                          QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                              .arg(zip, staged)});
        if (!powershell.waitForFinished(120000) || powershell.exitCode() != 0) {
            if (error != nullptr) *error = QStringLiteral("更新包解压失败");
            return false;
        }
    }
    const QString app = find_file(staged, QStringLiteral("xrkit_app.exe"));
    if (app.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("更新包内找不到 xrkit_app.exe");
        return false;
    }
    const QString install_dir = QCoreApplication::applicationDirPath();
    const QString updater = QDir(install_dir).filePath(QStringLiteral("xrkit_updater.exe"));
    if (!QFileInfo::exists(updater)) {
        if (error != nullptr) *error = QStringLiteral("找不到 xrkit_updater.exe");
        return false;
    }
    const QString txn = QDir(work).filePath(QStringLiteral("transaction"));
    const QString exe = QDir(install_dir).filePath(QStringLiteral("xrkit_app.exe"));
    const bool started = QProcess::startDetached(
        updater,
        {QStringLiteral("--pid"), QString::number(QCoreApplication::applicationPid()),
         QStringLiteral("--target"), install_dir, QStringLiteral("--staged"),
         QFileInfo(app).absolutePath(), QStringLiteral("--transaction"), txn,
         QStringLiteral("--exe"), exe},
        QDir::tempPath());
    if (!started) {
        if (error != nullptr) *error = QStringLiteral("xrkit_updater 启动失败");
        return false;
    }
    QCoreApplication::quit();
    return true;
}

}  // namespace xrkit::shell
