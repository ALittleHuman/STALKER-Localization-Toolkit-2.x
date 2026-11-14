#include "host_update.h"

#include "xrkit/core/http.h"
#include "xrkit/core/sha256.h"
#include "xrkit/core/version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <chrono>
#include <algorithm>
#include <future>
#include <thread>
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

constexpr const char* kApiDirect =
    "https://api.github.com/repos/ALittleHuman/"
    "STALKER-Localization-Toolkit-2.x/releases?per_page=100";
constexpr const char* kProxyPrefix = "https://gh-proxy.com/";
constexpr const char* kApiProxy =
    "https://gh-proxy.com/https://api.github.com/repos/ALittleHuman/"
    "STALKER-Localization-Toolkit-2.x/releases?per_page=100";

QStringList manifest_sources() {
    const QString override_url = qEnvironmentVariable("XRKIT_APP_UPDATE_URL");
    if (!override_url.isEmpty()) {
        QStringList out;
        for (const QString& item : override_url.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            out << item.trimmed();
        }
        return out;
    }
    return {QString::fromLatin1(kApiDirect), QString::fromLatin1(kApiProxy)};
}

QStringList package_mirrors(const QString& url) {
    QStringList out;
    if (url.isEmpty()) return out;
    out << url;
    if (url.startsWith(QStringLiteral("https://github.com/")) ||
        url.startsWith(QStringLiteral("https://raw.githubusercontent.com/"))) {
        out << QString::fromLatin1(kProxyPrefix) + url;
    }
    out.removeDuplicates();
    return out;
}

bool parse_update_payload(const std::string& body, bool allow_test, AppUpdateInfo* info,
                          QString* error) {
    QJsonParseError parse_error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(body), &parse_error);
    if (doc.isNull()) {
        if (error != nullptr) *error = QStringLiteral("更新源不是 JSON");
        return false;
    }
    const auto fill_release = [](const QJsonObject& obj, AppUpdateInfo* out) {
        out->version = obj.value(QStringLiteral("tag_name")).toString().trimmed();
        for (const QJsonValue& value : obj.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) continue;
            const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
            if (!url.isEmpty()) out->packages << package_mirrors(url);
            if (out->sha256.isEmpty()) {
                const QString digest = asset.value(QStringLiteral("digest")).toString();
                if (digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
                    out->sha256 = digest.mid(7).trimmed().toLower();
                }
            }
        }
        out->packages.removeDuplicates();
        if (!out->packages.isEmpty()) out->package = out->packages.first();
        return !out->version.isEmpty() && !out->packages.isEmpty();
    };

    if (doc.isArray()) {
        bool found = false;
        AppUpdateInfo best;
        for (const QJsonValue& value : doc.array()) {
            const QJsonObject obj = value.toObject();
            if (obj.value(QStringLiteral("draft")).toBool()) continue;
            if (!allow_test && obj.value(QStringLiteral("prerelease")).toBool()) continue;
            AppUpdateInfo candidate;
            if (!fill_release(obj, &candidate)) continue;
            // A1：prerelease 字段不够。测试版若按普通 Release 发布，prerelease=false，
            // 必须再按版本号本身判断，否则稳定版会被推测试版。
            if (!allow_test && HostUpdate::isTestVersion(candidate.version)) continue;
            if (!found || HostUpdate::versionIsNewer(candidate.version, best.version)) {
                best = std::move(candidate);
                found = true;
            }
        }
        if (!found) {
            if (error != nullptr) *error = QStringLiteral("没有当前通道可用的 Release");
            return false;
        }
        *info = std::move(best);
        return true;
    }

    if (!doc.isObject()) {
        if (error != nullptr) *error = QStringLiteral("更新源不是 JSON 对象/数组");
        return false;
    }
    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("assets")) ||
        obj.contains(QStringLiteral("tag_name"))) {
        if (!allow_test && obj.value(QStringLiteral("prerelease")).toBool()) {
            if (error != nullptr) *error = QStringLiteral("只有测试版 Release");
            return false;
        }
        if (!fill_release(obj, info)) {
            if (error != nullptr) *error = QStringLiteral("Release 缺 version/安装包地址");
            return false;
        }
        if (!allow_test && HostUpdate::isTestVersion(info->version)) {
            if (error != nullptr) *error = QStringLiteral("当前正式通道不接受测试版");
            return false;
        }
        return true;
    }
    // 旧清单兜底（测试注入 XRKIT_APP_UPDATE_URL 仍可用）
    info->version = obj.value(QStringLiteral("version")).toString().trimmed();
    const QString single = obj.value(QStringLiteral("package")).toString().trimmed();
    if (!single.isEmpty()) info->packages << package_mirrors(single);
    for (const QJsonValue& value : obj.value(QStringLiteral("packages")).toArray()) {
        const QString url = value.isString()
                                ? value.toString()
                                : value.toObject().value(QStringLiteral("url")).toString();
        if (!url.isEmpty()) info->packages << package_mirrors(url);
    }
    info->sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    info->packages.removeDuplicates();
    if (!info->packages.isEmpty()) info->package = info->packages.first();
    if (info->version.isEmpty() || info->packages.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("更新源缺 version/安装包地址");
        return false;
    }
    return true;
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

