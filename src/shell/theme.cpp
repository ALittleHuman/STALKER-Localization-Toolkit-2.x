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
    const QString accent_hover = role("accent_hover").name();
    const QString bright = role("text_bright").name();
    const QString danger = role("red").name();
    xrkit::core::set_theme_mode(saved);

    // ★ 2026-09-27 界面重构（用户："排版完全重新设计…配色也改一下"）：
    //   这张 QSS 是"控件像不像成品"的关键 —— 按钮/下拉/输入/列表/滚动条/表头/勾选**统一**，
    //   并且每个可交互控件都有 **hover / pressed / disabled** 三态；强调色(accent)只出现在
    //   "当前/主操作/进行中"这三处。尺寸一律走 px()（HiDPI）。
    return QStringLiteral(
               // 基底
               "QWidget { background: %1; color: %6; }"
               "QLabel { background: transparent; }"
               "QLabel#status_label { color: %5; }"
               "QLabel#page_header { color: %16; }"
               "QLabel#page_subtitle { color: %5; }"
               // 折叠日志：折叠按钮（一行、透明、左对齐）+ 清空小按钮
               "QPushButton#log_toggle { background: transparent; border: 0; color: %5;"
               "  padding: %11px %12px; text-align: left; }"
               "QPushButton#log_toggle:hover { color: %6; }"
               "QPushButton#log_clear { padding: %18px %12px; }"
               // 顶部标签条（方案一）：横向铺开、当前页 accent 下划线 + 高亮
               "QTabBar#page_tabs { background: %2; border: 0;"
               "  border-bottom: 1px solid %4; }"
               "QTabBar#page_tabs::tab { background: transparent; color: %5;"
               "  padding: %12px %12px; border: 0; border-bottom: %18px solid transparent;"
               "  margin-right: %18px; }"
               "QTabBar#page_tabs::tab:hover { color: %6; }"
               "QTabBar#page_tabs::tab:selected { color: %6;"
               "  border-bottom: %18px solid %7; }"
               // 卡片与分组（内容区加边界：原来"裸列表铺满"全靠这个改观）
               "QFrame#card, QGroupBox { background: %3; border: 1px solid %4;"
               "  border-radius: %17px; }"
               "QFrame#card { padding: %12px; }"
               "QGroupBox { margin-top: %11px; padding: %12px; }"
               "QGroupBox::title { subcontrol-origin: margin; left: %12px; padding: 0 %11px;"
               "  color: %5; }"
               // 列表：项高一致、hover 可见、选中用强调色底 + 亮文本
               "QListWidget, QListView, QTreeView { background: %2; border: 1px solid %4;"
               "  border-radius: %17px; outline: none; }"
               "QListWidget::item, QListView::item, QTreeView::item { padding: %11px %12px;"
               "  border-radius: %18px; }"
               "QListWidget::item:hover, QListView::item:hover, QTreeView::item:hover"
               "  { background: %3; }"
               "QListWidget::item:selected, QListView::item:selected, QTreeView::item:selected"
               "  { background: %9; color: %19; }"
               "QHeaderView::section { background: %3; color: %5; border: 0;"
               "  border-bottom: 1px solid %4; padding: %11px %12px; }"
               // 日志/文本区
               "QPlainTextEdit, QTextEdit { background: %8; color: %6; border: 1px solid %4;"
               "  border-radius: %17px; font-family: Consolas; }"
               // 按钮：三态 + 主按钮（objectName=primary）用 accent 实心
               "QPushButton { background: %3; color: %6; border: 1px solid %4;"
               "  border-radius: %17px; padding: %11px %12px; }"
               "QPushButton:hover { background: %2; border-color: %15; }"
               "QPushButton:pressed { background: %1; }"
               "QPushButton:disabled { color: %5; border-color: %4; background: %3; }"
               "QPushButton#primary { background: %7; color: %19; border: 1px solid %15; }"
               "QPushButton#primary:hover { background: %15; }"
               "QPushButton#primary:pressed { background: %7; }"
               // 下拉 / 输入：与按钮同一套边界与圆角
               "QComboBox, QLineEdit, QSpinBox { background: %8; color: %6;"
               "  border: 1px solid %4; border-radius: %17px; padding: %11px %12px; }"
               "QComboBox:hover, QLineEdit:hover { border-color: %15; }"
               "QComboBox:disabled, QLineEdit:disabled { color: %5; }"
               "QComboBox::drop-down { border: 0; width: %19px; }"
               "QComboBox QAbstractItemView { background: %2; border: 1px solid %4;"
               "  selection-background-color: %9; }"
               // 勾选 / 单选：强调色只在"选中"这一处
               "QCheckBox, QRadioButton { spacing: %11px; }"
               "QCheckBox::indicator, QRadioButton::indicator { width: %20px; height: %20px;"
               "  border: 1px solid %4; border-radius: %18px; background: %8; }"
               "QCheckBox::indicator:checked, QRadioButton::indicator:checked"
               "  { background: %7; border-color: %15; }"
               // 滚动条：细、圆角、hover 变亮
               "QScrollBar:vertical { background: %2; width: %13px; margin: 0; }"
               "QScrollBar:horizontal { background: %2; height: %13px; margin: 0; }"
               "QScrollBar::handle { background: %4; border-radius: %18px; }"
               "QScrollBar::handle:hover { background: %15; }"
               "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
               "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
               // 分栏 / 停靠 / 提示
               "QSplitter::handle { background: %14; }"
               "QDockWidget::title { background: %3; padding: %11px; }"
               "QMainWindow::separator { background: %4; width: %11px; height: %11px; }"
               "QToolTip { background: %3; color: %6; border: 1px solid %4;"
               "  border-radius: %17px; padding: %11px; }"
               "QProgressBar { background: %8; border: 1px solid %4; border-radius: %17px;"
               "  text-align: center; color: %6; }"
               "QProgressBar::chunk { background: %7; border-radius: %17px; }")
        .arg(bg, surface, surface2, border, dim, text, accent, entry, selected, scroll_border)
        .arg(px(4))
        .arg(px(8))
        .arg(px(10))
        .arg(sash)
        .arg(accent_hover)      // %15
        .arg(bright)            // %16
        .arg(px(6))             // %17 圆角
        .arg(px(3))             // %18 小圆角
        .arg(bright)            // %19 强调底上的文字色
        .arg(px(14))            // %20 勾选框尺寸
        .arg(danger)            // %21（保留：危险色，供后续按钮/状态用）
        .arg(px(32));           // %22 导航项高（口径：32px，过 px() 缩放）
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
