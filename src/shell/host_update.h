#pragma once
// 宿主内置更新服务：联网/下载/校验在本体，文件替换交给 xrkit_updater。
#include <QString>
#include <QStringList>

#include <cstddef>
#include <functional>

namespace xrkit::shell {

struct AppUpdateInfo {
    QString version;
    QString package;      // 兼容旧清单：主地址
    QString sha256;
    QStringList packages;  // 多源：交给 DownloadManager 探测/换源；每个都校验同一个 sha256
};

// 更新下载的持久状态：zip 与 pending.json 放在用户本地目录，跨进程可续传。
struct AppUpdatePending {
    AppUpdateInfo info;
    QString zip;
    bool ready = false;   // zip 已下载且 sha 通过；false=有 .part.N/.segments 可续传
};

using AppUpdateProgress = std::function<bool(std::size_t bytes, std::size_t total)>;

class HostUpdate {
public:
    static bool networkAllowed(QString* reason = nullptr);
    static bool fetchAppInfo(AppUpdateInfo* info, QString* error);

    // 整个“检测更新”功能的总开关（[update] enabled；0=不自动检查、不下载、不安装）。
    static bool updateCheckingEnabled();
    static void setUpdateCheckingEnabled(bool enabled);

    // 读取/下载/安装持久化的更新包。安装成功后调用方退出进程，由 xrkit_updater 替换文件。
    static bool pendingAppUpdate(AppUpdatePending* out);
    static bool downloadAppUpdate(const AppUpdateInfo& info, const AppUpdateProgress& progress,
                                  QString* error);
    static bool installPendingAppUpdate(bool* started, QString* error);
    // x.y.z + 测试构建 n：正式版高于同 x.y.z 测试版；测试版比较 n。
    static bool versionIsNewer(const QString& candidate, const QString& current);
    static bool isTestVersion(const QString& version);
    // 正式版不接受测试版候选；测试版接受测试版/正式版。
    static bool upgradeAllowed(const QString& candidate, const QString& current);
    // 下载 + sha256 + 解压 + 启动 updater；成功时调用方应退出当前进程。
    static bool applyAppUpdate(const AppUpdateInfo& info, QString* error);
};

}  // namespace xrkit::shell
