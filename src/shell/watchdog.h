#pragma once
// xrkit 外壳 UI 看门狗（RC2 后）。
// UI 线程 30 秒没有心跳 ⇒ 写日志并退出进程，避免插件/引擎死循环让宿主永久卡死。
// 这是 fail-fast，不是热恢复；长活必须放 task_run worker。
namespace xrkit::shell {
void start_ui_watchdog();
void ui_heartbeat();
}  // namespace xrkit::shell