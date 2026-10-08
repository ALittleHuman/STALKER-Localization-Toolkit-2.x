// xrkit::shell —— 注册表 context 项 → 右键菜单（唯一实现；口径见头文件）。
//
// ★ 这段逻辑**原来住在 `PageBase::append_plugin_context_items` 里**，P13-2a-2 抽到这里：
//   插件页的右键菜单（门面 `ui.context_menu`）也要把"其它插件注册的右键命令"列出来，
//   而"再抄一份"正是本仓最贵的一类返工（用户口径「同类东西只维护一套组件」）。
//   `PageBase` 那份现在只做转发。

#include "plugin_context_menu.h"

#include <QAction>
#include <QString>

namespace xrkit::shell {

int append_registry_context_items(QMenu* menu, xrkit::plugin::Registry* registry,
                                 const std::string& host_page_id,
                                 const xrkit::plugin::Context& context, QObject* owner) {
    if (menu == nullptr || registry == nullptr) return 0;
    const std::vector<const xrkit::plugin::CommandReg*> items =
        registry->menu_items_for(host_page_id, "context", context);
    if (items.empty()) return 0;   // 1.x「无注册即无痕迹」：连分隔符都不加
    if (!menu->actions().isEmpty()) menu->addSeparator();
    QObject* connect_owner = owner != nullptr ? owner : menu;
    int added = 0;
    for (const xrkit::plugin::CommandReg* command : items) {
        if (command == nullptr) continue;
        const std::string plugin = command->plugin;
        const std::string label = command->label;
        const xrkit::plugin::CommandHandler handler = command->handler;
        QAction* action = menu->addAction(QString::fromStdString(label));
        action->setObjectName(QStringLiteral("plugin_ctx_") +
                              QString::fromStdString(command->id));
        QObject::connect(action, &QAction::triggered, connect_owner,
                         [registry, plugin, label, handler] {
                             registry->invoke(plugin, label, handler);
                         });
        ++added;
    }
    return added;
}

}  // namespace xrkit::shell
