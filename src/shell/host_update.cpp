#include "host_update.h"

#include "xrkit/core/http.h"
#include "xrkit/core/sha256.h"
#include "xrkit/core/version.h"
#include "xrkit/core/theme.h"

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
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
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
// 安装包镜像：直连 release-assets.githubusercontent.com 在部分网络下会卡 30s+；
// ghfast.top 实测可直连（1 MB 约 7s），放在 gh-proxy.com 前面。
constexpr const char* kAssetMirrors[] = {
    "https://ghfast.top/",
    "https://gh-proxy.com/",
};
// npm 更新包（Trusted Publishing 发布）；unpkg 实测 36 MB 约 4.6s。
constexpr const char* kNpmUpdatePackage = "@alittlehuman/stalker-toolkit-update";

QString npm_update_url(const QString& version) {
    QString v = version.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V'))) v.remove(0, 1);
    if (v.isEmpty()) return {};
    return QStringLiteral("https://unpkg.com/%1@%2/full/%3.zip")
        .arg(QString::fromLatin1(kNpmUpdatePackage), v, v);
}

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
    // 先试镜像，最后才直连：没开加速器时 GitHub 资产域名经常 30s 才返回 302。
    if (url.startsWith(QStringLiteral("https://github.com/")) ||
        url.startsWith(QStringLiteral("https://raw.githubusercontent.com/"))) {
        for (const char* prefix : kAssetMirrors) {
            out << QString::fromLatin1(prefix) + url;
        }
    }
    out << url;
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
        const QString npm_url = npm_update_url(out->version);
        if (!npm_url.isEmpty()) out->packages.prepend(npm_url);
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

// ── 持久下载状态（跨进程断点续传）────────────────────────────────────────────
// 目录固定，不能用 applyAppUpdate 那种时间戳目录：时间戳一变，.part 就找不到了。
QString update_root() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    return QDir(base).filePath(QStringLiteral("xrkit/update"));
}

QString pending_metadata_path() {
    return QDir(update_root()).filePath(QStringLiteral("pending.json"));
}

QString pending_backup_path() {
    return pending_metadata_path() + QStringLiteral(".bak");
}

QString pending_temp_path() {
    return pending_metadata_path() + QStringLiteral(".tmp");
}

// 读取一个候选槽（current/bak）。committed 允许旧文件缺省（迁移兼容）。
bool read_pending_file(const QString& path, AppUpdatePending* out, int* generation) {
    if (out == nullptr) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    out->info.version = obj.value(QStringLiteral("version")).toString();
    out->info.package = obj.value(QStringLiteral("package")).toString();
    out->info.sha256 = obj.value(QStringLiteral("sha256")).toString().toLower();
    for (const QJsonValue& value : obj.value(QStringLiteral("packages")).toArray()) {
        const QString url = value.toString();
        if (!url.isEmpty()) out->info.packages << url;
    }
    if (out->info.packages.isEmpty() && !out->info.package.isEmpty()) {
        out->info.packages << out->info.package;
    }
    if (generation != nullptr) *generation = obj.value(QStringLiteral("generation")).toInt(0);
    out->zip = QDir(update_root()).filePath(QStringLiteral("update.zip"));
    out->ready = obj.value(QStringLiteral("ready")).toBool() &&
                 QFileInfo(out->zip).isFile() && QFileInfo(out->zip).size() > 0;
    if (out->ready && !out->info.sha256.isEmpty()) {
        std::string sha_error;
        const std::string actual =
            xrkit::core::sha256_file(out->zip.toStdString(), sha_error);
        if (actual != out->info.sha256.toStdString()) out->ready = false;
    }
    return !out->info.version.isEmpty();
}

// current 无效时回退 bak；两代都保留，保证非正常退出后还有一份正确状态。
bool read_pending_metadata(AppUpdatePending* out) {
    if (out == nullptr) return false;
    AppUpdatePending current;
    AppUpdatePending backup;
    int current_gen = -1;
    int backup_gen = -1;
    const bool has_current = read_pending_file(pending_metadata_path(), &current, &current_gen);
    if (has_current && current_gen < 0) current_gen = 0;
    const bool has_backup = read_pending_file(pending_backup_path(), &backup, &backup_gen);
    if (has_backup && backup_gen < 0) backup_gen = 0;
    if (has_current && (!has_backup || current_gen >= backup_gen)) {
        *out = std::move(current);
        return true;
    }
    if (has_backup) {
        *out = std::move(backup);
        return true;
    }
    return false;
}

