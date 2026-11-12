#pragma once
// 宿主内置更新服务：联网/下载/校验在本体，文件替换交给 xrkit_updater。
#include <QString>

namespace xrkit::shell {

struct AppUpdateInfo {
    QString version;
    QString package;
    QString sha256;
};

class HostUpdate {
public:
    static bool networkAllowed(QString* reason = nullptr);
    static bool fetchAppInfo(AppUpdateInfo* info, QString* error);
    // 下载 + sha256 + 解压 + 启动 updater；成功时调用方应退出当前进程。
    static bool applyAppUpdate(const AppUpdateInfo& info, QString* error);
};

}  // namespace xrkit::shell
