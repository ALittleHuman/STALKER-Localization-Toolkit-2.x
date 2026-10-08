#pragma once
// xrkit::shell —— **把注册表里的 context 命令追加进右键菜单**（P13-2a-2）。
//
// 口径（1.x `attach_plugin_context_menu` / `PLUGINS.md` §3.3）：**「原有项 → 分隔符 → 插件项」**，
//   · 精确匹配 `(host, location="context")`；
//   · `when` 用**当次**的 context 判定（不是注册时冻结的死条件）；
//   · 一条都没有 ⇒ **连分隔符都不加**（1.x「无注册即无痕迹」）；
//   · 点下去走 `Registry::invoke`（插件回调抛异常记进 registration_errors，绝不冒到 Qt 事件循环）。
//
// ★ 本文件是「同类东西只维护一套」的落实：六页（`PageBase`）与**插件页门面**的右键菜单共用它。

#include <QMenu>
#include <QObject>

#include "xrkit/engine/plugin/registry.h"

namespace xrkit::shell {

int append_registry_context_items(QMenu* menu, xrkit::plugin::Registry* registry,
                                 const std::string& host_page_id,
                                 const xrkit::plugin::Context& context, QObject* owner);

}  // namespace xrkit::shell
