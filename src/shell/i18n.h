#pragma once

// 轻量 UI 本地化：源文案仍是中文，俄语通过 lang/ru.json 的精确匹配替换。
// 这样不需要把 1800+ 个 QStringLiteral 全部改成 tr()，中文版行为不变；
// 新增文案只要加一条 JSON，缺失时自动回退中文。

#include <QString>
#include <QStringList>

class QObject;

namespace xrkit::shell {

enum class UiLanguage { Chinese, Russian };

void initialize_ui_language(const QStringList& args);
UiLanguage ui_language();
QString tr_zh(const char* chinese);

// 递归翻译已构造控件上的静态文案（菜单/按钮/标签/页签/表头/占位符）。
// 只替换 JSON 里存在的**完整字符串**；动态拼接文本保持中文回退。
void apply_ui_language(QObject* root);

// 监听后续弹出的顶层窗口（对话框/插件窗口），Show 后自动翻译。
void install_ui_language_watcher();

}  // namespace xrkit::shell
