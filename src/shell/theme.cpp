#include "theme.h"

#include "screen_scale.h"   // 启动序列要用 apply_screen_ui_scale()（H-1 把它搬进函数）

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
#include <cstring>
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

    // ★★ 2026-10-05（用户第 3 条「**所有下拉老样式**…1.x 线条比 2.0.0 硬朗多了」）：
    //   下拉的**三角箭头**颜色由 `apply_theme` 每次灌进来（＝主题令牌 `text_dim`）。
    //   为什么不读 palette：`text_dim` 在 palette 里**没有对应的标准角色**
    //   （Base/Button/Mid/Text 都已经各有其主，见 apply_theme 那一段），硬塞一个角色
    //   会连带改掉别的控件观感；而"颜色只有一处真值"这条仍然成立 —— 真值就是主题令牌，
    //   这里只是把它搬过来。**本文件里没有任何写死的色值。**
    void set_arrow_color(const QColor& color) { arrow_color_ = color; }
    void set_arrow_colors(const QColor& arrow, const QColor& slot, const QColor& edge) {
        arrow_color_ = arrow;
        slot_color_ = slot;
        edge_color_ = edge;
    }

    // ★★ 2026-10-05（用户第 3 条「**所有下拉老样式**…1.x 线条比 2.0.0 硬朗多了」）：
    //   **本类在 `PE_IndicatorArrowDown` 里画下拉的「箭头槽 + 三角箭头」** ——
    //   本批**实测出来的机制**，不是猜的（真 combo `grab()` 后数 `text_dim` 像素，四种 QSS 变体）：
    //     · 只要 QSS 里**存在**一条 `QComboBox::drop-down` 规则（哪怕内容只有 `border: 0`），
    //       Qt 的样式表样式就接管整个 combo 的绘制、**连箭头都不画** ⇒ **0** 个箭头像素；
    //     · 去掉那条规则 ⇒ 箭头出来，但 Fusion 那个只有 **6** 个像素大（远不是 1.x 的硬朗三角）。
    //   为什么在这里画（而不是 `drawComplexControl(CC_ComboBox)`）：**实测 Qt 对 combo 走的就是
    //   这个 primitive**（抓到的 `option->rect` = `[367,4 10x26]`，`widget` = 那个 381×34 的
    //   combo）—— 而 `drawComplexControl` 在本项目的样式栈下**根本不被调**（本批先写在那里，
    //   真机上一个箭头都出不来）。
    //   ⇒ 定案：**QSS 不再给 `::drop-down` 写任何规则**（槽几何与槽底都归本类），
    //     本类在基类画完"框 + 文本"之后补上 1.x 的**箭头槽（surface2）+ 三角（text_dim）**。
    //   ★ 只画一次：交给基类的**副本**里摘掉 `SC_ComboBoxArrow`，否则 Fusion 会再画一个。
    //   ★ 判据（SHELL-SMOKE 下拉①′）：真 combo 的槽里必须量到 ≥6 个 `text_dim` 像素
    //     （把 `::drop-down` 规则加回来 / 或本函数不画 ⇒ 立刻红，实测 0）。
    // ★★ 2026-10-05（用户第 3 条「**所有下拉老样式**…1.x 线条比 2.0.0 硬朗多了」）：
    //   **箭头槽 + 三角箭头整块在这里画** —— 这是本批**实测出来的机制**，不是猜的：
    //
    //   ★ 为什么不能靠 QSS：
    //     · `image:` 走不通（实测 QSS 不加载 `url(data:…)`，见类顶注释）；
    //     · `::down-arrow { background: … }` 也不行 —— Qt 自己把那一格涂完
    //       **就不再调本 primitive**（与 `::indicator` 那条坑同源）⇒ 真机上是**空槽**；
    //     · `::drop-down` 规则**只要存在**（哪怕只有 `border: 0`）—— Qt 会接管整个 combo
    //       的绘制、**连箭头都不画**（本批实测四种 QSS 变体，真 combo `grab()` 后数
    //       `text_dim` 像素：带 `::drop-down` 规则 = **0**；去掉 = 有）。
    //   ⇒ 定案（与任务书"箭头优先在 CheckMarkStyle 层重写"一致）：**QSS 里没有 `::drop-down`
    //     规则**、`::down-arrow` 只留几何声明；**槽底与三角都在本 primitive 里画**。
    //   ★ 实测到的 primitive 矩形：`[367,4 10x26]`（widget 381×34 的那个 combo）—— 所以
    //     **尺寸要按"槽宽"算，不能用 `min(w,h)*0.5`**（本批踩过：那样只有 5px 宽、1.5px 高，
    //     真机上几乎看不见，探针里只有 6 个像素）。现在的算式给 10×26 的槽 ⇒ 底 8px、高 4.8px。
    //   ★ 判据（SHELL-SMOKE 下拉①′）：真 combo 的槽里必须量到 ≥6 个 `text_dim` 像素
    //     （实测 19；把 `::drop-down` 规则加回 / 给 `::down-arrow` 加 `background` ⇒ 0）。
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override {
        if (element != PE_IndicatorArrowDown || option == nullptr || painter == nullptr) {
            draw_check_or_base(element, option, painter, widget);
            return;
        }
        const QRect r = option->rect;
        if (r.width() <= 2 || r.height() <= 2) {
            draw_check_or_base(element, option, painter, widget);
            return;
        }
        const QColor slot_fill =
            slot_color_.isValid() ? slot_color_ : option->palette.color(QPalette::Button);
        const QColor edge = edge_color_.isValid() ? edge_color_ : option->palette.color(QPalette::Mid);
        const QColor fill = arrow_color_.isValid() ? arrow_color_ : option->palette.color(QPalette::Text);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);   // "硬朗"＝不羽化
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        // ① 槽底（1.x `TStyle` 的 `background=surface2`）＋ 与主体之间一条 1px 分隔
        painter->fillRect(r, slot_fill);
        painter->setPen(QPen(edge, 1));
        painter->drawLine(r.topLeft(), r.bottomLeft());
        // ② 三角箭头（色＝主题令牌 `text_dim`；底 ≈ 槽宽 ×0.8，高 ≈ 底 ×0.6）
        const qreal base = std::max<qreal>(6.0, std::min<qreal>(r.width() * 0.8, r.height() * 0.6));
        const qreal height = base * 0.6;
        const QPointF c = QRectF(r).center();
        const QPolygonF tri({QPointF(c.x() - base / 2.0, c.y() - height / 2.0),
                             QPointF(c.x() + base / 2.0, c.y() - height / 2.0),
                             QPointF(c.x(), c.y() + height / 2.0)});
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawPolygon(tri);
        painter->restore();
    }