bool sync_file(QFile& file) {
    if (!file.flush()) return false;
#ifdef _WIN32
    const intptr_t fd = static_cast<intptr_t>(file.handle());
    if (fd >= 0) {
        const intptr_t os_handle = _get_osfhandle(static_cast<int>(fd));
        if (os_handle != -1) FlushFileBuffers(reinterpret_cast<HANDLE>(os_handle));
    }
#endif
    return true;
}

bool write_pending_metadata(const AppUpdateInfo& info, bool ready) {
    QDir().mkpath(update_root());
    AppUpdatePending old;
    int old_generation = 0;
    if (read_pending_file(pending_metadata_path(), &old, &old_generation)) {
        // current 有效，使用它的下一代号
    } else {
        AppUpdatePending backup;
        int backup_generation = 0;
        if (read_pending_file(pending_backup_path(), &backup, &backup_generation)) {
            old_generation = backup_generation;
        }
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("generation"), old_generation + 1);
    obj.insert(QStringLiteral("committed"), true);
    obj.insert(QStringLiteral("version"), info.version);
    obj.insert(QStringLiteral("package"), info.package);
    obj.insert(QStringLiteral("sha256"), info.sha256);
    QJsonArray urls;
    for (const QString& url : info.packages) urls.append(url);
    obj.insert(QStringLiteral("packages"), urls);
    obj.insert(QStringLiteral("ready"), ready);
    const QString temp = pending_temp_path();
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented)) < 0 || !sync_file(file)) {
        file.close();
        QFile::remove(temp);
        return false;
    }
    file.close();
    // 先写 tmp，再轮换。关键：current 不存在时不能先删 bak，否则崩溃会两份都没了。
    if (QFileInfo::exists(pending_metadata_path())) {
        QFile::remove(pending_backup_path());
        if (!QFile::rename(pending_metadata_path(), pending_backup_path())) {
            QFile::remove(temp);
            return false;
        }
    }
    if (!QFile::rename(temp, pending_metadata_path())) {
        // current 已被移到 bak；保留 bak，下次启动回退。
        return false;
    }
    return true;
}

void clear_pending_metadata() {
    QFile::remove(pending_temp_path());
    QFile::remove(pending_backup_path());
    QFile::remove(pending_metadata_path());
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
    // 顺序探测：先直连 GitHub API，失败再走代理源。
    // 不用并行 future：先成功的那一路返回后，析构仍会等慢的镜像（实测能多卡 80 秒）。
    for (const QString& source : sources) {
        const bool direct_first =
            source.startsWith(QStringLiteral("https://api.github.com/"));
        const xrkit::core::HttpResult result =
            xrkit::core::http_get(source.toStdString(), 20, 1u << 20, direct_first);
        if (!result.ok) continue;
        AppUpdateInfo parsed;
        QString parse_error;
        if (parse_update_payload(result.body, allow_test, &parsed, &parse_error)) {
            *info = std::move(parsed);
            return true;
        }
    }
    if (error != nullptr) *error = QStringLiteral("所有更新源都不可达或返回无效");
    return false;
    } catch (const std::exception& e) {
        // 后台线程/解析异常：检查更新失败是正常结果，不能让启动崩。
        if (error != nullptr) *error = QStringLiteral("检查更新异常：") + QString::fromUtf8(e.what());
        return false;
    }
}

bool HostUpdate::updateCheckingEnabled() {
    return xrkit::core::load_user_value("update", "enabled", "1") != "0";
}

void HostUpdate::setUpdateCheckingEnabled(bool enabled) {
    xrkit::core::save_user_value("update", "enabled", enabled ? "1" : "0");
}

bool HostUpdate::pendingAppUpdate(AppUpdatePending* out) {
    return read_pending_metadata(out);
}

