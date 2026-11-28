#pragma once

// 统一 UI 文案 API：界面构造时调用 tr_zh("中文源文案")。
// 源文案保持中文；俄语由 lang/ru.json 覆盖，缺失时回退中文。
// 不再依赖事后遍历控件，因此自定义控件/placeholder/表头/纯文本都必须走这里。

#include <QString>
#include <QStringList>

namespace xrkit::shell {

enum class UiLanguage { Chinese, Russian };

void initialize_ui_language(const QStringList& args);
UiLanguage ui_language();
QString tr_zh(const char* chinese);

}  // namespace xrkit::shell