private:
    // 勾选框那一笔（其余 primitive 原样委托给基类）——从 `drawPrimitive` 里拆出来，
    // 让箭头那一条分支能共用同一个出口。
    void draw_check_or_base(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                            const QWidget* widget) const {
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

    QColor arrow_color_;   // 主题令牌 `text_dim`（箭头本体）
    QColor slot_color_;    // 主题令牌 `surface2`（箭头槽底）
    QColor edge_color_;    // 主题令牌 `border`（槽与主体之间的 1px 分隔）
};

// 本进程里那一份 `CheckMarkStyle`（**造一次、复用**）。
// ★ 为什么不靠 `dynamic_cast<CheckMarkStyle*>(app.style())` 去认它：设过样式表之后
//   `QApplication::style()` 返回的是 **QStyleSheetStyle 包装** ⇒ 那个 cast 恒为 nullptr
//   （原句 `if (dynamic_cast<…> == nullptr)` 因此**每次**都为真、每次 apply_theme 都重造一个
//    样式对象 —— 只影响样式对象的身份、不影响观感，所以一直没被注意到）。本批顺手修掉。
CheckMarkStyle* g_check_mark_style = nullptr;

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
    // ★★ P17-96（审计 `03 F-13`）：**字体族也走令牌** —— 修前 QSS 里硬编 `font-family: Consolas;`，
    //   而主题表里本来就有 `font_mono` 角色（`src/core/theme_tables.inc`）⇒ 同一件事两处写死，
    //   改主题不会改到 QSS（"等宽策略"在 `video_page.cpp` 那边又是第三种写法）。
    //   现在从**唯一事实来源**取：`core::theme_font(i)` 里 `role == "font_mono"` 的 `family`。
    const auto font_family = [&tokens](const char* token_name, const char* role) {
        for (int i = 0; i < xrkit::core::theme_font_count(); ++i) {
            const xrkit::core::ThemeFontSpec& spec = xrkit::core::theme_font(i);
            if (spec.role != nullptr && role != nullptr && std::strcmp(spec.role, role) == 0) {
                tokens.emplace_back(QStringLiteral("@{") + QString::fromUtf8(token_name) +
                                        QStringLiteral("}"),
                                    QString::fromUtf8(spec.family != nullptr ? spec.family : ""));
                return;
            }
        }
        // 找不到角色 ⇒ **不许静默**：留一个**必然判红**的令牌（生成样式表里会出现未替换的 `@{`）
        tokens.emplace_back(QStringLiteral("@{") + QString::fromUtf8(token_name) + QStringLiteral("}"),
                            QStringLiteral("@{missing-role:") + QString::fromUtf8(role) +
                                QStringLiteral("}"));
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
    // ★ 2026-10-25：菜单项右侧给子菜单箭头留位用（新增令牌，不是改现有值）
    size("pad24", 24);
    size("r3", 3);
    size("r2", 2);
    size("ind14", 14);
    // ★ 勾选框的"勾"**不走 QSS 图片**（实测 Qt 的样式表不加载 `url(data:...)`），
    //   由 `CheckMarkStyle`（本文件顶部）在选中态画一笔 —— 见那边的说明。
    // 控件高度：28 → 24（用户 2026-09-30："默认有点偏大"⇒ 整体收紧，页面也不再堆得那么高）
    size("ctl", 24);
    // 内容区（包列表/包内树/日志）字号：比界面正文大一档，且走 px() ⇒ 跟着界面大小缩
    size("font_content", 16);;
    // ★★ 2026-10-05（用户第 3 条「所有下拉老样式…1.x 线条比 2.0.0 硬朗多了」）：
    //   下拉箭头**槽**那一格（`::drop-down`）与箭头本身（`::down-arrow`）的尺寸。
    //   1.x `TStyle` 里的 `arrowcolor=text_dim`、`background=surface2`（toolkit_theme.py:311-322）
    //   ⇒ 2.0.0 的对应物就是一条 `QComboBox::down-arrow` 规则；`arrow12` 是那块箭头的槽宽。
    size("arrow12", 12);
    // ★★ P17-96（审计 `03 F-13`）：等宽字体族令牌 —— 值取自主题表 `font_mono`（唯一事实来源）
    font_family("font_mono", "font_mono");
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
               "  border-radius: @{r3}; font-family: @{font_mono}; }"
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
               // ★★ 2026-10-05（用户第 3 条）：**这里刻意没有 `QComboBox::drop-down` 规则** ——
               //   本批实测：那条规则**只要存在**（哪怕只有 `border: 0`），Qt 的样式表样式就
               //   接管整个 combo 的绘制、**连箭头都不画**（真 combo `grab()` 后数 `text_dim`
               //   像素 = **0**）⇒ 真机上是一个**没有箭头的空槽**。
               //   箭头槽（surface2 底）+ 三角（text_dim）现在整块归
               //   `CheckMarkStyle::drawPrimitive(PE_IndicatorArrowDown)`（见那里的实测数据）。
               // ★★ 2026-10-05（用户第 3 条「**所有下拉老样式**…我实际想要的样式是 1.x 里的
               //   那种，**1.x 线条比 2.0.0 硬朗多了**」）：补上 1.x 的**三角箭头**那一套。
               //   1.x（toolkit_theme.py:311-322）的口径是 `arrowcolor=text_dim`、
               //   `background=surface2`（箭头**槽**的底），两样都给出来。
               //   ★ 为什么箭头**不可能**用 QSS 画（本批实测，两处都试过）：
               //     ① `image:` 走不通 —— Qt 的样式表引擎不加载 `url(data:…)` 图片（勾选框那一族
               //        已经踩过同一个坑，见 CheckMarkStyle 顶部）；
               //     ② 给 `::down-arrow` 配 `background` 也不行 —— Qt 会**自己把那一格涂完就完事**、
               //        **根本不再调 `PE_IndicatorArrowDown`**（与 `::indicator` 那条坑同源）
               //        ⇒ 用户在真机上看到的是一个**没有箭头的空槽**（实测：真 combo 槽里
               //        `text_dim` 像素 0 个）。
               //   ⇒ 定案：**槽底 + 三角整块由 `CheckMarkStyle` 在那个 primitive 里画**
               //     （任务书说的"箭头优先在 CheckMarkStyle 层重写"；实测 Qt 对 combo 走的就是
               //     这个 primitive —— 见那里的注释）；本行只留**几何声明**，
               //     让"槽是 12px"这件事在 QSS 里也看得见（自检锁按它断言）。
               //   ★ **不引 accent**：强调色只许出现在三处（主按钮 / 当前标签 / 进度条）。
               "QComboBox::down-arrow { width: @{arrow12}; height: @{arrow12}; }"
               // ★ 2026-10-05：下拉的**弹层**统一到与输入框同一套令牌（原来弹层底是 `surface`、
               //   与"选中的那一行"的灰底 `selected` 撞色 ⇒ 用户说"下拉栏设计太丑"）。
               //   `item` 也补上内距与圆角，与列表项同一套几何。
               "QComboBox QAbstractItemView { background: @{entry_bg}; color: @{text};"
               "  border: 1px solid @{border}; border-radius: 0; outline: none;"
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
               // ★★ 2026-10-05（用户第 3 条同一批）：**补 QMenu 一套**。
               //   修前的事实：整个 QSS 里 `QMenu` **一条规则都没有**（本轮全库核过）⇒
               //   菜单栏的下拉、各页右键菜单全走 Qt 默认外观：**系统蓝高亮** +
               //   有立体感的弹层 —— 正是用户说的"2.0.0 观感不对"。
               //   1.x 的对应物（toolkit_theme.py:317-322 的 TOptionMenu/TMenubutton）：
               //   `background=surface2`、`relief=flat`、`borderwidth=1` + `bordercolor=border`，
               //   选中态用**中性灰**（`selected`，与列表项同一套），**不是**系统蓝。
               //   ★ 为什么把 `relief: flat` 写在这里（而不是只写子控件）：Qt 的 style hint
               //     只从 `QMenu` 这一条规则里认 `relief`，写在别的选择器上等于没写。
               //   ★ 这一条**整块只此一份**（同名选择器后写的赢 ⇒ 绝不再补第二条 QMenu）。
               //   ★ **`QMenu { … }` 里恰好三条**（background / border / relief）：
               //     1.x `TOptionMenu` 的 `background=surface2 + relief=flat + borderwidth=1`。
               //     文字色与内距**不写在这一条里**（写在 `::item` 上）—— 一是 1.x 的口径就是
               //     这三条，二是自检那条锁钉的就是"这一块恰好三条"。多写一条即红。
               "QMenu { background: @{surface2}; border: 1px solid @{border}; relief: flat; }"
               // ★★ 2026-10-25 用户实测（截图）：子菜单的**文字和右侧三角重合** ——
    //   修前 `padding: @{pad4} @{pad8}` 右边只留 8px，而 Qt 在 item 矩形**内部**画子菜单箭头。
    //   现在右边留出箭头宽度；再给箭头一个显式尺寸（不然不同 DPI 下箭头大小与留白对不上）。
    "QMenu::item { color: @{text}; padding: @{pad4} @{pad24} @{pad4} @{pad8}; }"
    "QMenu::right-arrow { width: @{pad8}; height: @{pad8}; margin-right: @{pad4}; }"
               "QMenu::item:selected { background: @{selected}; color: @{text_bright}; }"
               "QMenu::separator { height: 1px; background: @{border}; margin: @{pad2} 0; }"
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
               // ★★ 2026-10-05（用户第 3 条）：这一条**原来写的是 `pad8 @{pad10}`**，
               //   它同名同选择器地**盖掉**了上面那份 `pad4 pad8`（后写的赢）⇒ 生效的一直是
               //   宽内距 10 + 高内距 8，也就是用户说的"2.0.0 不硬朗"。1.x 的
               //   `TCombobox padding=(6,4)` / `TOptionMenu padding=(10,4)` 是**扁的**，
               //   所以这里回到 `pad4 @{pad8}`：与上面那一份**逐字一致**（同类只维护一套）。
               "QComboBox, QLineEdit, QSpinBox { min-height: @{ctl}; padding: @{pad4} @{pad8}; }"
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
    // ★★ 用户 2026-10-25（实测截图）：「目前的**小**才应该是默认（正常）大小」
    //   ⇒ 档位数值不动（仍是 0.9/1.0/1.1/1.2，判据钉着这个集合），只把**标签**顺着改：
    //     0.9 叫「默认」；原来的「默认」(1.0) 顺延成「大」，1.1 → 「特大」，1.2 → 「超大」。
    static const std::vector<UiSizeChoice> kChoices = {{"默认", 0.9}, {"大", 1.0},
                                                       {"特大", 1.1}, {"超大", 1.2}};
    return kChoices;
}

