#pragma once
// 宿主内置更新服务：联网/下载/校验在本体，文件替换交给 xrkit_updater。
#include <QString>
#include <QStringList>

namespace xrkit::shell {

struct AppUpdateInfo {
    QString version;
    QString package;      // 兼容旧清单：主地址
    QString sha256;
    QStringList packages;  // 多源：按探测结果选择最快；每个都校验同一个 sha256
};

class HostUpdate {
public:
    static bool networkAllowed(QString* reason = nullptr);
    static bool fetchAppInfo(AppUpdateInfo* info, QString* error);
    // 下载 + sha256 + 解压 + 启动 updater；成功时调用方应退出当前进程。
    static bool applyAppUpdate(const AppUpdateInfo& info, QString* error);
};

}  // namespace xrkit::shell
