#include "theme.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QWidget>

#include <algorithm>
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

namespace {

// ★★ 2026-10-05「勾就勾」（用户对勾选框的原话）：**把勾真的画出来**，且**不引入写死的颜色**。
//   为什么不用 QSS 的 `image:`：勾图只能"生成"出来（本机 Qt **没有 Svg 模块**、QImage 的
//   builtin 格式里也没有能内联进 QSS 的文本格式；实测把 PNG/XPM 以 `url(data:...)` 塞进 QSS，
//   Qt 的样式表引擎根本不加载它 —— 三条写法全试过，规则里其他属性都生效、只有 image 不生效）。
//   为什么不用字体里的 ✓：离屏自检机器上字体会退化成空白方块（`--shot` 实测）。
//   ★ 还有一个实测出来的坑：**只要 QSS 里给 `::indicator` 配了尺寸/边框/底色，Qt 就完全自己
//     画那个指示器、根本不会再走 `QStyle::drawPrimitive(PE_IndicatorCheckBox)`**（本轮用
//     "置一个调试环境变量就往指示器里填品红"验证过：真机截图上 0 个品红像素）⇒ 所以
//     **指示器的 QSS 规则整条撤掉**，方框与勾都交给这个类画。
//   颜色仍然**全部来自主题**：`apply_theme()` 把主题令牌塞进 palette 的标准角色
//   （底 = Base、选中 = Button、描边/勾 = Text/Mid），这里只读 palette —— 页面与 QSS 里
//   没有任何一处写死的色值（强调色也仍然只出现在三处）。
class CheckMarkStyle : public QProxyStyle {
public:
    explicit CheckMarkStyle(QStyle* base) : QProxyStyle(base) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element != PE_IndicatorCheckBox || option == nullptr || painter == nullptr) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        const bool on = button != nullptr && (button->state & State_On) != 0 &&
                        !(button->state & State_NoChange);
        const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor fill = option->palette.color(on ? QPalette::Button : QPalette::Base);
        QColor edge = option->palette.color(QPalette::Mid);
        edge.setAlpha(on ? 255 : 160);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        // 方框：输入控件的样子（未选中 = 空框；选中 = 灰底）——"勾就勾"，不多加东西
        painter->setPen(QPen(edge, 1.0));
        painter->setBrush(fill);
        painter->drawRoundedRect(box, 2.0, 2.0);
        if (on) {
            QColor mark_color = option->palette.color(QPalette::Text);
            if (widget != nullptr) mark_color = widget->palette().color(QPalette::Text);
            QPen pen(mark_color);
            pen.setWidthF(std::max(1.6, option->rect.height() / 8.0));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(pen);
            const double u = option->rect.width() / 14.0;
            const double ox = option->rect.left();
            const double oy = option->rect.top();
            painter->drawPolyline(QPolygonF({QPointF(ox + 3.0 * u, oy + 7.6 * u),
                                             QPointF(ox + 5.9 * u, oy + 10.4 * u),
                                             QPointF(ox + 11.0 * u, oy + 4.0 * u)}));
        }
        painter->restore();
    }
};

}  // namespace

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
    // ★ 勾选框的"勾"**不走 QSS 图片**（实测 Qt 的样式表不加载 `url(data:...)`），
    //   由 `CheckMarkStyle`（本文件顶部）在选中态画一笔 —— 见那边的说明。
    // 控件高度：28 → 24（用户 2026-09-30："默认有点偏大"⇒ 整体收紧，页面也不再堆得那么高）
    size("ctl", 24);
    // 内容区（包列表/包内树/日志）字号：比界面正文大一档，且走 px() ⇒ 跟着界面大小缩
    size("font_content", 16);;
    xrkit::core::set_theme_mode(saved);

    // ★ 配色口径（用户 2026-09-27：「配色里为什么这么多蓝色」⇒ 与
    //   `docs/2.0.0-界面重构口径(排版与配色).md` 第二节同一条）：
    //   **强调色（蓝）只出现在三处** —— 主按钮（#primary）/ 当前标签项 / 进度条。
    //   hover 边框、勾选框选中态、滚动条 hover 一律用**中性色**；列表与下拉的选中态
    //   用灰底 `@{selected}` + 亮字。
    QString sheet = QStringLiteral(
               // 基底
               // ★★ 2026-10-05 用户：「那个露出深灰色底板的设计需要重做或者改掉，很丑」。
               //   机制（本轮用 `--shot` + 控件级 `render()` 采样**实测**出来的，不是猜的）：
               //   `QWidget { background: @{bg}; }` 这条**泛化规则命中所有"没写 QSS 的裸容器"**
               //   （`path_row()` 里的行容器、`TaskShell::panel()`、页视口…）—— Qt 给它们置位
               //   `WA_StyledBackground` 后**每个容器都自己涂一层 `bg`（比卡片 `surface2` 更深的
               //   灰）**；行容器又正好被标签/输入框盖住只**露出标签背后那一块** ⇒ 就是用户截图里
               //   那块"缺了一块"的深灰圆角底板（实测采样：容器渲染出 `#1e2124`，标签自己
               //   `render()` 是透明的，画底的是**容器**）。
               //   ⇒ 修法：**把"给窗口上色"和"给子控件上色"分开** —— 窗口层（`QMainWindow` +
               //   `#hub_central`）自己上 `bg`，其余容器一律**透明**（不再自己涂）；真正需要底的
               //   面（卡片/列表/输入/按钮/表头/标签条/进度条）本来就有各自规则。
               //   这条改法同时消掉"父子上色叠出深灰块"这一整类毛病（同类控件一处样式统一改）。
               //   ★ 为什么**保留** `QWidget { background: @{bg}; }` 这一份而不是删掉：它是
               //     **QPalette::Window 的口径来源**（自检 ④ 就钉着"暗/亮色 QPalette::Window
               //     == 令牌 bg"）。删掉它会连调色板一起变（实测：probe 的 Window 掉成 #000000，
               //     亮色主题下 app palette 还退化成默认色）⇒ 那是把"底色口径"拆成两处真值。
               //     所以保留这一份，**只把会露馅的那几类裸容器单独改成透明**（下面几条）。
               "QWidget { background: @{bg}; color: @{text}; }"
               // ★ 字段行容器（六个页面的"标签 + 输入框"行都经 `DropZones::attach_row`，
               //   那里统一挂上 `field_row` 名字）：它**不该自己涂底** —— 涂了就是用户截图里
               //   标签背后那块深灰圆角（容器底色 `bg` 比卡片 `surface2` 深，只有标签那一块
               //   露出来）。整行铺满窗口的拖放区照旧是 `bg`（与所在面板一致），不会露白。
               "QWidget#field_row { background: transparent; }"
               //   任务面板：同一类现场（裸容器 + 进度条/状态字），自己不涂底。
               "QWidget#task_panel { background: transparent; }"
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
               "QComboBox::drop-down { border: 0; background: transparent; width: @{pad10}; }"
               // ★ 2026-10-05：下拉的**弹层**统一到与输入框同一套令牌（原来弹层底是 `surface`、
               //   与"选中的那一行"的灰底 `selected` 撞色 ⇒ 用户说"下拉栏设计太丑"）。
               //   `item` 也补上内距与圆角，与列表项同一套几何。
               "QComboBox QAbstractItemView { background: @{entry_bg}; color: @{text};"
               "  border: 1px solid @{border}; border-radius: @{r2}; outline: none;"
               "  padding: @{pad2}; selection-background-color: @{selected};"
               "  selection-color: @{text_bright}; }"
               "QComboBox QAbstractItemView::item { padding: @{pad4} @{pad8};"
               "  border-radius: @{r2}; min-height: @{ctl}; }"
               // 勾选 / 单选（用户 2026-10-05 原话：「勾就勾」）：**勾选态只表现"勾上了"** ——
               //   方框保持"输入控件"的样子（不选中 = 空框），选中 = 灰底 + **真画出来的勾**；
               //   不再用"整体填一块浅灰"这种"多加了东西"的形态（那正是用户说"违和"的地方）。
               //   ★ 指示器**不写 QSS**：实测只要 QSS 给 `::indicator` 配了尺寸/边框/底色，
               //     Qt 就自己画指示器、**完全不再进 `drawPrimitive(PE_IndicatorCheckBox)`**
               //     ⇒ 自绘的勾永远不出现（本轮验证过）。方框与勾都由 `CheckMarkStyle` 画，
               //     颜色全部来自 `apply_theme()` 塞进 palette 的主题令牌。
               //   （顺带记一笔坑：`QCheckBox::indicator:checked` 这种写法**不合法**，
               //     伪状态必须写在子控件前面 —— 修前那条"浅灰实心块"是死规则，从没生效过。）
               "QCheckBox, QRadioButton { spacing: @{pad4}; }"
               // 尺寸令牌仍由 Qt 读（`ind14`），自绘时用的是 QSS 算出来的这个矩形
               "QCheckBox::indicator, QRadioButton::indicator { width: @{ind14};"
               "  height: @{ind14}; }"
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
"QTabWidget::pane { border: 1px solid @{border}; border-radius: @{r2}; }"
               // 可关闭标签页（QTabWidget 的页签）：与页面顶栏同一套令牌 ——
               // 未选中 = 输入框底 `entry_bg`、选中 = 面板底 `surface` + 中性色边线
               // （强调色只许出现在三处，这里不刷蓝）。**这是 QTabWidget 页签唯一的一份规则**。
               "QTabWidget::tab-bar { left: 0; }"
               "QTabBar::tab { background: @{entry_bg}; color: @{text_dim};"
               "  border: 1px solid @{border}; border-bottom: none;"
               "  border-top-left-radius: @{r2}; border-top-right-radius: @{r2};"
               "  padding: @{pad4} @{pad8}; }"
               "QTabBar::tab:hover { color: @{text}; }"
               "QTabBar::tab:selected { background: @{surface}; color: @{text}; }"
// ★ 2026-10-05：这里原有 `QTabBar::tab` / `QGroupBox` / `QHeaderView::section` /
//   `QMenu, QToolTip` 四组**重写规则**，把上面同一批选择器的属性又改了一遍
//   （同名选择器后写的赢）—— 等于“同一个控件两套样式、只有后来那套生效”。
//   用户口径是**同类只维护一套**，所以这四处重写删掉，只留上面那一份。
               // ★ 用户 2026-09-30：「包和内容物的字体大小太小。」⇒ 内容区字号单独放大
               "QTreeWidget, QTreeView, QListView { font-size: @{font_content}; }"
               // ★ 用户真机反馈「又挤又空」的根因＝控件太小、间距太小 ⇒ 控件最小高度与内边距**整体上调
               //   一档**（值都在上面那批命名令牌里；这块原先用独立 `%1..%4` 是对的，一并并进来）
               "QPushButton { min-height: @{ctl}; padding: @{pad8} @{pad14}; }"
               "QComboBox, QLineEdit, QSpinBox { min-height: @{ctl}; padding: @{pad8} @{pad10}; }"
               "QListWidget::item, QListView::item, QTreeView::item { min-height: @{ctl}; }"
               // ★★ 表头不许裁字（用户 2026-09-30 与 2026-10-01 两次指出"那行字顶部被切"）：
               //   **实测过的机制**（`--shot` 打的 HDR 行）：QSS 给 section 配了 8 px 上下内边距，
               //   而 `QHeaderView` 的取高算法**不把这段内边距算进去**（实测表头 25 px，文字
               //   15 px ⇒ 内容区只剩 25-16=9 px ⇒ 顶部被切）。QSS 的 `min-height` 也压不住它。
               //   ⇒ 两处一起改：① 上下内边距降到 4 px；② **表头字号与内容同号**
               //   （`font_content`，表头尺寸随之算进去 ⇒ 实测 26 px 高、内容区 18 px > 字高）；
               //   ③ `min-height` 用同一个字号令牌兜底。**改任何一个数都要重看 HDR 行与截图。**
               "QHeaderView::section { font-size: @{font_content}; min-height: @{font_content};"
               "  padding: @{pad4} @{pad10}; }"
               "QLabel#page_header { padding: @{pad8} 0; }"
               // ★ 2026-10-04 用户第 1 条：「所有目录都需要拖拽，而且是**拖拽到相应区域**。」
               //   "看得见它接住了"＝拖放区域高亮。**只改颜色，一个像素都不动布局**
               //   ★ 用**中性色** `@{selected}`，**不用** `@{accent}`：强调色（蓝）只许出现在
               //     三处（主按钮 / 当前标签项 / 进度条，见配色口径第二节），这里刷蓝会被那条锁判红
               //     （本轮真被拦住过一次 —— 锁是对的，改的是我）。
               //   （属性由 `drop_filter.cpp` 在 DragEnter/Drop/DragLeave 置位）。
               "*[drop_hover=\"true\"] { background: @{surface};"
               "  border: 1px solid @{selected}; }");

    for (const auto& kv : tokens) sheet.replace(kv.first, kv.second);
    return sheet;
}