void apply_ui_size(QApplication& app, double scale) {
    if (scale <= 0.0) return;
    const double base = ui_size_base_font_pt();
    xrkit::core::set_ui_scale(scale);   // 控件尺寸（px() 全部按它算）
    // 字体由 `apply_theme` 统一按 ui_scale 设（见那边的 setPointSizeF）；这里只重套主题。
    Q_UNUSED(base);
    // ★★ 2026-10-06（H-2）：模式**从 core 取**，不再读 `app.property("xrkit_theme")`。
    //   修前：那个属性只在 `Hub::apply_theme_mode()`（用户手动切主题）里写，**启动路径从不写**
    //   ⇒ 浅色用户第一次点「界面大小」读到空串 ⇒ 走 Dark ⇒ 整界面被拍回暗色
    //   （用户红线："主题和字体大小要保留继承"）。`apply_theme` 内部会
    //   `core::set_theme_mode(mode)`，所以 core 这份是**唯一真值**，启动路径也一定写过。
    //   判红锁：`src/shell/smoke.cpp` 的「④-c」——预置浅色 + 清掉那个属性（如实重现启动态）
    //   ⇒ 点「界面大小」后必须仍是浅色（改回读属性立刻红）。
    apply_theme(app, xrkit::core::theme_mode());
}

