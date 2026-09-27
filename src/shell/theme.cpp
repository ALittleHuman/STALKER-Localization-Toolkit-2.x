#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QWidget>

#include <utility>
#include <vector>

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
    // ★ 2026-09-27：**这套 QSS 从 `%1..%N` 链式 `.arg()` 改成「命名令牌 + replace」**。
    //   起因是真机实测到的真事故（用户截图 + 控制台刷屏）：模板编号**不连续**时
    //   Qt 的 `.arg()` 会把"下一个值"喂给"下一个**还存在**的号" ⇒ **从缺号处整体错位**。
    //   当时的现场（本机 `--smoke` 实测 88 条 `QCssParser::parseHexColor: Unknown color
    //   name '#3a3f45px'`）：`padding: #3a3f45px 4px`、`border-radius: #ffffffpx`、
    //   `color: 2`、`background: 10` —— 也就是**尺寸槽拿到颜色值、颜色槽拿到数字**，
    //   于是那些 padding / 圆角 / 勾选框尺寸 / 选中文字色**全部静默失效**（观感因此
    //   "又挤又空"），而链尾还多出 3 条 `QString::arg: Argument missing`。
    //   命名令牌从机制上消除这一类错位：模板里再写错名字会留下 `@{`，自检当场判红。
    //   判据（都在 SHELL-SMOKE 里）：① 生成样式表里**不许出现「颜色值 + px」**；
    //   ② 不许有未替换的令牌、不许有残留 `%N`；③ 应用样式表期间 Qt **一条
    //   `parseHexColor` / `Argument missing` 警告都不许发**。
    xrkit::core::ThemeMode saved = xrkit::core::theme_mode();
    xrkit::core::set_theme_mode(mode);

    std::vector<std::pair<QString, QString>> tokens;
    tokens.reserve(24);
    // 颜色令牌：名字与 core 的角色名一致（`@{text_dim}` 这种），少写/写错会在自检里红
    const auto color = [&tokens](const char* name) {
        const char* hex = xrkit::core::theme_color_current(name);
        tokens.emplace_back(QStringLiteral("@{") + QString::fromUtf8(name) + QStringLiteral("}"),
                            QString::fromUtf8(hex != nullptr ? hex : "#000000"));
    };
    // 尺寸令牌：**按 96 DPI 的设计值过 px()**（HiDPI 口径，见 px-only 门禁）
    const auto size = [&tokens](const char* name, int value) {
        tokens.emplace_back(QStringLiteral("@{") + QString::fromUtf8(name) + QStringLiteral("}"),
                            QString::number(px(value)) + QStringLiteral("px"));
    };
    color("bg");
    color("surface");
    color("surface2");
    color("border");
    color("text");
    color("text_dim");
    color("text_bright");
    color("accent");
    color("accent_hover");
    color("entry_bg");
    color("selected");
    color("sash");
    size("pad2", 2);
    size("pad4", 4);
    size("pad8", 8);
    size("pad10", 10);
    size("pad14", 14);
    size("r3", 3);
    size("r2", 2);
    size("ind14", 14);
    size("ctl28", 28);
    xrkit::core::set_theme_mode(saved);

    // ★ 配色口径（用户 2026-09-27：「配色里为什么这么多蓝色」⇒ 与
    //   `docs/2.0.0-界面重构口径(排版与配色).md` 第二节同一条）：
    //   **强调色（蓝）只出现在三处** —— 主按钮（#primary）/ 当前标签项 / 进度条。
    //   hover 边框、勾选框选中态、滚动条 hover 一律用**中性色**；列表与下拉的选中态
    //   用灰底 `@{selected}` + 亮字。
    QString sheet = QStringLiteral(
               // 基底
               "QWidget { background: @{bg}; color: @{text}; }"
               "QLabel { background: transparent; }"
               "QLabel#status_label { color: @{text_dim}; }"
               "QLabel#page_header { color: @{text_bright}; }"
               "QLabel#page_subtitle { color: @{text_dim}; }"
               // 折叠日志：折叠按钮（一行、透明、左对齐）+ 清空小按钮
               "QPushButton#log_toggle { background: transparent; border: 0; color: @{text_dim};"
               "  padding: @{pad4} @{pad8}; text-align: left; }"
               "QPushButton#log_toggle:hover { color: @{text}; }"
               "QPushButton#log_clear { padding: @{pad2} @{pad8}; }"
               // 顶部标签条（方案一）：横向铺开、当前页 accent 下划线 + 高亮
               "QTabBar#page_tabs { background: @{surface}; border: 0;"
               "  border-bottom: 1px solid @{border}; }"
               "QTabBar#page_tabs::tab { background: transparent; color: @{text_dim};"
               "  padding: @{pad8} @{pad8}; border: 0; border-bottom: @{r2} solid transparent;"
               "  margin-right: @{pad2}; }"
               "QTabBar#page_tabs::tab:hover { color: @{text}; }"
               "QTabBar#page_tabs::tab:selected { color: @{text};"
               "  border-bottom: @{r2} solid @{accent}; }"
               // 卡片与分组（内容区加边界：原来"裸列表铺满"全靠这个改观）
               "QFrame#card, QGroupBox { background: @{surface2}; border: 1px solid @{border};"
               "  border-radius: @{r3}; }"
               "QFrame#card { padding: @{pad8}; }"
               "QGroupBox { margin-top: @{pad4}; padding: @{pad8}; }"
               "QGroupBox::title { subcontrol-origin: margin; left: @{pad8}; padding: 0 @{pad4};"
               "  color: @{text_dim}; }"
               // 列表：项高一致、hover 可见、选中用**灰底** + 亮文本（不刷蓝）
               "QListWidget, QListView, QTreeView { background: @{surface}; border: 1px solid @{border};"
               "  border-radius: @{r3}; outline: none; }"
               "QListWidget::item, QListView::item, QTreeView::item { padding: @{pad4} @{pad8};"
               "  border-radius: @{r2}; }"
               "QListWidget::item:hover, QListView::item:hover, QTreeView::item:hover"
               "  { background: @{surface2}; }"
               "QListWidget::item:selected, QListView::item:selected, QTreeView::item:selected"
               "  { background: @{selected}; color: @{text_bright}; }"
               "QHeaderView::section { background: @{surface2}; color: @{text_dim}; border: 0;"
               "  border-bottom: 1px solid @{border}; padding: @{pad4} @{pad8}; }"
               // 日志/文本区
               "QPlainTextEdit, QTextEdit { background: @{entry_bg}; color: @{text}; border: 1px solid @{border};"
               "  border-radius: @{r3}; font-family: Consolas; }"
               // 按钮：三态 + 主按钮（objectName=primary）用 accent 实心 —— accent 的三处之一
               "QPushButton { background: @{surface2}; color: @{text}; border: 1px solid @{border};"
               "  border-radius: @{r3}; padding: @{pad4} @{pad8}; }"
               "QPushButton:hover { background: @{surface}; border-color: @{border}; }"
               "QPushButton:pressed { background: @{bg}; }"
               "QPushButton:disabled { color: @{text_dim}; border-color: @{border}; background: @{surface2}; }"
               "QPushButton#primary { background: @{accent}; color: @{text_bright}; border: 1px solid @{accent_hover}; }"
               "QPushButton#primary:hover { background: @{accent_hover}; }"
               "QPushButton#primary:pressed { background: @{accent}; }"
               // 下拉 / 输入：与按钮同一套边界与圆角；hover 用中性色 + 提亮的底（不刷蓝）
               "QComboBox, QLineEdit, QSpinBox { background: @{entry_bg}; color: @{text};"
               "  border: 1px solid @{border}; border-radius: @{r3}; padding: @{pad4} @{pad8}; }"
               "QComboBox:hover, QLineEdit:hover { border-color: @{border}; background: @{surface2}; }"
               "QComboBox:disabled, QLineEdit:disabled { color: @{text_dim}; }"
               "QComboBox::drop-down { border: 0; width: @{pad10}; }"
               "QComboBox QAbstractItemView { background: @{surface}; border: 1px solid @{border};"
               "  selection-background-color: @{selected}; }"
               // 勾选 / 单选：选中态用**中性色的实心块**（原来那点蓝也在这里，按口径收掉）
               "QCheckBox, QRadioButton { spacing: @{pad4}; }"
               "QCheckBox::indicator, QRadioButton::indicator { width: @{ind14}; height: @{ind14};"
               "  border: 1px solid @{border}; border-radius: @{r2}; background: @{entry_bg}; }"
               "QCheckBox::indicator:checked, QRadioButton::indicator:checked"
               "  { background: @{text_dim}; border-color: @{text}; }"
               // 滚动条：细、圆角、hover 变亮（用中性色，不用 accent）
               "QScrollBar:vertical { background: @{surface}; width: @{pad10}; margin: 0; }"
               "QScrollBar:horizontal { background: @{surface}; height: @{pad10}; margin: 0; }"
               "QScrollBar::handle { background: @{border}; border-radius: @{r2}; }"
               "QScrollBar::handle:hover { background: @{text_dim}; }"
               "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
               "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
               // 分栏 / 停靠 / 提示
               "QSplitter::handle { background: @{sash}; }"
               "QDockWidget::title { background: @{surface2}; padding: @{pad4}; }"
               "QMainWindow::separator { background: @{border}; width: @{pad4}; height: @{pad4}; }"
               "QToolTip { background: @{surface2}; color: @{text}; border: 1px solid @{border};"
               "  border-radius: @{r3}; padding: @{pad4}; }"
               "QProgressBar { background: @{entry_bg}; border: 1px solid @{border}; border-radius: @{r3};"
               "  text-align: center; color: @{text}; }"
               "QProgressBar::chunk { background: @{accent}; border-radius: @{r3}; }"
               // ★ 用户真机反馈「又挤又空」的根因＝控件太小、间距太小 ⇒ 控件最小高度与内边距**整体上调
               //   一档**（值都在上面那批命名令牌里；这块原先用独立 `%1..%4` 是对的，一并并进来）
               "QPushButton { min-height: @{ctl28}; padding: @{pad8} @{pad14}; }"
               "QComboBox, QLineEdit, QSpinBox { min-height: @{ctl28}; padding: @{pad8} @{pad10}; }"
               "QListWidget::item, QListView::item, QTreeView::item { min-height: @{ctl28}; }"
               "QHeaderView::section { min-height: @{pad8}; padding: @{pad8} @{pad10}; }"
               "QLabel#page_header { padding: @{pad8} 0; }");

    for (const auto& kv : tokens) sheet.replace(kv.first, kv.second);
    return sheet;
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
    // ★ 用户真机反馈（2026-09-27）：「很多字符和控件太小，相互的空格也小」
    //   ⇒ 界面看着又挤又空。这里把**全局默认字体放大一档**（所有控件继承）。
    {
        // ★ 用户 2026-09-27 实测 bug：**每次切主题（哪怕选的是同一个）字号都会被再 +2**
        //   —— 原来是"在当前字号上 +2"，切一次涨一次。这里改成**从一次性的基线值算**。
        static const double kBasePt = [] {
            const QFont f = QApplication::font();
            return f.pointSizeF() > 0 ? f.pointSizeF() : 9.0;
        }();
        QFont base = app.font();
        base.setPointSizeF(kBasePt + 2.0);
        app.setFont(base);
    }
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