namespace {
double g_base_font_pt = 0.0;   // 首次调用时的基准（之后缩放都相对它，避免连乘）
}  // namespace

double ui_size_base_font_pt() {
    if (g_base_font_pt <= 0.0) g_base_font_pt = QApplication::font().pointSizeF();
    return g_base_font_pt;
}

const std::vector<UiSizeChoice>& ui_size_choices() {
    // ★ 用户 2026-09-30：「小有点偏小，默认有点偏大，大和特大就不用说了。」⇒ 收紧档位
    static const std::vector<UiSizeChoice> kChoices = {{"小", 0.9}, {"默认", 1.0},
                                                       {"大", 1.1}, {"特大", 1.2}};
    return kChoices;
}

void apply_ui_size(QApplication& app, double scale) {
    if (scale <= 0.0) return;
    const double base = ui_size_base_font_pt();
    xrkit::core::set_ui_scale(scale);   // 控件尺寸（px() 全部按它算）
    // 字体由 `apply_theme` 统一按 ui_scale 设（见那边的 setPointSizeF）；这里只重套主题。
    Q_UNUSED(base);
    // 样式表里的尺寸是 px() 生成的 ⇒ 重新套一遍才立刻生效；模式从属性里读（hub 切主题时写的）
    const QString mode = app.property("xrkit_theme").toString();
    apply_theme(app, mode == QStringLiteral("light") ? xrkit::core::ThemeMode::Light
                                                       : xrkit::core::ThemeMode::Dark);
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
    // ★ 2026-10-05：勾选框的方框/勾由 `CheckMarkStyle` 自绘，它只读 palette —— 这里把
    //   用到的标准角色一次配齐（未选中底 = 输入框底；选中底 = 中性灰；描边 = 边框色）。
    //   这样"勾选框的颜色"仍然只有**一处真值**（主题令牌），QSS/页面里都没有写死的色值。
    palette.setColor(QPalette::Base, role("entry_bg"));
    palette.setColor(QPalette::Button, role("selected"));
    palette.setColor(QPalette::Mid, role("border"));
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
        // 字体点数也要乘 ui_scale（用户：调整 UI 控件大小要**包括里面的字体**）——
    // 这里是字体尺寸的**唯一来源**，`apply_ui_size` 只负责 set_ui_scale + 重套本函数。
    base.setPointSizeF((kBasePt + 2.0) * xrkit::core::ui_scale());
        app.setFont(base);
    }
    app.setPalette(palette);
    // ★ 用户 2026-09-30：「很多UI控件可以稍微硬朗一些，现在有点太圆润了。」——查证：我们的圆角
    //   令牌本来就小（r3=3px / r2=2px），圆润来自**平台原生样式**（Win11 风格自己给标签栏/分组框/
    //   表头画圆角，而这些控件我们原本没写 QSS）⇒ 用 **Fusion**（扁平、完全由 QSS 决定外观），
    //   并补上缺的几条规则。
    // ★ 2026-10-05：外层再套一层 `CheckMarkStyle`（只多做一件事：给勾选框/单选框的选中态
    //   画一个勾；别的全部原样委托给 Fusion）。
    // 用 dynamic_cast（本类**故意不带 Q_OBJECT**：moc 只扫有 Q_OBJECT 的头，
    // 这个文件里的小样式类没必要引 moc；qobject_cast 则需要 Q_OBJECT 宏）。
    if (dynamic_cast<CheckMarkStyle*>(app.style()) == nullptr) {
        QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
        if (fusion == nullptr) fusion = app.style();
        app.setStyle(new CheckMarkStyle(fusion));
    }
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
