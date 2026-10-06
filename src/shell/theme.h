#pragma once

#include <vector>
// xrkit::shell —— 主题映射（M2-0/M2-1）：把 core 的令牌映射成 QPalette + QSS。
//
// **Qt 侧不抄令牌表**（1.x 的口径：`THEMES`/`_FONTS` 是唯一真值来源，Qt 侧按 AST 读它）。
// 这里逐项从 `xrkit::core` 取色，切换主题走 `QApplication::setPalette` +
// `setStyleSheet`（Qt 自己会把 StyleChange 事件发到所有控件 —— 与 1.x"`T` 原地更新、
// 绝不重新绑定"是同一个语义）。

#include <QString>

#include "xrkit/core/theme.h"

class QApplication;
class QWidget;

namespace xrkit::shell {

// 按 96 DPI 基准值取像素（外壳里所有硬编码尺寸都要过它，见 core::px 的说明）
int px(int value);

// 生成当前模式的 QSS（角色 → Qt 控件）。
// ★ 用户 2026-09-30：主题旁要一个「调整 UI 控件大小（包括里面的字体）」的下拉。
//   scale: 0.85 小 / 1.0 默认 / 1.25 大 / 1.5 特大；**立即生效**（控件尺寸 + 字体 + 重套样式表）。
void apply_ui_size(class QApplication& app, double scale);
// 「界面大小」的档位（label, scale）。自检要能真读到这些值（不是只看菜单文案）。
struct UiSizeChoice { const char* label; double scale; };
const std::vector<UiSizeChoice>& ui_size_choices();
double ui_size_base_font_pt();   // 未缩放时的基准字体点数（首用时的值）

QString stylesheet_for(xrkit::core::ThemeMode mode);

// 应用主题：设置 QPalette + QSS，并刷新已存在控件（原地更新）。
void apply_theme(QApplication& app, xrkit::core::ThemeMode mode);

// 启动序列（**交互式启动与离屏自检共用的唯一一份**）：读 user.ltx 的偏好 → 铺主题 →
// 屏幕自适应 → 有保存档时覆盖档位 → **再套一次主题**（字体与几何必须同源，见 .cpp 里的
// H-1 说明），返回本次生效的主题（调用方要拿它去套深色标题栏等）。
xrkit::core::ThemeMode apply_startup_ui(QApplication& app);

// Windows 深色标题栏（DWM）。非 Windows 上是空操作。
void apply_dark_titlebar(QWidget* widget, xrkit::core::ThemeMode mode);

}  // namespace xrkit::shell