// ── 启动序列：把 user.ltx 里的「主题 / 界面大小」铺上（**唯一一份实现**）──────────
//
// ★★ 2026-10-06（H-1）：这段原先**内联在 `src/shell/main.cpp` 的 main() 里**，于是
//   "字体与几何必须同源""界面大小不许翻主题"这两条判据在离屏自检里**没法复现**
//   （`--smoke` 走的是另一条分支，压根不经过交互式启动那段）。**口径一字不改，只是搬到
//   能被调用处**，`main.cpp` 与 `smoke.cpp` 调同一个函数 ⇒ 锁钉的就是真实启动路径。
//
// 顺序（有意如此，别改）：
//   ① 先按 user.ltx 的偏好铺一次主题 —— 控件构造之前界面就得有主题；
//   ② 再让屏幕自适应给出"没有偏好时"的基准（并吃掉 DPI 检测的副作用）；
//   ③ 有保存档时用它覆盖 ②（用户上次选的「界面大小」压过屏幕自适应）；
//   ④ ★ **必须再套一次主题**：`apply_theme` 里的字号是 `(基准 + 2.0) * ui_scale()`，
//      而 ① 那次是按**旧的** ui_scale 算的 ⇒ 少这一刀就是"几何按 1.2、字体按 1.0"
//      （用户红线："字体大小也要保留继承"）。两次调用幂等：字号基准取自一次性 static，
//      不会连乘（见 `apply_theme` 里 kBasePt 的说明）。
xrkit::core::ThemeMode apply_startup_ui(QApplication& app) {
    const xrkit::core::ThemeMode mode = xrkit::core::load_theme_mode();
    apply_theme(app, mode);
    const double screen_scale = apply_screen_ui_scale();
    const double saved_scale = xrkit::core::load_ui_scale();
    if (saved_scale > 0.0) {
        xrkit::core::set_ui_scale(saved_scale);
    } else {
        // ★★ 2026-10-25 用户裁定：「目前的**小**才应该是默认（正常）大小」。
        //   没有偏好时不再直接用 1.0，而是 **0.9 × 屏幕系数**（高 DPI 机器保留放大，
        //   但在 100% 缩放的本机上就是 0.9）。用户手动选过的档位仍然原样生效。
        xrkit::core::set_ui_scale(screen_scale > 1.0 ? screen_scale * 0.9 : 0.9);
    }
    apply_theme(app, mode);
    return mode;
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
        g_check_mark_style = new CheckMarkStyle(fusion);
        app.setStyle(g_check_mark_style);
    }
    // ★★ 2026-10-05（用户第 3 条）：**每次 apply_theme 都把下拉箭头的颜色刷一遍** ——
    //   样式对象是复用的（上面那个 `dynamic_cast` 只造一次），只造的时候设一次的话，
    //   切主题后箭头还是旧主题的色。真值＝主题令牌 `text_dim`（见 CheckMarkStyle）。
    //   ★ 注意：**必须用 `g_check_mark_style` 这个指针**，不能用
    //     `dynamic_cast<CheckMarkStyle*>(app.style())` —— 设过样式表之后
    //     `QApplication::style()` 返回的是 **QStyleSheetStyle 包装**，那个 cast 恒为 nullptr
    //     （这也正是上面那句 `== nullptr` 判据**一直成立**、每次都重造样式对象的原因；
    //      本批顺手把它改成"真只造一次"）。
    if (g_check_mark_style != nullptr) {
        g_check_mark_style->set_arrow_colors(role("text_dim"), role("surface2"), role("border"));
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