bool HostUpdate::downloadAppUpdate(const AppUpdateInfo& info,
                                   const AppUpdateProgress& progress, QString* error) {
    if (info.version.isEmpty() || (info.packages.isEmpty() && info.package.isEmpty())) {
        if (error != nullptr) *error = QStringLiteral("更新信息缺版本或安装包地址");
        return false;
    }
    AppUpdateInfo normalized = info;
    if (normalized.packages.isEmpty()) normalized.packages << normalized.package;
    if (normalized.package.isEmpty()) normalized.package = normalized.packages.first();

    QDir().mkpath(update_root());
    const QString zip = QDir(update_root()).filePath(QStringLiteral("update.zip"));
    const QString part = zip + QStringLiteral(".part");
    AppUpdatePending old;
    const bool had_old = read_pending_metadata(&old);
    if (had_old && old.info.version != normalized.version) {
        QFile::remove(zip);
        QFile::remove(part);
    }
    // 没有任何有效元数据时，.part 无法证明属于哪个版本，不能盲目续传。
    if (!had_old) QFile::remove(part);
    // 已下完（或上次已校验）的同版本包：直接复用，避免重复下载。
    if (QFileInfo(zip).isFile() && QFileInfo(zip).size() > 0) {
        if (normalized.sha256.isEmpty()) {
            write_pending_metadata(normalized, true);
            return true;
        }
        std::string sha_error;
        const std::string actual = xrkit::core::sha256_file(zip.toStdString(), sha_error);
        if (actual == normalized.sha256.toStdString()) {
            write_pending_metadata(normalized, true);
            return true;
        }
        QFile::remove(zip);
        QFile::remove(part);
    }
    if (!write_pending_metadata(normalized, false)) {
        if (error != nullptr) *error = QStringLiteral("无法写入更新状态文件");
        return false;
    }
    QString last_error;
    for (const QString& url : normalized.packages) {
        const bool direct = url.startsWith(QStringLiteral("https://ghfast.top/")) ||
                            url.startsWith(QStringLiteral("https://gh-proxy.com/"));
        const xrkit::core::HttpResult result =
            xrkit::core::http_download(url.toStdString(), zip.toStdString(), 300,
                                       256u << 20, progress, direct);
        if (!result.ok) {
            last_error = QString::fromStdString(result.error);
            if (last_error == QStringLiteral("下载已取消")) break;
            continue;
        }
        if (!normalized.sha256.isEmpty()) {
            std::string sha_error;
            const std::string actual = xrkit::core::sha256_file(zip.toStdString(), sha_error);
            if (actual != normalized.sha256.toStdString()) {
                last_error = QStringLiteral("sha256 不匹配");
                QFile::remove(zip);
                QFile::remove(part);
                continue;
            }
        }
        write_pending_metadata(normalized, true);
        return true;
    }
    if (error != nullptr) {
        *error = last_error.isEmpty() ? QStringLiteral("下载失败") : last_error;
    }
    return false;
}

bool HostUpdate::installPendingAppUpdate(bool* started, QString* error) {
    if (started != nullptr) *started = false;
    AppUpdatePending pending;
    if (!read_pending_metadata(&pending) || !pending.ready) {
        if (error != nullptr) *error = QStringLiteral("没有已下载完成的更新包");
        return false;
    }
    QDir root(update_root());
    const QString stage = root.filePath(QStringLiteral("stage"));
    QDir(stage).removeRecursively();
    QDir().mkpath(stage);
    QProcess tar;
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), pending.zip, QStringLiteral("-C"), stage});
    if (!tar.waitForFinished(120000) || tar.exitCode() != 0) {
        QProcess powershell;
        powershell.start(QStringLiteral("powershell"),
                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                          QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                              .arg(pending.zip, stage)});
        if (!powershell.waitForFinished(120000) || powershell.exitCode() != 0) {
            if (error != nullptr) *error = QStringLiteral("更新包解压失败");
            return false;
        }
    }
    const QString app = find_file(stage, QStringLiteral("xrkit_app.exe"));
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
    const QString txn = root.filePath(QStringLiteral("transaction"));
    QDir(txn).removeRecursively();
    const QString exe = QDir(install_dir).filePath(QStringLiteral("xrkit_app.exe"));
    const bool launched = QProcess::startDetached(
        updater,
        {QStringLiteral("--pid"), QString::number(QCoreApplication::applicationPid()),
         QStringLiteral("--target"), install_dir, QStringLiteral("--staged"),
         QFileInfo(app).absolutePath(), QStringLiteral("--transaction"), txn,
         QStringLiteral("--exe"), exe},
        QDir::tempPath());
    if (!launched) {
        if (error != nullptr) *error = QStringLiteral("xrkit_updater 启动失败");
        return false;
    }
    // 更新器已接手：删 pending.json，避免新版本启动后又安装一次；stage/zip 留给更新器。
    clear_pending_metadata();
    if (started != nullptr) *started = true;
    return true;
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
