#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QWidget>

#if defined(_WIN32)
#include <dwmapi.h>
#include <windows.h>
#endif

namespace xrkit::shell {
namespace {

// 角色 → 颜色（令牌缺失就退回一个中性值，绝不崩）
QColor role(const char* name, const char* fallback = "#000000") {
    const char* hex = xrkit::core::theme_color_current(name);
    const QColor color(QString::fromUtf8(hex != nullptr ? hex : fallback));
    return color.isValid() ? color : QColor(QString::fromUtf8(fallback));
}

#if defined(_WIN32)
COLORREF to_colorref(const QColor& color) {
    // DWM 要的是 0x00BBGGRR（BGR 顺序）
    return static_cast<COLORREF>((color.blue() << 16) | (color.green() << 8) | color.red());
}
#endif

}  // namespace

int px(int value) { return xrkit::core::px(value); }

QString stylesheet_for(xrkit::core::ThemeMode mode) {
    xrkit::core::ThemeMode saved = xrkit::core::theme_mode();
    xrkit::core::set_theme_mode(mode);
    const QString bg = role("bg").name();
    const QString surface = role("surface").name();
    const QString surface2 = role("surface2").name();
    const QString border = role("border").name();
    const QString text = role("text").name();
    const QString dim = role("text_dim").name();
    const QString accent = role("accent").name();
    const QString entry = role("entry_bg").name();
    const QString selected = role("selected").name();
    const QString scroll_border = role("scroll_border").name();
    const QString sash = role("sash").name();
    xrkit::core::set_theme_mode(saved);

    // 只写"角色 → 控件"的映射；尺寸一律走 px()（HiDPI）
    return QStringLiteral(
               "QWidget { background: %1; color: %6; }"
               "QMainWindow::separator { background: %4; width: %11px; height: %11px; }"
               "QListWidget { background: %2; border: 1px solid %4; outline: none; }"
               "QListWidget::item { padding: %11px %12px; }"
               "QListWidget::item:selected { background: %9; color: %7; }"
               "QPlainTextEdit { background: %8; color: %6; border: 1px solid %4;"
               "  font-family: Consolas; }"
               "QLabel { background: transparent; }"
               "QLabel#status_label { color: %5; }"
               "QDockWidget::title { background: %3; padding: %11px; }"
               "QScrollBar:vertical { background: %2; width: %13px; margin: 0; }"
               "QScrollBar:horizontal { background: %2; height: %13px; margin: 0; }"
               "QScrollBar::handle { background: %4; border: 1px solid %10; }"
               "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
               "QPushButton { background: %3; border: 1px solid %4; padding: %11px %12px; }"
               "QPushButton:hover { border-color: %7; }"
               "QSplitter::handle { background: %14; }")
        .arg(bg, surface, surface2, border, dim, text, accent, entry, selected, scroll_border)
        .arg(px(4))
        .arg(px(8))
        .arg(px(10))
        .arg(sash);
}

void apply_theme(QApplication& app, xrkit::core::ThemeMode mode) {
    xrkit::core::set_theme_mode(mode);
    // 在**现有** palette 上改（不是新建一份塞回去）：这就是"原地更新"的 Qt 写法；
    // setPalette/setStyleSheet 会让 Qt 给所有控件发 StyleChange，界面即时跟着变。
    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, role("bg"));
    palette.setColor(QPalette::WindowText, role("text"));
    palette.setColor(QPalette::Base, role("entry_bg"));
    palette.setColor(QPalette::AlternateBase, role("surface2"));
    palette.setColor(QPalette::Text, role("text"));
    palette.setColor(QPalette::Button, role("surface2"));
    palette.setColor(QPalette::ButtonText, role("text"));
    palette.setColor(QPalette::Highlight, role("selected"));
    palette.setColor(QPalette::HighlightedText, role("text_bright"));
    palette.setColor(QPalette::ToolTipBase, role("surface2"));
    palette.setColor(QPalette::ToolTipText, role("text"));
    app.setPalette(palette);
    app.setStyleSheet(stylesheet_for(mode));

    for (QWidget* widget : QApplication::allWidgets()) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}

void apply_dark_titlebar(QWidget* widget, xrkit::core::ThemeMode mode) {
#if defined(_WIN32)
    if (widget == nullptr) return;
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (hwnd == nullptr) return;
    const BOOL dark = (mode == xrkit::core::ThemeMode::Dark) ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE（Win10 1809+；旧版本是 19）
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    // 35 = DWMWA_CAPTION_COLOR（Win11），取面板色，与 QSS 一致
    const COLORREF caption = to_colorref(role("surface"));
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
#else
    (void)widget;
    (void)mode;
#endif
}

}  // namespace xrkit::shell