bool HostUpdate::networkAllowed(QString* reason) {
    if (xrkit::core::network_enabled()) return true;
    if (reason != nullptr) *reason = QStringLiteral("联网功能已关闭");
    return false;
}

namespace {
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool test = false;
    int build = 0;
    bool valid = false;
};

SemVer parse_semver(const QString& text) {
    SemVer v;
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) s.remove(0, 1);
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(.*)$"),
                           QRegularExpression::CaseInsensitiveOption)
            .match(s);
    if (!m.hasMatch()) return v;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).toInt();
    v.patch = m.captured(3).toInt();
    v.valid = true;
    QString rest = m.captured(4).trimmed().toLower();
    if (rest.isEmpty()) return v;
    while (!rest.isEmpty() &&
           (rest[0] == QLatin1Char('.') || rest[0] == QLatin1Char('-') ||
            rest[0] == QLatin1Char('_') || rest[0] == QLatin1Char(' '))) {
        rest.remove(0, 1);
    }
    // x.y.z.n
    if (QRegularExpression(QStringLiteral("^\\d+$")).match(rest).hasMatch()) {
        v.test = true;
        v.build = rest.toInt();
        return v;
    }
    // alpha.n / beta.n / rc.n（含 beta244 / rc-1）
    const QRegularExpressionMatch pre =
        QRegularExpression(QStringLiteral("^(alpha|beta|rc)[._-]?(\\d+)$"),
                           QRegularExpression::CaseInsensitiveOption)
            .match(rest);
    if (pre.hasMatch()) {
        v.test = true;
        v.build = pre.captured(2).toInt();
        return v;
    }
    // hotfix / build-245 等未知后缀：不能当正式版，否则比较会静默错。
    v.valid = false;
    return v;
}
}  // namespace

bool HostUpdate::isTestVersion(const QString& version) {
    const SemVer v = parse_semver(version);
    return v.valid && v.test;
}

bool HostUpdate::upgradeAllowed(const QString& candidate, const QString& current) {
    if (isTestVersion(current)) return true;
    return !isTestVersion(candidate);
}

bool HostUpdate::versionIsNewer(const QString& candidate, const QString& current) {
    const SemVer a = parse_semver(candidate);
    const SemVer b = parse_semver(current);
    if (!a.valid || !b.valid) return false;
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    if (a.patch != b.patch) return a.patch > b.patch;
    if (a.test != b.test) return !a.test;  // 同 x.y.z：正式版高于测试版
    if (!a.test) return false;             // 都是同一正式版
    return a.build > b.build;              // 都是测试版：n 大者新
}

bool HostUpdate::fetchAppInfo(AppUpdateInfo* info, QString* error) {
    if (info == nullptr) return false;
    QString why;
    if (!networkAllowed(&why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    try {
    *info = AppUpdateInfo{};
    const bool allow_test = isTestVersion(QString::fromStdString(xrkit::core::version_string()));
    const QStringList sources = manifest_sources();
    std::vector<std::future<xrkit::core::HttpResult>> futures;
    futures.reserve(static_cast<std::size_t>(sources.size()));
    for (const QString& source : sources) {
        futures.push_back(std::async(std::launch::async, [source] {
            return xrkit::core::http_get(source.toStdString(), 20, 1u << 20);
        }));
    }
    std::vector<bool> consumed(futures.size(), false);
    // 谁先成功用谁（并行探测 = 最快的源先返回），其余请求留到 future 析构时收尾。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(22);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_done = true;
        for (std::size_t index = 0; index < futures.size(); ++index) {
            if (consumed[index]) continue;
            if (futures[index].wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready) {
                all_done = false;
                continue;
            }
            consumed[index] = true;
            const xrkit::core::HttpResult result = futures[index].get();
            if (!result.ok) continue;
            AppUpdateInfo parsed;
            QString parse_error;
            if (parse_update_payload(result.body, allow_test, &parsed, &parse_error)) {
                *info = std::move(parsed);
                return true;
            }
        }
        if (all_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (error != nullptr) *error = QStringLiteral("所有更新源都不可达或返回无效");
    return false;
    } catch (const std::exception& e) {
        // std::async 线程创建失败等：检查更新失败是正常结果，不能让启动崩。
        if (error != nullptr) *error = QStringLiteral("检查更新异常：") + QString::fromUtf8(e.what());
        return false;
    }
}

bool HostUpdate::applyAppUpdate(const AppUpdateInfo& info, QString* error) {
    QString why;
    if (!networkAllowed(&why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    QStringList candidates = info.packages;
    if (candidates.isEmpty() && !info.package.isEmpty()) candidates << info.package;
    candidates.removeDuplicates();
    if (candidates.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("没有可用的安装包地址");
        return false;
    }
    // 多源测速：每个源只取 1 字节测首包延迟，选最快的；失败再按延迟顺序回退。
    struct Candidate {
        QString url;
        qint64 ms = 0;
    };
    std::vector<Candidate> ranked;
    for (const QString& url : candidates) {
        QElapsedTimer timer;
        timer.start();
        const xrkit::core::HttpResult probe =
            xrkit::core::http_get(url.toStdString(), 8, 1);
        if (probe.ok && (probe.status == 200 || probe.status == 206)) {
            ranked.push_back({url, timer.elapsed()});
        }
    }
    if (ranked.empty()) {
        if (error != nullptr) *error = QStringLiteral("所有安装包镜像都不可达");
        return false;
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Candidate& a, const Candidate& b) { return a.ms < b.ms; });

    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir root(QDir::temp().filePath(QStringLiteral("xrkit_update")));
    root.mkpath(QStringLiteral("app-%1").arg(stamp));
    const QString work = root.filePath(QStringLiteral("app-%1").arg(stamp));
    const QString zip = QDir(work).filePath(QStringLiteral("update.zip"));
    bool downloaded_ok = false;
    QString last_error;
    for (const Candidate& candidate : ranked) {
        const xrkit::core::HttpResult downloaded =
            xrkit::core::http_download(candidate.url.toStdString(), zip.toStdString(), 300,
                                       256u << 20);
        if (!downloaded.ok) {
            last_error = QString::fromStdString(downloaded.error);
            continue;
        }
        if (!info.sha256.isEmpty()) {
            std::string sha_error;
            const std::string actual = xrkit::core::sha256_file(zip.toStdString(), sha_error);
            if (actual != info.sha256.toStdString()) {
                last_error = QStringLiteral("sha256 不匹配");
                QFile::remove(zip);
                QFile::remove(zip + QStringLiteral(".part"));
                continue;
            }
        }
        downloaded_ok = true;
        break;
    }
    if (!downloaded_ok) {
        if (error != nullptr) *error = last_error.isEmpty() ? QStringLiteral("下载失败")
                                                           : last_error;
        return false;
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
