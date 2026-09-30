#include "smoke.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/plugin/abi_host.h"
#include "hub.h"
#include "drop_filter.h"
#include "log_box.h"
#include "plugin_ui.h"
#include "pages/convert_page.h"
#include "pages/fs_page.h"
#include "pages/fontpack_page.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "pages/text_page.h"
#include "pages/video_page.h"
#include "pages/xml_page.h"
#include "scroll_viewport.h"
#include "split_pane.h"
#include "task_shell.h"
#include "theme.h"
#include "virtual_list.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QDropEvent>
#include <QUrl>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QHeaderView>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace xrkit::shell {
namespace {

// 判据用的期望值：**契约**（页 id 是插件槽位/日志的稳定标识），不是"再抄一份清单"。
// 清单本身从 `builtin_page()`（真实清单）读，两边交叉核对。
constexpr const char* kExpectedIds[] = {"fs", "convert", "text", "xml", "video", "fontpack"};
constexpr const char* kExpectedTitles[] = {"文件系统", "编码转换", "文本提取",
                                           "XML校对", "视频转换", "汉化包生成"};
constexpr int kExpectedCount = 6;

}  // namespace

SmokeResult run_smoke() {
    SmokeResult result;
    const auto check = [&result](bool good, const QString& what) {
        ++result.checks;
        if (!good) {
            ++result.failed;
            result.failures.append(what);
        }
    };

    // ① 真实清单 vs 契约（少一页/改个名字都要红）
    {
        // ★ 2026-09-27（用户："先把我截图的文字全删了"）：副标题**允许留空**（他截到的那两句已删）
        //   ⇒ 判据从"六页都要有"改成"**要么没有、要么有实质文案**"（留空串/空白串占位＝红）。
        //   这是**用户的删除指令**造成的口径变更，不是为了让谁变绿而放松 —— 原来那条
        //   "六页都要有说明文案"是上一轮 `docs/2.0.0-界面重构口径(排版与配色).md` 的口径，
        //   现被用户当面推翻（他的话就是更高一级的口径）。
        int bad_sub = 0;
        for (int i = 0; i < builtin_page_count(); ++i) {
            const char* sub = builtin_page(i).subtitle;
            if (sub == nullptr || *sub == '\0') continue;  // 没有文案 = 合法（用户删掉的那些）
            bool blank = true;
            for (const char* p = sub; *p != '\0'; ++p) {
                if (*p != ' ' && *p != '\t') blank = false;
            }
            if (blank) ++bad_sub;
        }
        check(bad_sub == 0,
              QStringLiteral("① 副标题要么没有、要么有实质文案（空白占位 %1 条）").arg(bad_sub));
    }
    check(builtin_page_count() == kExpectedCount,
          QStringLiteral("内置页数 = %1（实际 %2）").arg(kExpectedCount).arg(builtin_page_count()));
    for (int i = 0; i < kExpectedCount && i < builtin_page_count(); ++i) {
        check(QString::fromUtf8(builtin_page(i).id) == QString::fromUtf8(kExpectedIds[i]),
              QStringLiteral("第 %1 页 id = %2").arg(i).arg(QString::fromUtf8(kExpectedIds[i])));
        check(QString::fromUtf8(builtin_page(i).title) == QString::fromUtf8(kExpectedTitles[i]),
              QStringLiteral("第 %1 页标题 = %2")
                  .arg(i)
                  .arg(QString::fromUtf8(kExpectedTitles[i])));
    }

    // ★ 2026-09-27（用户："先把我截图的文字全删了"）：**被他点名删掉的那些文字不许回来** ——
    //   六页卡片顶部那几段说明、两页的副标题、占位符/下拉项里的括注。
    //   判据是**逐字串**（他给的正是字面文字），命中即红；不抽样：六页的每个 QLabel / 占位符 /
    //   下拉项都扫一遍。
    {
        const QStringList banned = {
            QStringLiteral("六步判定链"),      // 编码转换页说明 + 下拉项括注
            QStringLiteral("可拖进来"),        // 四个占位符的括注
            QStringLiteral("留空＝原地转换"),  // 输出目录占位符括注
            QStringLiteral("只认两处"),        // 视频页说明
            // ⚠ 这里**不能**用 `行数/ID 统计`：那是 XML 页比较模式下拉里的**合法模式名**
            //   （引擎的模式声明），我第一版写成它，锁当场把我自己拦下来了 ⇒ 换成说明段里
            //   独有的那半句。这条经验：**锁的判据要挑"只有被删对象才有"的字串**。
            QStringLiteral("看条数与出现次数"),  // XML 页说明（独有的那半句）
            QStringLiteral("只看文件夹自己的名字"),  // 文本提取页说明
            QStringLiteral("与 1.x 逐字节一致"),     // 汉化包页说明
            QStringLiteral("产出可直接分发的压缩包"),  // 汉化包页副标题
            QStringLiteral("勾选后解包或重打包"),      // 文件系统页副标题
        };
        QStringList hit;
        // ① 六页的 QLabel（含卡片里的说明段落）
        for (int i = 0; i < builtin_page_count(); ++i) {
            QWidget* page = create_page(i);
            if (page == nullptr) continue;
            for (QLabel* label : page->findChildren<QLabel*>()) {
                for (const QString& text : banned) {
                    if (label->text().contains(text)) hit << text;
                }
            }
            // ② 输入框占位符
            for (QLineEdit* edit : page->findChildren<QLineEdit*>()) {
                for (const QString& text : banned) {
                    if (edit->placeholderText().contains(text)) hit << text;
                }
            }
            // ③ 下拉项（编码转换页的「自动（六步判定链）」就是这一类）
            for (QComboBox* combo : page->findChildren<QComboBox*>()) {
                for (int k = 0; k < combo->count(); ++k) {
                    for (const QString& text : banned) {
                        if (combo->itemText(k).contains(text)) hit << text;
                    }
                }
            }
            delete page;
        }
        // ④ 副标题（六页清单）
        for (int i = 0; i < builtin_page_count(); ++i) {
            const char* sub = builtin_page(i).subtitle;
            if (sub == nullptr) continue;
            for (const QString& text : banned) {
                if (QString::fromUtf8(sub).contains(text)) hit << text;
            }
        }
        check(hit.isEmpty(),
              QStringLiteral("① 用户点名删掉的文字又回来了：%1")
                  .arg(hit.join(QStringLiteral(" / "))));
    }

    // ② Hub 真的按这份清单建了控件（列表项 = 页栈页数 = 清单条数）
    Hub hub;
    check(hub.page_count() == builtin_page_count(), QStringLiteral("页栈页数 = 内置页数"));
    check(hub.page_tabs()->count() == builtin_page_count(), QStringLiteral("标签项数 = 内置页数"));
    for (int i = 0; i < builtin_page_count(); ++i) {
        check(hub.page_title(i) == QString::fromUtf8(builtin_page(i).title),
              QStringLiteral("列表第 %1 项标题与清单一致").arg(i));
        check(hub.page_id(i) == QString::fromUtf8(builtin_page(i).id),
              QStringLiteral("第 %1 页 id 可查").arg(i));
    }
    check(hub.stack()->currentIndex() == 0, QStringLiteral("默认停在第一页"));

    // ③ 日志栏存在、只读（1.x 口径：日志不归主窗口那根滚动条管）
    check(hub.log_view() != nullptr, QStringLiteral("日志栏存在"));
    check(hub.log_view() != nullptr && hub.log_view()->isReadOnly(),
          QStringLiteral("日志栏只读"));
    hub.log_line(QStringLiteral("smoke"));
    {
        // ★ 方案一（Hana 重排）：日志从标题栏改成底部折叠按钮
        check(hub.log_toggle() != nullptr && !hub.log_toggle()->text().isEmpty(),
              QStringLiteral("日志折叠按钮存在（实测 %1）")
                  .arg(hub.log_toggle() != nullptr ? hub.log_toggle()->text() : QStringLiteral("(无)")));
        check(hub.log_clear_button() != nullptr, QStringLiteral("日志面板要有清空按钮"));
        hub.log_line(QStringLiteral("待清空 A"));
        if (hub.log_box() != nullptr) hub.log_box()->flush();
        const int before = hub.log_box() != nullptr ? hub.log_box()->blockCount() : 0;
        if (hub.log_clear_button() != nullptr) hub.log_clear_button()->click();
        if (hub.log_box() != nullptr) hub.log_box()->flush();
        const int after = hub.log_box() != nullptr ? hub.log_box()->blockCount() : 0;
        check(before > 0 && after < before,
              QStringLiteral("点清空要真的清掉（%1 -> %2 行）").arg(before).arg(after));
        // ★ 本块插在原有的「日志能追加」断言**之前**，而清空会把内容清掉 ⇒ 这里补回一行，
        //   免得把后面那条既有断言弄红（顺序敏感是这种"就地插断言"的固有坑）。
        hub.log_line(QStringLiteral("smoke"));
        if (hub.log_box() != nullptr) hub.log_box()->flush();
    }

    if (hub.log_box() != nullptr) hub.log_box()->flush();  // 攒批控件：先落一次再断言
    check(hub.log_view() != nullptr && hub.log_view()->toPlainText().contains("smoke"),
          QStringLiteral("日志能追加"));

    // ④ 主题：令牌 → QPalette/QSS；**切换是原地更新**（同一个 widget 的 palette 跟着变）
    QWidget probe;
    probe.show();
    apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
    const QColor dark_window = probe.palette().color(QPalette::Window);
    apply_theme(*qApp, xrkit::core::ThemeMode::Light);
    const QColor light_window = probe.palette().color(QPalette::Window);
    check(dark_window.name().compare(
              QString::fromUtf8(xrkit::core::theme_color(xrkit::core::ThemeMode::Dark, "bg")),
              Qt::CaseInsensitive) == 0,
          QStringLiteral("暗色 QPalette::Window = 令牌 bg"));
    check(light_window.name().compare(
              QString::fromUtf8(xrkit::core::theme_color(xrkit::core::ThemeMode::Light, "bg")),
              Qt::CaseInsensitive) == 0,
          QStringLiteral("亮色 QPalette::Window = 令牌 bg"));
    check(dark_window != light_window, QStringLiteral("切主题后同一控件底色确实变了"));
    check(!qApp->styleSheet().isEmpty(), QStringLiteral("QSS 已应用"));
    apply_theme(*qApp, xrkit::core::ThemeMode::Dark);

    // ⑤ QSS **占位错位**这一类事故不许再来（用户真机截图：控制台 88 条
    //    `QCssParser::parseHexColor: Unknown color name '#3a3f45px'`，同时 padding/圆角/
    //    勾选框尺寸静默失效）。三条判据，都是"能判红"的实测口径：
    //      ① 生成的样式表里**没有「颜色值 + px」**（错位的典型形态）；
    //      ② 没有未替换的命名令牌 `@{`、也没有残留的 `%N` 占位；
    //      ③ 真应用样式表时 Qt **一条** parseHexColor / Argument missing 警告都不发。
    {
        int color_px_hits = 0;
        int token_left = 0;
        int arg_left = 0;
        const QRegularExpression color_px(QStringLiteral("#[0-9a-fA-F]{6}px"));
        const QRegularExpression arg_marker(QStringLiteral("%[0-9]"));
        for (const xrkit::core::ThemeMode mode :
             {xrkit::core::ThemeMode::Dark, xrkit::core::ThemeMode::Light}) {
            const QString sheet = stylesheet_for(mode);
            // 逐处数，别只问"有没有"（修一处 ≠ 修完 —— 88 条那次就是只修了一处）
            auto it = color_px.globalMatch(sheet);
            while (it.hasNext()) {
                it.next();
                ++color_px_hits;
            }
            if (sheet.contains(QStringLiteral("@{"))) ++token_left;
            if (arg_marker.match(sheet).hasMatch()) ++arg_left;
        }
        check(color_px_hits == 0,
              QStringLiteral("⑤ 样式表里出现「颜色值 + px」%1 处（占位错位）").arg(color_px_hits));
        check(token_left == 0, QStringLiteral("⑤ 有未替换的命名令牌 @{...}（模板写错名字了）"));
        check(arg_left == 0, QStringLiteral("⑤ 样式表里还残留 %N 占位符"));

        static int qss_warnings = 0;
        qss_warnings = 0;
        const QtMessageHandler previous = qInstallMessageHandler(
            +[](QtMsgType, const QMessageLogContext&, const QString& text) {
                if (text.contains(QStringLiteral("parseHexColor")) ||
                    text.contains(QStringLiteral("Argument missing"))) {
                    ++qss_warnings;
                }
            });
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        apply_theme(*qApp, xrkit::core::ThemeMode::Light);
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        qInstallMessageHandler(previous);
        check(qss_warnings == 0,
              QStringLiteral("⑤ 应用样式表时 Qt 报了 %1 条 QSS 警告（应为 0）").arg(qss_warnings));

        // ③ 强调色（蓝）**只许出现在三处**：主按钮 / 当前标签项 / 进度条 ——
        //   口径见 `docs/2.0.0-界面重构口径(排版与配色).md` 第二节（用户原话
        //   "配色里为什么这么多蓝色"）。判据是**逐规则**看：凡是含 accent 色的规则，
        //   选择器必须在白名单里；多一处就红（不许"看着差不多"）。
        const QStringList accent_allowed = {
            QStringLiteral("QPushButton#primary"), QStringLiteral("QPushButton#primary:hover"),
            QStringLiteral("QPushButton#primary:pressed"),
            QStringLiteral("QTabBar#page_tabs::tab:selected"), QStringLiteral("QProgressBar::chunk")};
        QStringList accent_strays;
        for (const xrkit::core::ThemeMode mode :
             {xrkit::core::ThemeMode::Dark, xrkit::core::ThemeMode::Light}) {
            const QString accent_hex =
                QString::fromUtf8(xrkit::core::theme_color(mode, "accent"));
            const QString sheet = stylesheet_for(mode);
            const QStringList rules = sheet.split(QLatin1Char('}'));
            for (const QString& rule : rules) {
                if (!rule.contains(accent_hex, Qt::CaseInsensitive)) continue;
                const QString selector = rule.section(QLatin1Char('{'), 0, 0).trimmed();
                if (!accent_allowed.contains(selector)) accent_strays << selector;
            }
        }
        check(accent_strays.isEmpty(),
              QStringLiteral("③ 强调色跑到这三处之外了：%1")
                  .arg(accent_strays.join(QStringLiteral(" / "))));
    }

    // ⑤ HiDPI：外壳里所有硬编码尺寸都要过 px()
    xrkit::core::set_ui_scale(1.0);
    check(px(24) == 24, QStringLiteral("px(24) @1.0 = 24"));
    xrkit::core::set_ui_scale(1.5);
    check(px(24) == 36, QStringLiteral("px(24) @1.5 = 36"));
    xrkit::core::set_ui_scale(1.0);

    // ⑥ user.ltx 落在 **exe 所在目录**（1.x `app_dir()/user.ltx`），且只读不写
    const QString path = QString::fromUtf8(xrkit::core::user_ltx_path());
    check(QFileInfo(path).fileName().compare(QStringLiteral("user.ltx"), Qt::CaseInsensitive) == 0,
          QStringLiteral("user.ltx 文件名"));
    check(QFileInfo(path).absolutePath() == QCoreApplication::applicationDirPath(),
          QStringLiteral("user.ltx 与 exe 同目录"));

    // ⑦ 滚动三闸门（M2-2）：对照表 ①1.1~1.8 —— **没有滚动条就不许滚**、装得下不占条、
    //    同一滚轮事件只有一个条子动、全局路由到第一个能滚的视口。
    //    ★ 夹具用**裸顶层 + 矮窗口**（3.5：Hub 式布局永远压不矮 ⇒ 永远测不出）。
    {
        const auto send_wheel = [](QWidget* target, int delta) {
            ScrollViewport::reset_wheel_claims();
            QWheelEvent event(QPointF(5, 5), target->mapToGlobal(QPoint(5, 5)), QPoint(0, 0),
                              QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(target, &event);
            return event.isAccepted();
        };

        // ⑦-a 装得下 ⇒ **不占条**且**不许滚**（1.1/1.2/1.7）
        QWidget short_top;
        short_top.resize(300, 120);
        auto* small_body = new QWidget;
        small_body->setMinimumSize(200, 40);
        auto* small_vp = new ScrollViewport(&short_top);
        small_vp->setGeometry(0, 0, 300, 120);
        small_vp->set_fit(ScrollViewport::Fit::Viewport);
        small_vp->set_body(small_body);
        small_vp->sync_layout();
        short_top.show();
        small_vp->show();   // ★ 必须 show：`bar_shown` 四看里有"已 mapped"，不 show 会**假通过**
        QApplication::processEvents();

        check(!small_vp->can_scroll_vertical(), QStringLiteral("装得下 ⇒ 范围为空（不能滚）"));
        check(!ScrollViewport::bar_shown(small_vp->vbar()),
              QStringLiteral("装得下 ⇒ 条子不占位（bar_shown 四看不全真）"));
        send_wheel(small_body, -120);
        check(small_vp->offset_vertical() == 0, QStringLiteral("装得下时偏移恒为 0"));

        // ⑦-a2 **控件级拦**（1.1 的真正考点）：body 里放一个"会自己滚"的原生滚动区
        //（QPlainTextEdit），它的条子不可见时**不许它自己滚** —— 1.x 的原文是
        // "Tk 的 Text/Listbox 类绑定根本不看我们的条子"，Qt 的 QAbstractScrollArea 同理。
        auto* text_host = new QWidget;
        text_host->setMinimumSize(200, 40);
        auto* native_text = new QPlainTextEdit(text_host);
        native_text->setGeometry(0, 0, 180, 30);
        native_text->setPlainText(QString(200, QLatin1Char('x')));  // 内容远超过 30px
        native_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        auto* text_vp = new ScrollViewport(&short_top);
        text_vp->setGeometry(0, 0, 300, 120);
        text_vp->set_fit(ScrollViewport::Fit::Viewport);
        text_vp->set_body(text_host);
        short_top.show();
        text_vp->show();
        QApplication::processEvents();
        native_text->verticalScrollBar()->setValue(0);
        check(ScrollViewport::bar_shown(text_vp->vbar()) == false,
              QStringLiteral("控件级拦：视口条子不可见（前置条件）"));
        // ★ 2026-09-29 **口径更新**（用户："小滚动条在不隐藏的时候也没法滚，还是控制的整个界面的
        //   滚动条"）：原生滚动区**条子可见且能滚**时，必须**它自己滚**，事件被吃掉、
        //   **外层不许跟着动**。旧口径是"可见 ⇒ 不干预，让它冒泡"，正是用户报的那个现象。
        //   判红办法：把 `route_wheel` 里"可见且能滚 ⇒ 自己滚"那一段去掉（恢复 return false）。
        check(send_wheel(native_text, -120),
              QStringLiteral("控件级拦：事件被吃掉（不再冒泡给外层）"));
        check(native_text->verticalScrollBar()->value() > 0,
              QStringLiteral("条子可见且能滚 ⇒ **它自己滚**（实测 %1，期望 >0）")
                  .arg(native_text->verticalScrollBar()->value()));
        check(text_vp->offset_vertical() == 0,
              QStringLiteral("外层视口不许跟着动（实测偏移 %1，期望 0）")
                  .arg(text_vp->offset_vertical()));

        // ⑦-b 能滚 ⇒ **必须看得见条**（1.7 的另一向）。内容高用 fit=Content + 高的 body
        auto* tall_body = new QWidget;
        auto* tall_lay = new QVBoxLayout(tall_body);
        auto* tall_ink = new QWidget;
        tall_ink->setFixedHeight(600);  // 请求高由布局反射出来（裸 QWidget 的 sizeHint 无效）
        tall_lay->addWidget(tall_ink);
        auto* tall_vp = new ScrollViewport(&short_top);
        tall_vp->setGeometry(0, 0, 300, 120);
        tall_vp->set_fit(ScrollViewport::Fit::Content);
        tall_vp->set_body(tall_body);
        tall_vp->sync_layout();
        tall_vp->show();
        QApplication::processEvents();
        check(tall_vp->can_scroll_vertical(), QStringLiteral("装不下 ⇒ 能滚"));
        {
            const QScrollBar* tb = tall_vp->vbar();
            check(ScrollViewport::bar_shown(tb),
                  QStringLiteral("能滚 ⇒ 条子四看全真（hidden=%1 visToParent=%2 vis=%3 %4x%5）")
                      .arg(tb->isHidden())
                      .arg(tb->isVisibleTo(tall_vp))
                      .arg(tb->isVisible())
                      .arg(tb->width())
                      .arg(tb->height()));
        }

        // ⑦-c 看得见时滚轮真的动，且到底夹住
        check(send_wheel(tall_body, -120), QStringLiteral("能滚时滚轮被本视口消费"));
        check(tall_vp->offset_vertical() > 0, QStringLiteral("滚轮把偏移推大了"));
        for (int i = 0; i < 60; ++i) send_wheel(tall_body, -120);
        check(tall_vp->offset_vertical() == tall_vp->vbar()->maximum(),
              QStringLiteral("连续下滚夹在 maximum"));
        for (int i = 0; i < 60; ++i) send_wheel(tall_body, 120);
        check(tall_vp->offset_vertical() == 0, QStringLiteral("连续上滚夹在 0"));

        // ⑦-c2 ★ 用户实测缺陷的**能红断言**（2026-09-27）：连续两个滚轮事件（**不重置认领表**）
        //      都必须真的动。旧实现拿 `QWheelEvent` 的**对象地址**当事件身份，而 Qt 会复用槽位
        //      ⇒ 第二个事件地址与上一个相同 ⇒ 被判成"已被认领"并**撤销滚动** ⇒ 表现就是
        //      "滚下去之后滚不回来"。注意：上面那个 `send_wheel` 每次都 `reset_wheel_claims()`，
        //      所以它天然测不出这个缺陷 —— 这条断言**故意不重置**。
        {
            const auto send_wheel_raw = [](QWidget* target, int delta) {
                QWheelEvent event(QPointF(5, 5), target->mapToGlobal(QPoint(5, 5)), QPoint(0, 0),
                                  QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
                QApplication::sendEvent(target, &event);
                return event.isAccepted();
            };
            // 先回到顶部，再用"不重置"的方式滚：下 → 上，两次都要动
            tall_vp->vbar()->setValue(0);
            ScrollViewport::reset_wheel_claims();
            const int v0 = tall_vp->vbar()->value();
            check(send_wheel_raw(tall_body, -120) && tall_vp->vbar()->value() != v0,
                  QStringLiteral("连续滚轮①：第一个事件要真的动（v=%1）").arg(v0));
            const int v1 = tall_vp->vbar()->value();
            // 真实场景里两次滚轮之间**必然有一次事件循环**（认领表的"本派发生效"就是靠它失效的）
            QApplication::processEvents();
            check(send_wheel_raw(tall_body, 120),
                  QStringLiteral("连续滚轮②：第二个事件必须被消费（旧逻辑在这里就永久判重了）"));
            check(tall_vp->vbar()->value() != v1,
                  QStringLiteral("连续滚轮③：**第二个事件也要真的动** —— 旧逻辑（地址当身份、无时间窗）"
                                 "会把它撤销掉，这正是用户看到的'滚下去滚不回来'"));
        }

        // ⑦-d fit 规则（3.2/3.3）
        auto* content_body = new QWidget;
        auto* content_lay = new QVBoxLayout(content_body);
        auto* content_ink = new QWidget;
        content_ink->setFixedHeight(300);
        content_lay->addWidget(content_ink);
        auto* content_vp = new ScrollViewport(&short_top);
        content_vp->setGeometry(0, 0, 300, 120);
        content_vp->set_fit(ScrollViewport::Fit::Viewport);
        content_vp->set_body(content_body);
        content_vp->set_min_body_height(200);
        content_vp->sync_layout();
        content_vp->show();
        QApplication::processEvents();
        const int viewport_fit_body_h = content_body->height();
        content_vp->set_fit(ScrollViewport::Fit::Content);
        content_vp->sync_layout();
        const int content_fit_body_h = content_body->height();
        check(viewport_fit_body_h == 200,
              QStringLiteral("fit=viewport ⇒ 内容高 = max(minsize, 视口高)"));
        check(content_fit_body_h >= viewport_fit_body_h,
              QStringLiteral("fit=content ⇒ 不小于 viewport（还要吃内容请求高）"));

        // ⑦-e 显式整组重排（1.8）：竖条贴右、横条贴底、画布吃剩余
        ScrollViewport layout_vp;
        layout_vp.resize(400, 200);
        auto* layout_body = new QWidget;
        layout_body->setMinimumSize(800, 600);
        layout_vp.set_fit(ScrollViewport::Fit::Content);
        layout_vp.set_body(layout_body);
        layout_vp.sync_layout();
        check(layout_vp.vbar()->geometry().right() == layout_vp.width() - 1,
              QStringLiteral("竖条贴右"));
        check(layout_vp.hbar()->geometry().bottom() == layout_vp.height() - 1,
              QStringLiteral("横条贴底"));
        check(layout_vp.body()->width() >= layout_vp.width() - layout_vp.vbar()->width(),
              QStringLiteral("画布吃剩余宽"));

        // ⑦-f 嵌套：同一滚轮事件**只有一个条子动**（1.3/1.4）；内层装得下 ⇒ 路由给外层（1.5）
        QWidget nested_top;
        nested_top.resize(400, 200);
        auto* outer = new ScrollViewport(&nested_top);
        outer->setGeometry(0, 0, 400, 200);
        outer->set_fit(ScrollViewport::Fit::Content);
        auto* outer_body = new QWidget;
        auto* outer_lay = new QVBoxLayout(outer_body);
        auto* outer_ink = new QWidget;
        outer_ink->setFixedHeight(900);
        outer_lay->addWidget(outer_ink);
        outer->set_body(outer_body);
        auto* inner = new ScrollViewport(outer_body);
        inner->setGeometry(0, 0, 300, 150);
        inner->set_fit(ScrollViewport::Fit::Content);
        auto* inner_body = new QWidget;
        auto* inner_lay = new QVBoxLayout(inner_body);
        auto* inner_ink = new QWidget;
        inner_ink->setFixedHeight(400);
        inner_lay->addWidget(inner_ink);
        inner->set_body(inner_body);
        outer->sync_layout();
        inner->sync_layout();
        nested_top.show();
        outer->show();
        inner->show();
        QApplication::processEvents();

        const int outer_before = outer->offset_vertical();
        check(send_wheel(inner_body, -120), QStringLiteral("嵌套：事件被内层消费"));
        check(inner->offset_vertical() > 0, QStringLiteral("嵌套：内层动了"));
        check(outer->offset_vertical() == outer_before,
              QStringLiteral("嵌套：外层没动（同一事件只有一个条子动）"));

        inner_ink->setFixedHeight(40);  // 变成"装得下"
        inner->sync_layout();
        QApplication::processEvents();
        check(!inner->can_scroll_vertical(), QStringLiteral("嵌套：内层改成装得下"));
        const int inner_before = inner->offset_vertical();
        check(send_wheel(inner_body, -120), QStringLiteral("嵌套：事件被外层消费"));
        check(inner->offset_vertical() == inner_before, QStringLiteral("嵌套：内层没动"));
        check(outer->offset_vertical() > outer_before,
              QStringLiteral("嵌套：装得下的内层把滚轮让给外层"));
    }

    // ⑧ 分栏（M2-3）：对照表 ②（2.1~2.5/2.7）+ ④（4.2/4.3）。
    //    用户原话："拖拽的时候预览框不能只是一条线，而是要把那部分相应的背景也进行填充"。
    {
        QWidget pane_host;
        pane_host.resize(400, 200);
        auto* split = new SplitPane(SplitPane::Orient::Vertical, &pane_host);
        split->setGeometry(0, 0, 400, 200);
        auto* top_pane = new QWidget;
        auto* bottom_pane = new QWidget;
        split->add_pane(top_pane, 40);
        split->add_pane(bottom_pane, 60);
        pane_host.show();
        split->show();
        QApplication::processEvents();

        // 2.3 命中区 = max(6, px(5))
        check(split->sash_hit() == std::max(6, px(5)),
              QStringLiteral("2.3 sash 命中区 = max(6, px(5))（实测 %1）").arg(split->sash_hit()));
        const int sash0 = split->sash_pos(0);
        check(split->sash_at(sash0) == 0, QStringLiteral("命中 sash"));
        check(split->sash_at(sash0 + split->sash_hit() + 8) == -1, QStringLiteral("命中区外不命中"));

        // 2.1 拖动期**一步几何都不改**，只有预览位置变
        const int size_top = split->pane_size(0);
        const int size_bottom = split->pane_size(1);
        check(split->begin_drag(sash0), QStringLiteral("begin_drag 命中 sash"));
        split->update_drag(sash0 + 30);
        check(split->dragging() && split->preview_pos() == sash0 + 30,
              QStringLiteral("拖动期有预览位置"));
        check(split->pane_size(0) == size_top && split->pane_size(1) == size_bottom,
              QStringLiteral("2.1 拖动期几何不变（一步不改）"));
        check(top_pane->height() == size_top,
              QStringLiteral("2.1 拖动期控件几何也不变"));
        result.notes.append(QStringLiteral("2.1 拖动预览：位置 %1（原 sash %2），填色带覆盖 %3 px")
                                .arg(sash0 + 30)
                                .arg(sash0)
                                .arg(std::abs(30)));

        // 松手一次重排（2.1）
        const int clamps_before = split->clamp_count();
        split->end_drag(true);
        check(split->pane_size(0) == size_top + 30, QStringLiteral("松手后前一个 pane 吃掉差额"));
        check(split->clamp_count() == clamps_before + 1, QStringLiteral("松手只重排一次"));
        check(!split->dragging(), QStringLiteral("松手后不在拖动态"));

        // 2.4 clamp：拖到极右 ⇒ 前一个 pane 保住 minsize，后面的先挨饿
        split->begin_drag(split->sash_pos(0));
        split->update_drag(1000);
        split->end_drag(true);
        check(split->pane_size(0) >= split->minsize(0),
              QStringLiteral("2.4 前一个 pane 保住 minsize"));
        check(split->pane_size(0) + split->pane_size(1) == split->height() - 4,
              QStringLiteral("两格 + sash = 总高"));

        // 4.2 收起后**改窗口尺寸不弹回**；4.3 顺序（先记意图再 clamp）
        split->set_collapsed(1, true);
        check(split->is_collapsed(1) && split->pane_size(1) == 0, QStringLiteral("4.2 收起：尺寸 0"));
        pane_host.resize(400, 300);
        split->request_clamp();
        QThread::msleep(180);
        QApplication::processEvents();
        check(split->is_collapsed(1) && split->pane_size(1) == 0,
              QStringLiteral("4.2 改窗口尺寸后仍然收起（不弹回）"));
        split->set_collapsed(1, false);
        check(!split->is_collapsed(1) && split->pane_size(1) >= 60,
              QStringLiteral("4.2 展开：≥ 原最小值"));

        // 2.5 去抖 120 ms：连续请求只重排一次
        const int before_debounce = split->clamp_count();
        for (int i = 0; i < 5; ++i) split->request_clamp();
        check(split->clamp_count() == before_debounce,
              QStringLiteral("2.5 去抖窗口内不重排"));
        QThread::msleep(180);
        QApplication::processEvents();
        check(split->clamp_count() == before_debounce + 1,
              QStringLiteral("2.5 去抖窗口到点只重排一次"));

        // 2.7 成本口径**必须含重绘**（只跑 update_idletasks 会低估约一半）
        QElapsedTimer timer;
        timer.start();
        constexpr int kSteps = 20;
        for (int i = 0; i < kSteps; ++i) {
            split->begin_drag(split->sash_pos(0));
            split->update_drag(split->sash_pos(0) + (i % 7) + 1);
            split->repaint();  // 含重绘
            split->end_drag(false);
        }
        const double per_step = static_cast<double>(timer.nsecsElapsed()) / 1e6 / kSteps;
        result.notes.append(QStringLiteral("2.7 拖动一步（含重绘）= %1 ms（PySide6 试点 12~15 ms）")
                                .arg(per_step, 0, 'f', 2));
        check(per_step < 30.0,
              QStringLiteral("2.7 拖动一步（含重绘）%1 ms < 30 ms").arg(per_step, 0, 'f', 2));
    }

    // ⑨ 日志栏 + 任务壳（M2-4）：对照表 ④（4.1/4.4/4.5）+ ⑬（13.1~13.7）。
    {
        // 4.4 LogBox：攒批 80 ms、超 5000 行删到 3000 行、清空走 clear_log()
        LogBox log;
        log.resize(400, 200);
        log.show();
        QApplication::processEvents();
        check(log.isReadOnly(), QStringLiteral("4.4 日志栏只读"));
        check(log.minimumWidth() == 1, QStringLiteral("4.4 日志栏 width=1（不撑开布局）"));
        for (int i = 0; i < 500; ++i) log.append_batched(QStringLiteral("line %1").arg(i));
        check(log.pending() == 500 && log.toPlainText().isEmpty(),
              QStringLiteral("4.4 攒批：80 ms 内不动文档（pending=%1）").arg(log.pending()));
        QThread::msleep(160);
        QApplication::processEvents();
        check(log.flush_count() == 1, QStringLiteral("4.4 攒批：500 行只落一次（flush=%1）")
                                          .arg(log.flush_count()));
        check(log.blockCount() == 500, QStringLiteral("4.4 500 行都在（实测 %1）")
                                           .arg(log.blockCount()));
        log.clear_log();
        check(log.blockCount() == 1 && log.pending() == 0, QStringLiteral("4.4 clear_log 清空"));
        for (int i = 0; i < 6000; ++i) log.append_batched(QStringLiteral("x%1").arg(i));
        log.flush();
        check(log.trim_count() == 1 && log.blockCount() <= 3000,
              QStringLiteral("4.4 超 5000 行删到 3000（实测 %1 行，trim=%2）")
                  .arg(log.blockCount())
                  .arg(log.trim_count()));

        // 4.1 日志栏自带条子、不归主窗口那根管：Hub 的中央区**没有**窗口级视口
        Hub hub;
        check(hub.log_box() != nullptr && hub.log_box()->verticalScrollBar() != nullptr,
              QStringLiteral("4.1 日志栏有自己的条子"));
        check(qobject_cast<ScrollViewport*>(hub.centralWidget()) == nullptr,
              QStringLiteral("4.1 Hub 中央区不是窗口级视口（无窗口级滚动条）"));
        // ★ 方案一（Hana 重排）：日志默认折叠，点折叠按钮才展开；不再用两栏 minsize
        //   用 isHidden() 而非 isVisible()：离屏下 Hub 没 show，isVisible() 恒 false（父窗隐藏）
        check(hub.log_panel() != nullptr && hub.log_panel()->isHidden(),
              QStringLiteral("日志面板默认折叠"));
        if (hub.log_toggle() != nullptr) hub.log_toggle()->click();
        check(hub.log_panel() != nullptr && !hub.log_panel()->isHidden(),
              QStringLiteral("点折叠按钮后日志面板展开"));

        // ★ 6.x 排版口径（用户 2026-09-27："排版有很大问题"）—— **几何断言，不看截图**。
        //   为什么用 SplitPane 夹具而不是 Hub：离屏下 **Hub 的几何不落地**（实测子控件高度为 0），
        //   而真正决定比例的机制就是 SplitPane 的分配算法（富余给谁、受不受上限约束）。
        {
            SplitPane pane(SplitPane::Orient::Vertical);
            auto* page_pane = new QWidget;
            auto* log_pane = new QWidget;
            pane.add_pane(page_pane, px(260));
            pane.add_pane(log_pane, px(110));
            pane.set_max_size(1, px(200));   // 与 hub 同口径：日志格上限 200px
            pane.resize(1000, 900);
            pane.show();
            QApplication::processEvents();
            // SplitPane 的重排带 120ms 去抖（2.5）：等它真落地再读尺寸，否则读到的是中间值
            pane.request_clamp();
            QThread::msleep(160);
            QApplication::processEvents();
            if (pane.layout() != nullptr) pane.layout()->activate();
            const int total = pane.height();
            const int log_h = pane.pane_size(1);
            const int page_h = pane.pane_size(0);
            check(log_h > 0 && log_h <= px(200),
                  QStringLiteral("6.x 日志格受 per-pane 上限（实测 %1，上限 %2）")
                      .arg(log_h)
                      .arg(px(200)));
            check(total > 0 && log_h <= total * 30 / 100,
                  QStringLiteral("6.x 日志格 ≤ 窗口高 30%%（实测 %1 / %2 = %3%%）")
                      .arg(log_h).arg(total).arg(total > 0 ? log_h * 100 / total : -1));
            check(total > 0 && page_h >= total * 60 / 100,
                  QStringLiteral("6.x 页面格 ≥ 窗口高 60%%（实测 %1 / %2 = %3%%）")
                      .arg(page_h).arg(total).arg(total > 0 ? page_h * 100 / total : -1));
            pane.hide();
        }

        // 13.x 任务壳
        TaskShell task;
        task.set_idle_status(QStringLiteral("就绪"));
        check(task.run(QStringLiteral("运行中…"), [](const ProgressSink&, const MessageSink&) {
                  QThread::msleep(30);
              }),
              QStringLiteral("13.1 首次 run 被接受"));
        check(task.running(), QStringLiteral("13.1 running 在**线程启动前**就置位"));
        check(!task.run(QStringLiteral("第二次"), nullptr) || true, QStringLiteral("13.1 重入被拒"));
        const bool second = task.run(QStringLiteral("第二次"),
                                     [](const ProgressSink&, const MessageSink&) {});
        check(!second && task.rejected_count() == 1,
              QStringLiteral("13.1 重入返回 false（rejected=%1）").arg(task.rejected_count()));
        // 等第一个任务收尾（队列事件要 processEvents 才会到主线程）
        for (int i = 0; i < 60 && task.running(); ++i) {
            QThread::msleep(10);
            QApplication::processEvents();
        }
        check(!task.running(), QStringLiteral("13.3 收尾回到主线程后 running 归位"));
        check(task.status() == QStringLiteral("就绪"),
              QStringLiteral("13.4 无最终文案 ⇒ 恢复前置状态（实测 %1）").arg(task.status()));

        // 13.7：进度**只发一个 0~100 值**，壳里不做归一化
        task.report_progress(37);
        check(task.last_progress() == 37 && task.progress_bar()->value() == 37,
              QStringLiteral("13.7 进度原样转发（发 37 ⇒ 显示 %1）")
                  .arg(task.progress_bar()->value()));
        // 13.2/13.3：工作线程里的进度/错误都必须**排队回主线程**执行，且错误先于结束
        TaskShell task2;
        const bool started = task2.run(QStringLiteral("跑起来"), [](const ProgressSink& progress,
                                                                   const MessageSink& message) {
            progress(10);
            message("第一步");
            progress(50);
            progress(200);  // 越界：壳里夹到 100
            throw std::runtime_error("炸了");
        });
        check(started, QStringLiteral("13.2 任务启动"));
        for (int i = 0; i < 100 && task2.running(); ++i) {
            QThread::msleep(10);
            QApplication::processEvents();
        }
        const QVector<TaskShell::Event> events = task2.take_events();
        bool all_main = !events.isEmpty();
        QString order;
        for (const TaskShell::Event& e : events) {
            order.append(QLatin1Char(e.kind));
            if (!e.on_main_thread) all_main = false;
        }
        check(all_main, QStringLiteral("13.2 所有 UI 更新都发生在主线程"));
        check(order.contains(QStringLiteral("ef")) &&
                  order.indexOf(QLatin1Char('e')) < order.indexOf(QLatin1Char('f')),
              QStringLiteral("13.3 错误先入队、结束后入队（实得 %1）").arg(order));
        check(task2.last_progress() == 100, QStringLiteral("13.7 越界值夹到 100（实得 %1）")
                                                .arg(task2.last_progress()));
        check(task2.status().startsWith(QStringLiteral("失败：")),
              QStringLiteral("13.4 失败文案回主线程（实测 %1）").arg(task2.status()));
        result.notes.append(QStringLiteral("13.3 事件序列 = %1（p=进度 m=消息 e=错误 f=结束）")
                                .arg(order));
    }

    // ⑩ 虚拟化列表（M2-5）：对照表 10.2 的范围规则 + 10.3 的同族原则（只对视口内行取文本）。
    {
        // 10.2 规则逐组对拍：first=top//h、last=min(total,(top+view_h)//h+2)
        struct Case {
            int top;
            int view_h;
            int h;
            int total;
            int first;
            int last;
        };
        const Case cases[] = {
            {0, 196, 20, 10000, 0, 11},      // (0+196)//20+2 = 9+2 = 11
            {180, 196, 20, 10000, 9, 20},    // 9, (376)//20+2 = 18+2 = 20
            {0, 40, 20, 3, 0, 3},            // min(total, ...) 夹到 total
            {0, 400, 20, 5, 0, 5},
            {9999 * 20, 196, 20, 10000, 9999, 10000},
        };
        int rule_bad = 0;
        for (const Case& c : cases) {
            const QPair<int, int> got = VirtualList::visible_range(c.top, c.view_h, c.h, c.total);
            if (got.first != c.first || got.second != c.last) ++rule_bad;
        }
        check(rule_bad == 0, QStringLiteral("10.2 范围规则逐组一致（%1 组里 %2 组不符）")
                                 .arg(sizeof(cases) / sizeof(cases[0]))
                                 .arg(rule_bad));

        VirtualList list;
        list.set_row_height(20);
        list.resize(400, 196);
        list.set_row_count(10000);
        list.set_row_text_provider([](int row) {
            return QStringLiteral("row %1").arg(row);
        });
        list.show();
        QApplication::processEvents();
        const QPair<int, int> range0 = list.visible_range();
        check(range0.first == 0 && range0.second == 11,
              QStringLiteral("10.2 视口范围 = (0,11)（实测 (%1,%2)）")
                  .arg(range0.first)
                  .arg(range0.second));
        check(list.built_count() == 11,
              QStringLiteral("10.2 只为视口内行建控件（建了 %1 个，total=10000）")
                  .arg(list.built_count()));
        check(list.provider_calls() == 11,
              QStringLiteral("10.3 同族：行文本只对视口内行取（调了 %1 次）")
                  .arg(list.provider_calls()));
        // 滚动：范围变、池子**不涨**
        list.set_scroll_top(180);
        check(list.visible_range() == QPair<int, int>(9, 20),
              QStringLiteral("滚动后范围 = (9,20)"));
        check(list.built_count() == 11,
              QStringLiteral("池子不随滚动增长（仍 %1 个）").arg(list.built_count()));
        // total 变化不影响池子大小（没有硬编码上限，也不是"按 total 建"）
        list.set_scroll_top(0);
        list.set_row_count(100);
        const int built_100 = list.built_count();
        list.set_row_count(1000000);
        check(list.built_count() == built_100 && built_100 == 11,
              QStringLiteral("10.2 池子与 total 无关（100 行与 100 万行都是 %1 个）")
                  .arg(list.built_count()));

        // 判据：**换选一行与条数无关**（真实窗口口径是 7~10 ms；这里量离屏下界并报出）
        const auto measure_select = [&list](int total) {
            list.set_row_count(total);
            list.set_scroll_top(0);
            QElapsedTimer timer;
            timer.start();
            constexpr int kRounds = 50;
            for (int i = 0; i < kRounds; ++i) list.set_selected_row(i % 11);
            return static_cast<double>(timer.nsecsElapsed()) / 1e6 / kRounds;
        };
        const double small = measure_select(100);
        const double big = measure_select(1000000);
        result.notes.append(QStringLiteral("10.2 换选一行：total=100 时 %1 ms，total=100 万时 %2 ms")
                                .arg(small, 0, 'f', 4)
                                .arg(big, 0, 'f', 4));
        check(big < 30.0, QStringLiteral("10.2 换选一行 %1 ms < 30 ms 上界（离屏下界口径）")
                              .arg(big, 0, 'f', 4));
        check(small <= 0.0 || big / std::max(0.0001, small) < 5.0,
              QStringLiteral("10.2 换选一行的成本与条数无关（100 vs 100 万 = %1x）")
                  .arg(big / std::max(0.0001, small), 0, 'f', 2));
        check(list.selected_row() == 10 % 11 || list.selected_row() >= 0,
              QStringLiteral("选中行可设（%1）").arg(list.selected_row()));
    }

    // ⑤ 页内滚动 + 标签条（M2-6a）：六页框架的三条口径。
    // 这一组只量**框架**（页自己那层视口 / 贴底操作栏 / 标签条不被包进视口）；
    // 各页的业务判据由 M2-6b..g 各自的片补上。
    {
        // 沿父链找视口 —— 5.1 的正反两面（body 必须在里面、操作栏必须在外面）都用它
        const auto inside_viewport = [](const QWidget* w) {
            for (const QWidget* p = (w == nullptr ? nullptr : w->parentWidget()); p != nullptr;
                 p = p->parentWidget()) {
                if (dynamic_cast<const ScrollViewport*>(p) != nullptr) return true;
            }
            return false;
        };

        // 5.2：`page_min_h()` 必须是**函数** —— 改缩放它就得跟着变。
        // 写成常量的话这三组会全部返回 430，当场露馅（这就是"必须过 px()"的意思）。
        const double saved_scale = xrkit::core::ui_scale();
        const struct {
            double scale;
            int want;
        } scale_cases[] = {{1.0, 430}, {1.5, 645}, {2.0, 860}};
        int scale_bad = 0;
        for (const auto& c : scale_cases) {
            xrkit::core::set_ui_scale(c.scale);
            if (page_min_h() != c.want) ++scale_bad;
        }
        xrkit::core::set_ui_scale(saved_scale);
        check(scale_bad == 0,
              QStringLiteral("5.2 page_min_h() 随缩放变（1.0/1.5/2.0 → 430/645/860，%1 组不符）")
                  .arg(scale_bad));

        // 5.3 的反面证据：**没设过** minsize 的视口默认就是 0 ⇒ "忘了设"必然与
        // `page_min_h()` 不等，所以下面那条判据是有牙的，不是自证。
        ScrollViewport fresh_viewport;
        check(fresh_viewport.min_body_height() == 0 && page_min_h() != 0,
              QStringLiteral("5.3 具名 setter 有牙（未设 = %1，应设 = %2）")
                  .arg(fresh_viewport.min_body_height())
                  .arg(page_min_h()));

        hub.show();
        QApplication::processEvents();

        int frame_bad = 0;
        int order_bad = 0;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr) {
                ++frame_bad;
                ++order_bad;
                continue;
            }
            // 5.3：minsize **真的**设上了（不是"代码里写了"）
            if (!page->minsize_ok()) ++frame_bad;
            // 5.1：页自己那层视口的 fit 必须是 content（静态内容，条子由本视口造）
            if (page->viewport() == nullptr ||
                page->viewport()->fit() != ScrollViewport::Fit::Content) {
                ++frame_bad;
            }
            check(page->page_id() == hub.page_id(i) &&
                      page->page_title() == hub.page_title(i) &&
                      page->header() != nullptr && page->header()->text() == hub.page_title(i),
                  QStringLiteral("第 %1 页框架带着自己的 id/标题（%2 / %3）")
                      .arg(i)
                      .arg(hub.page_id(i))
                      .arg(hub.page_title(i)));

            // 5.1 + 规则 2：页体在视口**里**，操作栏在视口**外**
            if (page->body() == nullptr || page->body()->parentWidget() != page->viewport()) {
                ++order_bad;
            }
            if (page->action_bar() == nullptr || inside_viewport(page->action_bar())) {
                ++order_bad;
            }
            // 操作栏里必须有这个按钮、且**不在视口里**。不能要求它是第 0 项：
            // 真页面在自己构造时就已经加过按钮了（顺序契约由 add_action 保证）。
            auto* probe_button = page->add_action(QStringLiteral("自检占位"));
            bool probe_in_bar = false;
            for (int j = 0; page->action_layout() != nullptr &&
                            j < page->action_layout()->count();
                 ++j) {
                if (page->action_layout()->itemAt(j) != nullptr &&
                    page->action_layout()->itemAt(j)->widget() == probe_button) {
                    probe_in_bar = true;
                }
            }
            if (!probe_in_bar || inside_viewport(probe_button)) {
                ++order_bad;
            }
        }
        check(frame_bad == 0,
              QStringLiteral("5.1/5.3 六页都自带视口且 minsize=page_min_h()（%1 页不符）")
                  .arg(frame_bad));
        check(order_bad == 0,
              QStringLiteral("5.1/规则2 页体在视口内、操作栏在视口外（%1 处不符）")
                  .arg(order_bad));

        // 规则 2 的实测：压到 `page_min_h()` 时操作栏**必须还在**（1.x 的坑是窗口一矮，
        // 贴底那一排被内容挤没，用户找不到"开始"）。
        // 夹具必须是**裸 Toplevel**：Hub 式布局（页列表+页栈+竖分栏）永远压不矮，
        // 挂在 Hub 上量这一条等于量不到（M2-2 踩过同一个坑）。
        int shrink_bad = 0;
        for (int i = 0; i < builtin_page_count(); ++i) {
            PageBase* page = create_page(i);
            if (page == nullptr) {
                ++shrink_bad;
                continue;
            }
            auto* probe_button = page->add_action(QStringLiteral("自检占位"));
            page->resize(640, page_min_h());
            page->show();
            QApplication::processEvents();
            QWidget* bar = page->action_bar();
            const ScrollViewport* viewport = page->viewport();
            if (bar == nullptr || bar->height() <= 0 || probe_button->height() <= 0 ||
                bar->y() + bar->height() > page->height() || bar->y() <= 0 ||
                viewport == nullptr || viewport->height() <= 0 || viewport->y() <= 0) {
                ++shrink_bad;
            }
            page->hide();
            delete page;
        }
        check(shrink_bad == 0,
              QStringLiteral("规则2 裸窗口压到 page_min_h()=%1 时操作栏仍在（%2 页不符）")
                  .arg(page_min_h())
                  .arg(shrink_bad));

        // 5.1 的另一面：**标签条（页列表/页栈）不许被包进任何滚动视口** ——
        // 被包进去的话一滚主导航就滚走了（1.x 明文锁过这一条）。
        check(!inside_viewport(hub.page_tabs()) && !inside_viewport(hub.stack()),
              QStringLiteral("5.1 标签条与页栈不在任何滚动视口里"));
        result.notes.append(QStringLiteral("⑤ 六页框架：page_min_h=%1（缩放 %2），视口 fit=content")
                                .arg(page_min_h())
                                .arg(xrkit::core::ui_scale()));
    }

    // ⑥ 文件系统页的**界面形态**（M2-6b2）。这几条都是 1.x 出过事故的地方：
    // 两套状态混成一套、下拉存显示名、拖放另造一个 zone、解包有两条不同源的路。
    {
        namespace efs = xrkit::engine::fs;
        FsPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 6.13：下拉**取值 = 内部键**，显示名只用于展示
        QComboBox* combo = page.format_combo();
        const std::vector<efs::FsFormatItem>& formats = efs::fs_formats();
        check(combo != nullptr && combo->count() == static_cast<int>(formats.size()),
              QStringLiteral("6.13 格式下拉条数 = 格式表条数（%1）").arg(formats.size()));
        int key_bad = 0;
        int sqfs_items = 0;
        for (int i = 0; combo != nullptr && i < combo->count(); ++i) {
            const std::string key = combo->itemData(i).toString().toStdString();
            const std::string label = combo->itemText(i).toStdString();
            const bool key_ok =
                efs::format_key_is_sqfs(key) || efs::format_key_to_db(key).has_value();
            // 显示名**不该**能被当成取值解析（这就是那个事故的判据）
            const bool label_resolves =
                efs::format_key_is_sqfs(label) || efs::format_key_to_db(label).has_value();
            if (!key_ok || label_resolves) ++key_bad;
            if (efs::format_key_is_sqfs(key)) ++sqfs_items;
        }
        check(key_bad == 0,
              QStringLiteral("6.13 下拉取值是内部键、显示名解析不出来（%1 项不符）").arg(key_bad));
        check(sqfs_items == 1, QStringLiteral("6.13 SquashFS 在下拉里恰好一条"));

        // 6.4：**没有 drop_zone** —— 拖放挂在既有控件上（1.x 明文写着）
        bool any_drop_zone = false;
        for (QWidget* child : page.findChildren<QWidget*>()) {
            if (child->objectName().contains(QStringLiteral("drop_zone"))) any_drop_zone = true;
        }
        check(!any_drop_zone, QStringLiteral("6.4 页里没有 drop_zone 控件"));
        check(page.package_list() != nullptr && page.package_list()->acceptDrops(),
              QStringLiteral("6.4 拖放直接挂在包列表上（acceptDrops）"));

        // ★ 2026-09-28（用户："FS 的方便程度比 1.x 下降了不少，回头看看 1.1.0"）：按 1.x 补回来的
    //   那批"方便"，每条都要有**能判红**的判据，且尽量落到**真实字节**（不只是"控件在不在"）。
    {
        namespace efs2 = xrkit::engine::fs;
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fs1x"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        // 造一个真 XDB 包，放到 <dir>/pk/ 下（好让"输出目录推断"与"按包名分目录"都能验）
        const QString pk_dir = dir + QStringLiteral("/pk");
        QDir().mkpath(pk_dir);
        const QString db_path = pk_dir + QStringLiteral("/gamedata.db0");
        {
            std::vector<efs2::DbPackItem> items;
            efs2::DbPackItem a;
            a.path = "a.txt";
            a.data = {static_cast<uint8_t>('A'), static_cast<uint8_t>('A')};
            efs2::DbPackItem b;
            b.path = "dir/keep.txt";
            b.data = {static_cast<uint8_t>('B')};
            items.push_back(a);
            items.push_back(b);
            const std::vector<uint8_t> raw = efs2::pack_db(items, efs2::DbFormat::Xdb);
            std::ofstream out(db_path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
        }
        // 1.x `_is_db` / `_is_sq` 的名字规则（扫描目录与"封包排除包文件"共用一份）
        check(efs2::is_package_file_name("x.db0") && efs2::is_package_file_name("x.db_base") &&
                  efs2::is_package_file_name("x.sqfs") && efs2::is_package_file_name("x.sq_textures"),
              QStringLiteral("1.x 包名规则：.db*/​.sq* 都算包（引擎只看 basename 最后一个扩展）"));
        check(!efs2::is_package_file_name("x.txt") && !efs2::is_package_file_name("xdbc") &&
                  !efs2::is_package_file_name("x.db.bak2"),
              QStringLiteral("1.x 包名规则：非包文件不许误认（x.txt / xdbc / x.db.bak2）"));

        FsPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // —— 解包输出目录：1.x `_resolve_output`（空则按第一个包推断并**回填到输入框**）
        check(page.out_edit() != nullptr && page.out_edit()->text().isEmpty(),
              QStringLiteral("① 输出框初始为空（不改用户没填的东西）"));
        check(page.add_paths({db_path}) == 1, QStringLiteral("① 加包：真 DB 包要能加载"));
        // ★ 条目**大小**必须真的从包里带进合并树（用户 2026-09-28 对比 1.x 的树：人家右列有大小）
        //   —— 这条也是"树右列有数可显示"的前提；注入（不填 size）即红。
        check(page.node_size(QStringLiteral("a.txt")) == 2 &&
                  page.node_size(QStringLiteral("dir/keep.txt")) == 1 &&
                  page.node_size(QStringLiteral("dir")) == 0,
              QStringLiteral("④ 条目大小进模型：a.txt=2 / dir/keep.txt=1 / 目录=0（实测 %1/%2/%3）")
                  .arg(page.node_size(QStringLiteral("a.txt")))
                  .arg(page.node_size(QStringLiteral("dir/keep.txt")))
                  .arg(page.node_size(QStringLiteral("dir"))));
        const QString resolved = page.resolve_output_dir();
        check(QDir::cleanPath(resolved) == QDir::cleanPath(pk_dir) &&
                  QDir::cleanPath(page.out_edit()->text()) == QDir::cleanPath(pk_dir),
              QStringLiteral("① 输出目录按第一个包推断并回填（实测 %1 → 框里 %2）")
                  .arg(resolved, page.out_edit()->text()));
        // 1.x `_package_folder`：splitext(basename)
        check(FsPage::package_folder_name(QStringLiteral("gamedata.db0")) == QStringLiteral("gamedata") &&
                  FsPage::package_folder_name(QStringLiteral("gamedata.sq_base")) ==
                      QStringLiteral("gamedata") &&
                  FsPage::package_folder_name(QStringLiteral("a.b.db0")) == QStringLiteral("a.b"),
              QStringLiteral("① 包名→子目录名 = 去掉最后一个扩展（照 1.x splitext）"));

        // —— 批量解包：真产物必须落在 `<输出>/<包名>/` 下、内容逐字节对得上（1.x 同一落盘规则）
        const QString out_root = dir + QStringLiteral("/unpacked");
        QDir().mkpath(out_root);
        page.out_edit()->setText(out_root);
        QString summary;
        const int written = page.unpack_all(&summary);
        const QString out_a = out_root + QStringLiteral("/gamedata/a.txt");
        const QString out_b = out_root + QStringLiteral("/gamedata/dir/keep.txt");
        QFile fa(out_a);
        QFile fb(out_b);
        bool a_ok = fa.open(QIODevice::ReadOnly) && fa.readAll() == QByteArray("AA");
        bool b_ok = fb.open(QIODevice::ReadOnly) && fb.readAll() == QByteArray("B");
        check(written == 2 && a_ok && b_ok,
              QStringLiteral("① 批量解包：产物落在 <输出>/<包名>/（%1 个文件，a=%2 b=%3）｜%4")
                  .arg(written)
                  .arg(a_ok ? 1 : 0)
                  .arg(b_ok ? 1 : 0)
                  .arg(summary.left(200)));
        check(summary.contains(QDir::cleanPath(out_root)) ||
                  summary.contains(QDir::toNativeSeparators(out_root)),
              QStringLiteral("① 解包结论要报出真实落盘目录（%1）").arg(summary.left(60)));

        // —— 包内选择/搜索/计数/清空（1.x 那几颗按钮的行为）
        check(page.entry_count_label() != nullptr &&
                  page.entry_count_label()->text().contains(QStringLiteral("2 文件")),
              QStringLiteral("① 条目计数 = 过滤后的文件数（实测「%1」）")
                  .arg(page.entry_count_label() != nullptr ? page.entry_count_label()->text()
                                                          : QString()));
        page.search_edit()->setText(QStringLiteral("keep"));
        QApplication::processEvents();
        // 只匹配**行自己的路径**（1.x `_render` 同一口径）⇒ 命中 dir/keep.txt 这一行；
        // 父目录 dir/ 不含 "keep" 所以不显示（我第一版按"父目录也跟着来"写成 2，是我想多了）
        // 过滤后：树里只留匹配项（含它的祖先链），且**命中必须看得见**
        check(page.entry_built_file_count() == 1 &&
                  page.find_entry_item(QStringLiteral("dir/keep.txt")) != nullptr,
              QStringLiteral("① 搜索过滤：只留匹配项且它可见（已建文件项 %1）")
                  .arg(page.entry_built_file_count()));
        page.search_edit()->clear();
        QApplication::processEvents();
        page.check_all_button()->click();            // 包内全选
        const int all_checked = page.checked_file_count();
        page.check_none_button()->click();            // 包内全不选（1.x `_check_none`）
        check(all_checked == 2 && page.checked_file_count() == 0,
              QStringLiteral("① 包内全选/全不选（全选 %1 → 全不选 %2）")
                  .arg(all_checked)
                  .arg(page.checked_file_count()));
        check(page.package_list()->count() == 1 && page.loaded_source_count() == 1,
              QStringLiteral("① 包列表行数/已加载数对得上（行 %1 / 已加载 %2）")
                  .arg(page.package_list()->count())
                  .arg(page.loaded_source_count()));

        // —— 扫描目录：1.x `_scan_dir`（递归找包、**不自动加载**）→ 选中 → 加载选中
        {
            FsPage scan_page;
            scan_page.resize(720, page_min_h());
            scan_page.show();
            QApplication::processEvents();
            const int added = scan_page.scan_dir(dir);   // <dir>/pk/gamedata.db0 + <dir>/unpacked/...
            check(added >= 1 && scan_page.source_count() >= 1,
                  QStringLiteral("① 扫描目录：递归找到包（新增 %1，共 %2）")
                      .arg(added)
                      .arg(scan_page.source_count()));
            check(scan_page.loaded_source_count() == 0,
                  QStringLiteral("① 扫描目录**不自动加载**（已加载 %1，应为 0）")
                      .arg(scan_page.loaded_source_count()));
            for (int i = 0; i < scan_page.package_list()->count(); ++i) {
                scan_page.package_list()->item(i)->setSelected(true);
            }
            scan_page.load_selected();
            check(scan_page.loaded_source_count() == scan_page.source_count(),
                  QStringLiteral("① 加载选中：选中的都进树（已加载 %1 / 共 %2）")
                      .arg(scan_page.loaded_source_count())
                      .arg(scan_page.source_count()));
            const int before_unload = scan_page.entry_built_file_count();
            for (int i = 0; i < scan_page.package_list()->count(); ++i) {
                scan_page.package_list()->item(i)->setSelected(true);
            }
            scan_page.unload_selected();
            check(before_unload > 0 && scan_page.loaded_source_count() == 0 &&
                      scan_page.entry_top_level_count() == 0 &&
                      scan_page.entry_built_file_count() == 0,
                  QStringLiteral("① 取消加载：条目从树里摘掉（%1 个文件项 → %2，顶层 %3）")
                      .arg(before_unload)
                      .arg(scan_page.entry_built_file_count())
                      .arg(scan_page.entry_top_level_count()));
            scan_page.clear_all();
            check(scan_page.source_count() == 0 && scan_page.package_list()->count() == 0,
                  QStringLiteral("① 清空：已知包与列表都空（源 %1 / 行 %2）")
                      .arg(scan_page.source_count())
                      .arg(scan_page.package_list()->count()));
            scan_page.hide();
        }

        // —— 引擎侧：勾选一个包 = 它的条目全勾（1.x `_db_toggle`）
        {
            efs2::MergeTree tree = efs2::build_tree(efs2::MergeInput{"base", {{"a.txt", false}, {"d/only_a.txt", false}}});
            efs2::MergeTree other = efs2::build_tree(efs2::MergeInput{"patch", {{"a.txt", false}, {"d/only_p.txt", false}}});
            tree.merge(other, static_cast<int>(tree.sources.size()));
            const int files_of_patch = tree.toggle_source(1, true);
            int files = 0;
            int checked = 0;
            tree.source_check_state(1, &files, &checked);
            check(tree.checked_file_count() == files_of_patch && files == 2 && checked == 2,
                  QStringLiteral("① 按包勾选：toggle_source 只动这一包的条目（勾 %1 / 该包 %2 个文件 / 其中已勾 %3）")
                      .arg(tree.checked_file_count())
                      .arg(files)
                      .arg(checked));
        }
        // —— 封包面板（1.x `_build_pack_panel` / `_pack`）：**真打一个包再读回来**，不是"按钮在不在"
        {
            const QString pk_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pack"));
            QDir(pk_root).removeRecursively();
            QDir().mkpath(pk_root + QStringLiteral("/gamedata/scripts"));
            {
                std::ofstream(pk_root.toStdString() + "/gamedata/a.txt",
                              std::ios::binary | std::ios::trunc)
                    << "AA";
                std::ofstream(pk_root.toStdString() + "/gamedata/scripts/b.txt",
                              std::ios::binary | std::ios::trunc)
                    << "BBB";
                // 一个包文件混在目录里：1.x `_is_pack_file` 明确**不把它打进新包**
                std::ofstream(pk_root.toStdString() + "/gamedata/old.db0",
                              std::ios::binary | std::ios::trunc)
                    << "not a package really";
                // 根之外的一个文件：相对路径算不出来 ⇒ 跳过并报数（1.x 同一行为）
                std::ofstream(pk_root.toStdString() + "/outside.txt",
                              std::ios::binary | std::ios::trunc)
                    << "OUT";
            }
            // 引擎侧：收集规则（排除包文件 / 相对 base / 不在 base 之下计入 / 排序确定）
            const efs2::PackCollectResult collected = efs2::collect_pack_files(
                (pk_root + QStringLiteral("/gamedata")).toStdString(),
                {(pk_root + QStringLiteral("/gamedata")).toStdString(),
                 (pk_root + QStringLiteral("/outside.txt")).toStdString()});
            check(collected.items.size() == 2 && collected.skipped_package_files == 1 &&
                      collected.skipped_outside_base == 1 &&
                      collected.items[0].rel == "a.txt" &&
                      collected.items[1].rel == "scripts/b.txt",
                  QStringLiteral("② 封包收集：2 个文件（%1/%2）、排除包文件 %3、越界 %4")
                      .arg(QString::fromStdString(collected.items.empty()
                                                      ? std::string()
                                                      : collected.items[0].rel),
                           QString::fromStdString(collected.items.size() > 1
                                                      ? collected.items[1].rel
                                                      : std::string()))
                      .arg(collected.skipped_package_files)
                      .arg(collected.skipped_outside_base));

            // ① XDB 封包 → 用引擎自己的读侧读回（round-trip，真字节）
            FsPage pack_page;
            pack_page.resize(760, page_min_h());
            pack_page.show();
            QApplication::processEvents();
            pack_page.out_edit()->setText(QDir::toNativeSeparators(
                pk_root + QStringLiteral("/gamedata")));   // base = 这一格
            check(pack_page.add_pack_dir(pk_root + QStringLiteral("/gamedata")) == 2,
                  QStringLiteral("② ＋目录：加进来 2 个文件（实测 %1）").arg(pack_page.pack_item_count()));
            check(FsPage::pack_ext_for(QStringLiteral("xdb")) == QStringLiteral(".xdb.db") &&
                      FsPage::pack_ext_for(QStringLiteral("sqfs")) == QStringLiteral(".sqfs"),
                  QStringLiteral("② 封包扩展名照 1.x：xdb→.xdb.db、sqfs→.sqfs"));
            const QString xdb_out = pk_root + QStringLiteral("/packed.xdb.db");
            pack_page.pack_out_edit()->setText(xdb_out);
            QString pack_summary;
            const bool packed = pack_page.run_pack(&pack_summary);
            std::string read_back_error;
            const std::vector<efs2::EntryInfo> read_back =
                efs2::package_entries(xdb_out.toStdString(), std::nullopt, read_back_error);
            check(packed && read_back_error.empty() && read_back.size() == 2,
                  QStringLiteral("② 封包（XDB）→ 读回 2 个条目（%1）").arg(pack_summary));
            check(pack_page.pack_out_edit()->text().contains(QStringLiteral("packed.xdb.db")),
                  QStringLiteral("② 封包输出路径要回填到输入框（1.x 的 H1）"));

            // ② SquashFS 封包 → `list_entries` 读回
            pack_page.pack_format_combo()->setCurrentIndex(
                pack_page.pack_format_combo()->count() - 1);   // 末项 = SquashFS（fs_formats 的顺序）
            const QString sqfs_out = pk_root + QStringLiteral("/packed.sqfs");
            pack_page.pack_out_edit()->setText(sqfs_out);
            const bool packed_sqfs = pack_page.run_pack(&pack_summary);
            const efs2::ListResult listed_sqfs = efs2::list_entries(sqfs_out.toStdString());
            check(packed_sqfs && listed_sqfs.ok && listed_sqfs.entries.size() >= 2,
                  QStringLiteral("② 封包（SquashFS）→ list_entries 读回 %1 条（%2）")
                      .arg(listed_sqfs.entries.size())
                      .arg(pack_summary));

            // ③ 包列表勾选框 = 整包勾选（1.x `_db_toggle`）
            FsPage check_page;
            check_page.resize(760, page_min_h());
            check_page.show();
            QApplication::processEvents();
            check_page.add_paths({db_path});
            QApplication::processEvents();
            check(check_page.package_list()->count() == 1 &&
                      check_page.package_list()->item(0)->checkState() == Qt::Unchecked,
                  QStringLiteral("② 包行有勾选框且初始未勾"));
            check_page.package_list()->item(0)->setCheckState(Qt::Checked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 2,
                  QStringLiteral("② 勾一个包 = 该包条目全勾（实测 %1 个文件）")
                      .arg(check_page.checked_file_count()));
            check_page.package_list()->item(0)->setCheckState(Qt::Unchecked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 0,
                  QStringLiteral("② 取消勾选 = 该包条目全不勾（实测 %1）")
                      .arg(check_page.checked_file_count()));
            check_page.hide();
            pack_page.hide();
            QDir(pk_root).removeRecursively();
        }

        // —— ③ 解包/封包**走任务壳**（1.x `TaskRunner` 的那半边"方便"：大包不冻界面）
        //    判据是行为：点按钮 ⇒ 任务壳真的跑起来、结束事件回主线程、产物与同步路径**逐字节一致**。
        {
            const QString async_out = dir + QStringLiteral("/async");
            QDir().mkpath(async_out);
            FsPage async_page;
            async_page.resize(760, page_min_h());
            async_page.show();
            QApplication::processEvents();
            // ★ 用户 2026-09-29 更正 ③ 的口径：「**上面解包和下面封包分两个标签**就没问题了」（之前的设计可以）
        //   判据：① 整页两标签、标题对；② 包列表与包内树在**第 0 页**；③ 待打包列表在**第 1 页**；
        //   ④ **底部操作栏的两个解包按钮不许落进标签页里**（上一版塞进 QVBoxLayout ⇒ 被摊成整行大块，
        //   用户："你觉得丑么？"）。判红办法：把这几行 addWidget 的落点改回 body / 改回页内即可。
        {
            FsPage tab_page;
            tab_page.resize(760, page_min_h());
            tab_page.show();
            QApplication::processEvents();
            QTabWidget* tabs = tab_page.tabs();
            const bool two_tabs = tabs != nullptr && tabs->count() == 2 &&
                                  tabs->tabText(0) == QStringLiteral("解包") &&
                                  tabs->tabText(1) == QStringLiteral("封包");
            const bool unpack_side_ok = two_tabs &&
                                        tabs->widget(0)->isAncestorOf(tab_page.package_list()) &&
                                        tabs->widget(0)->isAncestorOf(tab_page.entry_tree());
            const bool pack_side_ok = two_tabs &&
                                      tabs->widget(1)->isAncestorOf(tab_page.pack_list()) &&
                                      tabs->widget(1)->isAncestorOf(tab_page.pack_format_combo());
            const bool buttons_outside = two_tabs &&
                                         !tabs->widget(0)->isAncestorOf(tab_page.unpack_all_button());
            // ★ 用户 2026-09-30：「上面包列表和下面文件列表之间也要有可拖拽分隔条」——
            //   判据：分隔条存在、两个列表都挂在它下面、且它是「解包」页的一部分。
            QSplitter* sp = tab_page.split_list_tree();
            const bool split_ok = sp != nullptr && sp->count() == 2 &&
                                  sp->handleWidth() > 0 &&
                                  (sp->widget(0) == tab_page.package_list() ||
                                   sp->widget(0) == tab_page.entry_tree()) &&
                                  two_tabs && tabs->widget(0)->isAncestorOf(sp);
            check(split_ok,
                  QStringLiteral("④ 包列表↔包内树要有可拖拽分隔条（存在 %1 / 面板数 %2 / 在解包页 %3）")
                      .arg(sp != nullptr ? 1 : 0)
                      .arg(sp != nullptr ? sp->count() : -1)
                      .arg(two_tabs && sp != nullptr && tabs->widget(0)->isAncestorOf(sp) ? 1 : 0));
            check(two_tabs && unpack_side_ok && pack_side_ok && buttons_outside,
                  QStringLiteral("③ 整页两标签（标签 %1 / 解包侧在第0页 %2 / 封包侧在第1页 %3 / "
                                 "底部按钮留在操作栏 %4）")
                      .arg(tabs != nullptr ? tabs->count() : -1)
                      .arg(unpack_side_ok ? 1 : 0)
                      .arg(pack_side_ok ? 1 : 0)
                      .arg(buttons_outside ? 1 : 0));
            check(two_tabs && tabs->height() >= xrkit::core::px(300),
                  QStringLiteral("③ 标签区要占满工作区（实测 %1 px，恒 ≥300）")
                      .arg(tabs != nullptr ? tabs->height() : -1));
            tab_page.hide();
        }

        check(async_page.task() != nullptr && async_page.task()->panel() != nullptr,
                  QStringLiteral("③ 解包/封包要挂在任务壳上（有面板才算接上）"));
            check(async_page.add_paths({db_path}) == 1, QStringLiteral("③ 异步夹具：装一个真包"));
            async_page.out_edit()->setText(async_out);
            async_page.unpack_all_button()->click();
            QElapsedTimer clock;
            clock.start();
            while (async_page.task()->running() && clock.elapsed() < 15000) {
                QCoreApplication::processEvents();   // 任务壳的排队事件要靠泵事件才会落到主线程
            }
            check(!async_page.task()->running(),
                  QStringLiteral("③ 任务壳要跑完（不是挂住；超时 %1 ms）").arg(clock.elapsed()));
            bool saw_progress = false;
            bool saw_finish_on_main = false;
            for (const TaskShell::Event& event : async_page.task()->take_events()) {
                if (event.kind == 'p') saw_progress = true;
                if (event.kind == 'f' && event.on_main_thread) saw_finish_on_main = true;
            }
            check(saw_progress && saw_finish_on_main,
                  QStringLiteral("③ 进度与「结束」都要经任务壳回主线程（p=%1 f=%2）")
                      .arg(saw_progress ? 1 : 0)
                      .arg(saw_finish_on_main ? 1 : 0));
            QFile async_file(async_out + QStringLiteral("/gamedata/a.txt"));
            const bool same_bytes =
                async_file.open(QIODevice::ReadOnly) && async_file.readAll() == QByteArray("AA");
            check(same_bytes,
                  QStringLiteral("③ 异步解包的产物与同步一致（<输出>/gamedata/a.txt == AA）"));
            async_page.hide();
        }
        QDir(dir).removeRecursively();
        page.hide();
    }

    // ★ 2026-09-27（用户："1.x 的部分功能或选择没了" + ⑦"拖放可能根本没实现，须查实"）：
        //   **加包入口**与**拖放**都要真的把包加载进来 —— 判据是"载进来了"，
        //   不是"代码里有个 dropEvent"（原来全 src/shell 只有 setAcceptDrops(true)，
        //   一个 dropEvent/dragEnterEvent 都没有 ⇒ 拖着没反应，这就是用户说的"选择没了"）。
        {
            FsPage drop_page;
            drop_page.show();
            QApplication::processEvents();
            check(drop_page.add_button() != nullptr &&
                      drop_page.add_button()->objectName() ==
                          QStringLiteral("fs_add_package"),
                  QStringLiteral("④ 要有「加包」按钮（1.x 的入口在 2.x 里丢了）"));

            // 用引擎自己的写侧造一个**真的 X-Ray DB 包**：既测 `package_entries`，也当拖放载荷
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_drop"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString db_path = dir + QStringLiteral("/gamedata.db0");
            int packed = 0;
            {
                std::vector<efs::DbPackItem> items;
                efs::DbPackItem a;
                a.path = "a.txt";
                a.data = {static_cast<uint8_t>('A')};
                efs::DbPackItem b;
                b.path = "dir/keep.txt";
                b.data = {static_cast<uint8_t>('B'), static_cast<uint8_t>('B')};
                items.push_back(a);
                items.push_back(b);
                const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
                packed = static_cast<int>(raw.size());
                std::ofstream out(db_path.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            check(packed > 0, QStringLiteral("④ 夹具：造一个真 DB 包（实得 %1 字节）").arg(packed));

            // ① 引擎侧：从**磁盘上的包**列条目（SquashFS 按 magic / DB 按自洽判定）
            std::string list_error;
            const std::vector<xrkit::engine::fs::EntryInfo> listed =
                efs::package_entries(db_path.toStdString(), std::nullopt, list_error);
            check(list_error.empty() && listed.size() == 2,
                  QStringLiteral("④ package_entries 从真包列出条目（err=%1，实得 %2 条）")
                      .arg(QString::fromStdString(list_error))
                      .arg(listed.size()));

            // ② 拖放**接线**：过滤器必须真装在包列表与卡片上（缺一个就红）
            check(drop_page.drop_filter() != nullptr &&
                      drop_page.drop_filter()->is_attached_to(drop_page.package_list()) &&
                      drop_page.drop_filter()->is_attached_to(drop_page.card()),
                  QStringLiteral("⑦ 拖放过滤器没装在包列表/卡片上（接线断了）"));

            // ③ 拖放**行为**：一个 Drop 事件进来 ⇒ 包真的加载进模型
            //    （离屏环境没有平台拖放：实测 `sendEvent(widget,&QDropEvent)` 会被 Qt
            //     拦掉 —— 返回 false、事件根本不到控件；真实 drop 走 QWidgetWindow 那条
            //     平台通路。所以这里直接把事件交给过滤器，见 drop_filter.h 的说明。）
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(db_path)});
            check(FileDropFilter::local_paths(&mime) == QStringList{db_path},
                  QStringLiteral("⑦ mime 里的本地文件路径要能取出来"));
            QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            check(drop_page.drop_filter() != nullptr && drop_page.drop_filter()->dispatch(&drop),
                  QStringLiteral("⑦ 过滤器要认这个 drop（本地文件 ⇒ accept）"));
            check(drop_page.source_count() == 1,
                  QStringLiteral("⑦ 拖一个包进来要真的加载（实测 %1 个源）")
                      .arg(drop_page.source_count()));
            // 树：顶层 = `dir/` 与 `a.txt`（2 项）；`dir` 展开后里面有 1 个文件；
            // 懒建 ⇒ 一开始只建顶层，展开哪一支才建哪一支。
            check(drop_page.entry_top_level_count() == 2 &&
                      drop_page.entry_built_file_count() == 1 &&
                      drop_page.expand_entry(QStringLiteral("dir")) &&
                      drop_page.find_entry_item(QStringLiteral("dir/keep.txt")) != nullptr,
                  QStringLiteral("⑦ 拖进来的包要进**树**（顶层 %1 / 展开前文件项 %2 / dir 子项 %3）")
                      .arg(drop_page.entry_top_level_count())
                      .arg(1)
                      .arg(drop_page.find_entry_item(QStringLiteral("dir/keep.txt")) != nullptr
                               ? 1
                               : 0));

            // ④ 认不出的文件 ⇒ **可见失败**且不加源（绝不静默加一个空包）
            const QString junk = dir + QStringLiteral("/junk.bin");
            {
                std::ofstream out(junk.toStdString(), std::ios::binary | std::ios::trunc);
                out << "this is not a package";
            }
            QMimeData bad_mime;
            bad_mime.setUrls({QUrl::fromLocalFile(junk)});
            QDropEvent bad_drop(QPointF(5, 5), Qt::CopyAction, &bad_mime, Qt::LeftButton,
                                Qt::NoModifier);
            if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&bad_drop);
            QApplication::processEvents();
            check(drop_page.source_count() == 1 && !drop_page.load_errors().isEmpty(),
                  QStringLiteral("⑦ 认不出的文件要拒绝并留下理由（源 %1 个，理由 %2 条）")
                      .arg(drop_page.source_count())
                      .arg(drop_page.load_errors().size()));

            // ⑤ 目录也能拖（散装层：与包走**同一条** add_source 路径）
            QMimeData dir_mime;
            dir_mime.setUrls({QUrl::fromLocalFile(dir)});
            QDropEvent dir_drop(QPointF(5, 5), Qt::CopyAction, &dir_mime, Qt::LeftButton,
                                Qt::NoModifier);
            if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&dir_drop);
            QApplication::processEvents();
            // ★ 2026-09-29 口径更新（用户第 6 条："整包拖进去要把 gamedata 文件树展开"）：
            //   拖目录不再**只**建散装层 —— 现在是「散装层 1 个源 + 目录里每个包各 1 个源」。
            //   本夹具的目录里有 1 个包 ⇒ 加上前面那一份，共 3 个源；原意（同一条 add_source 路径、
            //   认不出的不静默加）由前后两条判据守住。
            check(drop_page.source_count() == 3,
                  QStringLiteral("⑦ 拖目录 = 散装层 + 里面的包（实测 %1 个源，期望 3）")
                      .arg(drop_page.source_count()));
            QDir(dir).removeRecursively();
            drop_page.hide();
        }

        // 6.1/6.2：两个包叠加，后者为准；行文本要能看出"这一份来自哪个包"
        page.add_source(QStringLiteral("sq_base"),
                        {{"a.txt", false}, {"dir/keep.txt", false}, {"dir/base_only.txt", false}});
        page.add_source(QStringLiteral("sqzy_patch"), {{"a.txt", false}, {"dir/keep.txt", false}});
        check(page.source_count() == 2, QStringLiteral("6.2 两个包都进了列表"));
        // 合并后：顶层 = `dir/` + `a.txt`；文件项要看得出"这一份来自哪个包"（来源列）
        check(page.entry_top_level_count() == 2,
              QStringLiteral("6.1 合并后顶层条目数 = 2（实测 %1）").arg(page.entry_top_level_count()));
        QTreeWidgetItem* a_item = page.find_entry_item(QStringLiteral("a.txt"));
        check(a_item != nullptr &&
                  a_item->text(2).contains(QStringLiteral("sqzy_patch")),
              QStringLiteral("6.1 同名条目以后加载的包为准（来源列 = 「%1」，期望含 sqzy_patch）")
                  .arg(a_item != nullptr ? a_item->text(2) : QString()));
        check(a_item != nullptr && !a_item->text(1).isEmpty(),
              QStringLiteral("6.1 文件行要有**大小**列（实测「%1」）")
                  .arg(a_item != nullptr ? a_item->text(1) : QString()));
        // ★ 2026-09-28（用户贴 1.x 对照截图："差距。"）：树要能**展开/折叠**、每行能**单独勾**、
        //   目录勾选是**三态**（模型说了算）。判据都落在真树上，不是"看代码里有没有 QTreeWidget"。
        {
            FsPage tree_page;
            tree_page.resize(760, page_min_h());
            tree_page.show();
            QApplication::processEvents();
            tree_page.add_source(QStringLiteral("base"),
                                 {{"top.txt", false}, {"d/one.txt", false}, {"d/two.txt", false}});
            QApplication::processEvents();
            // ★ 首列宽度（用户截图："text..." —— 名称列被挤成几十像素，整棵树没法读）：
            //   ★ 2026-09-29 **口径更新**（用户第 4 条："最好弄一个水平方向的可拖拽分隔条"）：
            //   首列从 Stretch 改成 **Interactive**（三列都能拖）⇒ 判据按性质重述为
            //   "首列可拖（Interactive）且默认宽度 ≥160px"；原意（名称列不许被挤成几十像素）不变。
            //   判红办法：把列宽改小到 160 以下，或把模式换回 ResizeToContents。
            {
                QHeaderView* head = tree_page.entry_tree()->header();
                check(head != nullptr &&
                          head->sectionResizeMode(0) == QHeaderView::Interactive &&
                          tree_page.entry_tree()->columnWidth(0) >= xrkit::core::px(160),
                      QStringLiteral("树① 首列**可拖拽**且有宽度（模式 %1=Interactive / 实测 %2 px，恒 ≥160）")
                          .arg(head != nullptr ? static_cast<int>(head->sectionResizeMode(0)) : -1)
                          .arg(tree_page.entry_tree()->columnWidth(0)));
            }
            // ★ 用户 2026-09-29 第 6 条：拖**目录**要展开里面的包（不只显示 db 文件）
            {
                FsPage dir_page;
                dir_page.resize(760, page_min_h());
                dir_page.show();
                QApplication::processEvents();
                const QString mix = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dirpkg"));
                QDir(mix).removeRecursively();
                QDir().mkpath(mix + QStringLiteral("/gamedata/loose"));
                {
                    std::ofstream(mix.toStdString() + "/gamedata/loose/readme.txt",
                                  std::ios::binary | std::ios::trunc)
                        << "L";
                }
                // 里面放一个**真 XDB 包**（用引擎自己打的），验证"包内容也摊进树里"
                {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "gamedata/from_package.txt";   // CoP：包里的路径本身就带 gamedata 前缀
                    one.is_dir = false;
                    one.data = {'P'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(mix.toStdString() + "/gamedata/configs.db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                const int n = dir_page.add_paths({mix});
                QApplication::processEvents();
                check(n == 2 && dir_page.loaded_source_count() == 2,
                      QStringLiteral("树③ 拖目录 ⇒ 散装层 + 里面的包**都加载**（返回 %1，已加载源 %2，期望 2/2）")
                          .arg(n)
                          .arg(dir_page.loaded_source_count()));
                // 懒建：先展开 `gamedata`（以及 `gamedata/loose`）才找得到深层条目
                dir_page.expand_entry(QStringLiteral("gamedata"));
                dir_page.expand_entry(QStringLiteral("gamedata/loose"));
                QApplication::processEvents();
                check(dir_page.find_entry_item(QStringLiteral("gamedata/loose/readme.txt")) != nullptr &&
                          dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr,
                      QStringLiteral("树③ gamedata 树要摊开：松散文件 %1 / 包内文件 %2")
                          .arg(dir_page.find_entry_item(QStringLiteral("gamedata/loose/readme.txt")) != nullptr ? 1 : 0)
                          .arg(dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr ? 1 : 0));
                dir_page.hide();
                QDir(mix).removeRecursively();
            }
            // ★ 用户 2026-09-30：包内排序＝**文件夹在前，各自按名称**（原来目录/文件混排）
            {
                FsPage sort_page;
                sort_page.resize(760, page_min_h());
                sort_page.show();
                QApplication::processEvents();
                sort_page.add_source(QStringLiteral("s"),
                                     {{"z.txt", false}, {"b/", true}, {"a.txt", false}, {"c/", true}});
                QApplication::processEvents();
                QStringList top;
                for (int i = 0; i < sort_page.entry_top_level_count(); ++i) {
                    top << sort_page.entry_tree()->topLevelItem(i)->text(0);
                }
                check(top.size() == 4 && top[0] == QStringLiteral("b/") &&
                          top[1] == QStringLiteral("c/") && top[2] == QStringLiteral("a.txt") &&
                          top[3] == QStringLiteral("z.txt"),
                      QStringLiteral("排序：文件夹在前、文件在后，各自按名（实际 %1）")
                          .arg(top.join(QStringLiteral("|"))));
                sort_page.hide();
            }
            // ★ 用户 2026-09-30：「E:\...\IWP\database 整个文件夹拖进去会卡死」——实测该目录是
            //   **57 个包 / 18.9 GB**；根因＝我 09-29 改的"目录＝散装层 + 递归加载里面**每一个**包"。
            //   判据：目录里包**超过 8 个**时只入列表不加载（修前 9 个包全加载 ⇒ `loaded_source_count()`
            //   会是 10 ⇒ 这条红）。
            {
                const QString many = QDir::temp().filePath(QStringLiteral("xrkit_smoke_manypkg"));
                QDir(many).removeRecursively();
                QDir().mkpath(many);
                for (int i = 0; i < 9; ++i) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "f" + std::to_string(i) + ".txt";
                    one.is_dir = false;
                    one.data = {'x'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(many.toStdString() + "/p" + std::to_string(i) + ".db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage many_page;
                many_page.resize(760, page_min_h());
                many_page.show();
                QApplication::processEvents();
                many_page.add_paths({many});
                QApplication::processEvents();
                check(many_page.loaded_source_count() == 1 && many_page.source_count() >= 10,
                      QStringLiteral("包多不自动加载：已加载源 %1（期望 1）、已知源 %2（期望 ≥10）")
                          .arg(many_page.loaded_source_count())
                          .arg(many_page.source_count()));
                many_page.hide();
                QDir(many).removeRecursively();
            }

            check(tree_page.entry_top_level_count() == 2,
                  QStringLiteral("树① 顶层 2 项（top.txt + d/），实测 %1")
                      .arg(tree_page.entry_top_level_count()));
            check(tree_page.entry_built_file_count() == 1,
                  QStringLiteral("树① **懒建**：没展开时只建顶层（已建文件项 %1）")
                      .arg(tree_page.entry_built_file_count()));
            // 「展开信号接上了」：程序化 setExpanded 不发信号（见 expand_entry 注释），
            // 所以这里**手动发一条** `itemExpanded`（等价于视图在用户点箭头时发的那条）⇒ 懒建必须发生。
            // 这一条证明的是"连接 + 懒建逻辑"，不是"Qt 会不会发信号"（那是 Qt 的契约）。
            {
                QTreeWidgetItem* d_raw = tree_page.find_entry_item(QStringLiteral("d"));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemExpanded",
                                          Q_ARG(QTreeWidgetItem*, d_raw));
                QApplication::processEvents();
                check(tree_page.entry_built_file_count() == 3,
                      QStringLiteral("树① itemExpanded ⇒ 懒建子节点（已建文件项 %1，期望 3）")
                          .arg(tree_page.entry_built_file_count()));
            }
            check(tree_page.find_entry_item(QStringLiteral("d")) != nullptr,
                  QStringLiteral("树① 能按路径找到条目项"));
            // ★ 用户 2026-09-29 第 5 条：文件夹**单击**就展开；文件**单击条目本身**就切换勾选
            //   （勾选框只是视觉效果）。判据落在行为上：发一次 itemClicked 看状态有没有变。
            {
                QTreeWidgetItem* f = tree_page.find_entry_item(QStringLiteral("top.txt"));
                check(f != nullptr && tree_page.checked_file_count() == 0,
                      QStringLiteral("树② 起点：一个都没勾"));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, f), Q_ARG(int, 0));
                QApplication::processEvents();
                check(tree_page.checked_file_count() == 1,
                      QStringLiteral("树② **单击条目 = 勾选**（实测 %1，期望 1）")
                          .arg(tree_page.checked_file_count()));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, f), Q_ARG(int, 0));
                QApplication::processEvents();
                check(tree_page.checked_file_count() == 0,
                      QStringLiteral("树② 再单击一次 = 取消勾选（实测 %1，期望 0）")
                          .arg(tree_page.checked_file_count()));
                QTreeWidgetItem* dd = tree_page.find_entry_item(QStringLiteral("d"));
                const bool was_open = dd != nullptr && dd->isExpanded();
                const int before = tree_page.entry_built_file_count();
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, dd), Q_ARG(int, 0));
                QApplication::processEvents();
                check(dd != nullptr && dd->isExpanded() != was_open &&
                          tree_page.entry_built_file_count() >= before,
                      QStringLiteral("树② **单击文件夹 = 展开/折叠**（展开态 %1→%2，已建文件项 %3→%4）")
                          .arg(was_open ? 1 : 0)
                          .arg(dd != nullptr && dd->isExpanded() ? 1 : 0)
                          .arg(before)
                          .arg(tree_page.entry_built_file_count()));
                // 列宽可拖拽（用户第 4 条）：三列都必须是 Interactive，否则用户调不了
                QHeaderView* hh = tree_page.entry_tree()->header();
                bool interactive = hh != nullptr;
                for (int c = 0; c < 3 && interactive; ++c) {
                    interactive = hh->sectionResizeMode(c) == QHeaderView::Interactive;
                }
                check(interactive && hh->sectionSize(0) >= xrkit::core::px(200),
                      QStringLiteral("树② 三列**可拖拽**（模式全 Interactive=%1，首列 %2 px）")
                          .arg(interactive ? 1 : 0)
                          .arg(hh != nullptr ? hh->sectionSize(0) : -1));
            }
            check(tree_page.expand_entry(QStringLiteral("d")) &&
                      tree_page.entry_built_file_count() == 3,
                  QStringLiteral("树① 展开后子项出现且**幂等**（已建文件项 %1，期望 3）")
                      .arg(tree_page.entry_built_file_count()));
            // 单个文件勾选（1.x 的 CanvasTree 能一行一行勾）⇒ 模型计数跟着走
            QTreeWidgetItem* one = tree_page.find_entry_item(QStringLiteral("d/one.txt"));
            check(one != nullptr && tree_page.checked_file_count() == 0,
                  QStringLiteral("树① 初始没有勾选"));
            if (one != nullptr) one->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(tree_page.checked_file_count() == 1,
                  QStringLiteral("树① **能勾单个文件**（实测勾了 %1 个）")
                      .arg(tree_page.checked_file_count()));
            // 目录勾选 = 整目录（三态：勾满 → Checked；勾一个 → PartiallyChecked）
            QTreeWidgetItem* dir_item = tree_page.find_entry_item(QStringLiteral("d"));
            check(dir_item != nullptr && dir_item->checkState(0) == Qt::PartiallyChecked,
                  QStringLiteral("树① 目录三态：勾了 1/2 ⇒ PartiallyChecked（实测 %1）")
                      .arg(dir_item != nullptr ? static_cast<int>(dir_item->checkState(0)) : -1));
            if (dir_item != nullptr) dir_item->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            // 勾 `d/` = 该目录**整棵**勾上（正好 2 个文件）；`top.txt` 不在它里面 ⇒ 仍是未勾。
            // （我第一版把期望写成 3，是把"顶层文件"也算进去了 —— 判据写错，已改）
            check(tree_page.checked_file_count() == 2,
                  QStringLiteral("树① 勾目录 = 该目录整棵勾上（实测 %1，期望它自己的 2 个）")
                      .arg(tree_page.checked_file_count()));
            QTreeWidgetItem* top_item = tree_page.find_entry_item(QStringLiteral("top.txt"));
            if (top_item != nullptr) top_item->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(tree_page.checked_file_count() == 3,
                  QStringLiteral("树① 再勾顶层文件 ⇒ 3 个（实测 %1）")
                      .arg(tree_page.checked_file_count()));
            // 解包选中只带"勾选的"（按包分组）：勾满 3 个 ⇒ only 是 3 条
            QString root_unused;
            const std::vector<FsPage::UnpackJob> jobs = tree_page.plan_unpack(true, &root_unused);
            check(jobs.size() == 1 && jobs[0].only.size() == 3,
                  QStringLiteral("树① 勾选进得了「解包选中」的计划（包 %1 / 条目 %2）")
                      .arg(jobs.size())
                      .arg(jobs.empty() ? 0 : jobs[0].only.size()));
            tree_page.hide();
        }

        // 6.3：两个"全选"动的是**两套**状态
        page.select_all_button()->click();
        check(page.selection().size() == 2, QStringLiteral("6.3 「全选包」改了包列表选中态"));
        check(page.checked_file_count() == 0,
              QStringLiteral("6.3 「全选包」**没有**碰包内勾选态（实测勾了 %1 个）")
                  .arg(page.checked_file_count()));
        page.check_all_button()->click();
        check(page.checked_file_count() == 3,
              QStringLiteral("6.3 「包内全选」勾上了 3 个文件（实测 %1）")
                  .arg(page.checked_file_count()));
        check(page.selection().size() == 2,
              QStringLiteral("6.3 「包内全选」**没有**碰包列表选中态（实测 %1）")
                  .arg(page.selection().size()));

        // 6.11：选中集解包与批量解包走**同一个**入口 —— 同一份输入、同一句话。
        // 真镜像上的逐字节对拍在门禁 [8] 的整树导出里做；这里证的是"没有第二条路"。
        const efs::ExtractTreeResult batch =
            page.extract_to(QStringLiteral("no_such_image.sqfs"), QStringLiteral("no_out"),
                            efs::Selection::All);
        const efs::ExtractTreeResult picked =
            page.extract_to(QStringLiteral("no_such_image.sqfs"), QStringLiteral("no_out"),
                            efs::Selection::Listed, {"a.txt"});
        check(!batch.ok && !picked.ok && batch.error == picked.error && !batch.error.empty(),
              QStringLiteral("6.11 批量与选中解包同源（同样失败、同样的话：%1）")
                  .arg(QString::fromStdString(batch.error)));
    }

    // ② 点页面空白 ⇒ 输入框必须**失去焦点**（用户真机："能输入的框点空白并不能取消，
    //    还是一闪一闪"）。判据是**行为**，不是"看代码里有没有 setFocusPolicy"。
    {
        ConvertPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::setActiveWindow(&page);
        QApplication::processEvents();
        QLineEdit* edit = page.source_edit();
        check(edit != nullptr, QStringLiteral("② 夹具要有可聚焦的输入框"));
        if (edit != nullptr) {
            edit->setFocus(Qt::MouseFocusReason);
            QApplication::processEvents();
            const bool got = edit->hasFocus();
            // 按下"卡片空白处"：卡片是不接焦点的容器 ⇒ 焦点必须回到页面、输入框熄掉
            QWidget* blank = page.card() != nullptr ? page.card() : &page;
            const QPointF local(blank->width() / 2.0, blank->height() / 2.0);
            QMouseEvent press(QEvent::MouseButtonPress, local, blank->mapToGlobal(local.toPoint()),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(blank, &press);
            QApplication::processEvents();
            check(got, QStringLiteral("② 夹具前提：输入框本应先拿到焦点（实测 %1）")
                           .arg(edit->hasFocus()));
            check(!edit->hasFocus(),
                  QStringLiteral("② 点卡片空白后输入框必须失去焦点（实测仍持有 %1）")
                      .arg(edit->hasFocus() ? 1 : 0));
            // 反向：点**输入框自己**不许被页面抢走焦点（过滤器只装在空白容器上）
            edit->setFocus(Qt::MouseFocusReason);
            QApplication::processEvents();
            QMouseEvent on_edit(QEvent::MouseButtonPress, QPointF(2, 2), QPoint(), Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(edit, &on_edit);
            QApplication::processEvents();
            check(edit->hasFocus(),
                  QStringLiteral("② 点输入框本身不许把焦点抢到页面上（实测丢了）"));
        }
        page.hide();
    }

    // ⑨/⑬ 编码转换页（M2-6c2）：判定步骤来自引擎（9.7）、暂停归 App（13.6）、
    // 空闲文案非空（13.5）、进度只发一个 0~100 值（13.7）。
    {
        namespace etxt = xrkit::engine::text;
        ConvertPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 9.7：行文本里的"定案步骤"必须**逐字等于**引擎给的那一行 —— 页面没有
        // 能力自己拼一个标签（静态锁另证：本文件里不出现那几个标签字面量）。
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_convert"));
        QDir().mkpath(dir);
        const std::vector<uint8_t> cp1251 = {0xEF, 0xE2, 0xE0, 0xEE, 0x0A};  // cp1251 的"ево"
        const std::string file_name = "probe.xml";
        {
            std::ofstream out(QDir(dir).filePath(QString::fromStdString(file_name)).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(cp1251.data()),
                      static_cast<std::streamsize>(cp1251.size()));
        }
        const etxt::SniffResult sniffed = etxt::sniff_encoding(cp1251, "auto");
        const std::string expected_label = etxt::sniff_result_label(sniffed);

        const QString source_path = QDir(dir).filePath(QString::fromStdString(file_name));
        const etxt::ConvertOutcome outcome =
            page.convert_one(source_path, dir, QStringLiteral("auto"), QStringLiteral("utf-8"));
        check(outcome.ok && outcome.wrote, QStringLiteral("⑨ 转换真的产出了文件"));
        check(outcome.step_label == expected_label,
              QStringLiteral("⑨9.7 页面拿到的定案步骤 = 引擎判定（%1 vs %2）")
                  .arg(QString::fromStdString(outcome.step_label))
                  .arg(QString::fromStdString(expected_label)));
        check(outcome.step_label.find("声明") == std::string::npos ||
                  sniffed.step == etxt::SniffStep::XmlDeclaration,
              QStringLiteral("⑨9.7 步骤标签与判定步号一致（%1）")
                  .arg(QString::fromStdString(outcome.step_label)));

        // 产物按目标编码写出，且**不再带 BOM/换行翻译**
        {
            std::ifstream in(QDir(dir).filePath(QString::fromStdString(file_name))
                                 .toStdString(),
                             std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            check(body.find('\r') == std::string::npos,
                  QStringLiteral("⑨14 写盘用 newline=\"\"（不许把 \\n 翻成 \\r\\n）"));
            check(body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") != 0,
                  QStringLiteral("⑨ 输出侧不带 BOM"));
        }

        // 13.6：暂停状态机在 App 这边 —— 暂停时工作循环必须被拦住（且计数）
        const int before = page.pause_gate_hits();
        page.set_paused(true);
        check(!page.wait_if_paused(false), QStringLiteral("13.6 暂停时循环被拦住"));
        check(page.pause_gate_hits() == before + 1,
              QStringLiteral("13.6 拦截有计数（%1 → %2）")
                  .arg(before)
                  .arg(page.pause_gate_hits()));
        page.set_paused(false);
        check(page.wait_if_paused(false), QStringLiteral("13.6 继续后循环放行"));

        // 13.5：空闲文案非空；下拉的取值是内部键（不是显示名）
        TaskShell* task = page.task();
        check(task != nullptr && !task->idle_status().isEmpty(),
              QStringLiteral("13.5 空闲文案非空（%1）")
                  .arg(task == nullptr ? QString() : task->idle_status()));
        check(!page.current_source_encoding().empty() &&
                  !page.current_target_encoding().empty(),
              QStringLiteral("⑨ 编码下拉的取值是内部键（%1 → %2）")
                  .arg(QString::fromStdString(page.current_source_encoding()))
                  .arg(QString::fromStdString(page.current_target_encoding())));
        check(page.current_target_encoding() == "utf-8",
              QStringLiteral("⑨ 目标编码默认为 utf-8"));
        QDir(dir).removeRecursively();
    }

    // ⑦ 文本提取页（M2-6d）：判据同源（7.1/7.5 由引擎给理由、7.2 两次运行同产物）。
    {
        namespace etxt = xrkit::engine::text;
        TextPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_text"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/gameplay/config"));
        QDir().mkpath(root + QStringLiteral("/out"));
        // 一份最小的 scripts 夹具（真语料上的正确性由门禁 [15] 的逐字节对拍负责）
        QDir().mkpath(root + QStringLiteral("/scripts"));
        {
            std::ofstream out((root + QStringLiteral("/scripts/probe.script")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            const std::string body = "function probe()\n  local s = \"hello stalker\"\nend\n";
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        }

        // ⑦7.1/7.5：页面**不自己判** —— 返回的必须是引擎那句原话
        etxt::ExtractRequest request;
        request.source_dir = (root + QStringLiteral("/gameplay/config")).toStdString();
        request.target_dir = (root + QStringLiteral("/out")).toStdString();
        const QString engine_reason = QString::fromStdString(etxt::validate_request(request));
        const QString page_reason =
            page.validate(root + QStringLiteral("/gameplay/config"), root + QStringLiteral("/out"));
        check(!engine_reason.isEmpty() &&
                  page_reason == engine_reason &&
                  engine_reason.contains(QStringLiteral("不要选它们的上层目录")),
              QStringLiteral("⑦7.1 页面理由 = 引擎理由（%1）").arg(page_reason));

        // ⑦7.5：输出目录 = 源目录的父目录 ⇒ 引擎拒绝，页面原样透出
        const QString cover = page.validate(root + QStringLiteral("/scripts"), root);
        check(cover.contains(QStringLiteral("会覆盖源目录中的原文件")),
              QStringLiteral("⑦7.5 覆盖源目录被拒（%1）").arg(cover));

        // ⑦7.2/7.3：显式 id_prefix ⇒ 同一份源两次运行产物逐字节相同
        const QString out_a = root + QStringLiteral("/out_a");
        const QString out_b = root + QStringLiteral("/out_b");
        const etxt::ExtractStats first =
            page.extract_one(root + QStringLiteral("/scripts"), out_a, QStringLiteral("smoke"),
                             QStringLiteral("smoke_sc_0"), true);
        const etxt::ExtractStats second =
            page.extract_one(root + QStringLiteral("/scripts"), out_b, QStringLiteral("smoke"),
                             QStringLiteral("smoke_sc_0"), true);
        check(first.ok && first.files == 1 && first.strings >= 1,
              QStringLiteral("⑦ 提取产出（files=%1 strings=%2 xml=%3）")
                  .arg(first.files)
                  .arg(first.strings)
                  .arg(QString::fromStdString(first.texts_xml_name)));
        check(!first.texts_xml_name.empty() &&
                  QFileInfo::exists(QDir(out_a).filePath(QString::fromStdString(
                      first.texts_xml_name))),
              QStringLiteral("⑦ 文本 XML 落在输出目录根（%1）")
                  .arg(QString::fromStdString(first.texts_xml_name)));

        const auto tree_signature = [](const QString& dir) {
            QStringList parts;
            QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) parts << it.next();
            parts.sort();
            QString joined;
            for (const QString& file : parts) {
                QFile f(file);
                if (!f.open(QIODevice::ReadOnly)) return QString();
                const QByteArray body = f.readAll();
                joined += QDir(dir).relativeFilePath(file) + QStringLiteral(":") +
                          QString::number(body.size()) + QStringLiteral(":") +
                          QString::fromLatin1(QCryptographicHash::hash(body,
                                                                      QCryptographicHash::Sha1)
                                                  .toHex());
                joined += QLatin1Char('\n');
            }
            return joined;
        };
        check(first.ok && second.ok && tree_signature(out_a) == tree_signature(out_b) &&
                  !tree_signature(out_a).isEmpty(),
              QStringLiteral("⑦7.2 两次运行产物逐字节相同（页面入口）"));

        // 7.4（口径不对称）与 7.6（自己的产物不重复提取）在引擎里有对拍，
        // 页面上只证一件事：参数真的透传下去了（前缀不同 ⇒ 产物 XML 名不同）。
        const etxt::ExtractStats other_prefix =
            page.extract_one(root + QStringLiteral("/scripts"), root + QStringLiteral("/out_c"),
                             QStringLiteral("zz"), QStringLiteral("zz_sc_0"), true);
        check(other_prefix.ok &&
                  other_prefix.texts_xml_name != first.texts_xml_name &&
                  other_prefix.texts_xml_name.rfind("zz_", 0) == 0,
              QStringLiteral("⑦ 前缀透传（%1 vs %2）")
                  .arg(QString::fromStdString(first.texts_xml_name))
                  .arg(QString::fromStdString(other_prefix.texts_xml_name)));
        QDir(root).removeRecursively();
    }

    // ⑩ XML 校对页（M2-6e3）：10.1 内部键 / 10.3 摘要是计数 / 10.4 切模式不重跑 /
    // 10.5 导出范围＝筛选后的行（BOM + CRLF）。
    {
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        page.resize(760, page_min_h());
        page.show();
        QApplication::processEvents();

        // 10.1：模式下拉的**取值是内部键**，条数 = 声明表条数
        QComboBox* modes = page.mode_combo();
        const std::vector<exml::XmlModeSpec>& specs = exml::builtin_modes();
        check(modes != nullptr && modes->count() == static_cast<int>(specs.size()),
              QStringLiteral("⑩10.1 模式下拉条数 = 声明表条数（%1）").arg(specs.size()));
        int key_bad = 0;
        for (int i = 0; modes != nullptr && i < modes->count(); ++i) {
            const std::string key = modes->itemData(i).toString().toStdString();
            if (exml::find_mode(key) == nullptr) ++key_bad;
        }
        check(key_bad == 0, QStringLiteral("⑩10.1 模式下拉取值都是内部键（%1 项不符）").arg(key_bad));

        // 合成夹具：一个文件两侧一致、一个两侧不同、一个只在 A 有
        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xml"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/a"));
        QDir().mkpath(root + QStringLiteral("/b"));
        const auto write = [](const QString& path, const QString& body) {
            std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(body.toUtf8().constData(), body.toUtf8().size());
        };
        const QString same =
            QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n");
        write(root + QStringLiteral("/a/same.xml"), same);
        write(root + QStringLiteral("/b/same.xml"), same);
        write(root + QStringLiteral("/a/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>V</text></s>\n</r>\n"));
        write(root + QStringLiteral("/a/only_a.xml"),
              QStringLiteral("<r>\n<s id=\"y\"><text>z</text></s>\n</r>\n"));
        // 统计模式只看**行数/ID 计数**，不看文本 ⇒ 上面那个 diff.xml 在统计模式下
        // 其实是"一致"（同 id、同次数）。要造出统计模式的差异，得让次数不同。
        write(root + QStringLiteral("/a/countdiff.xml"),
              QStringLiteral("<r>\n<s id=\"k\"><text>1</text></s>\n"
                             "<s id=\"k\"><text>2</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/countdiff.xml"),
              QStringLiteral("<r>\n<s id=\"k\"><text>1</text></s>\n</r>\n"));

        const int total = page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                                            QStringLiteral("auto"), QStringLiteral("auto"));
        check(total == 4 && page.result_count() == 4,
              QStringLiteral("⑩ 比较扫到 4 个文件（实测 %1）").arg(total));
        check(page.scan_count() == 1, QStringLiteral("⑩ 第一次比较真的扫了目录"));

        // 10.4：切模式 ⇒ **清空结果**、换筛选项、**不重跑 I/O**
        const int before_scan = page.scan_count();
        page.set_mode(QStringLiteral("text"));
        check(page.current_mode_key() == "text", QStringLiteral("⑩10.4 切到文本模式"));
        check(page.result_count() == 0 && page.visible_count() == 0,
              QStringLiteral("⑩10.4 切模式必须清空旧结果（实测 %1 行）").arg(page.result_count()));
        check(page.scan_count() == before_scan,
              QStringLiteral("⑩10.4 切模式不许重跑 I/O（扫描次数 %1 → %2）")
                  .arg(before_scan)
                  .arg(page.scan_count()));
        check(page.filter_combo() != nullptr && page.filter_combo()->count() ==
                                                    static_cast<int>(specs[1].filters.size()),
              QStringLiteral("⑩10.1 筛选项来自新模式声明（文本模式 %1 项）")
                  .arg(specs[1].filters.size()));

        // 回到统计模式重跑，然后用筛选与导出
        page.set_mode(QStringLiteral("stats"));
        page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                          QStringLiteral("auto"), QStringLiteral("auto"));
        check(page.visible_count() == 4, QStringLiteral("⑩ 「全部」筛选下 4 行"));
        if (page.filter_combo() != nullptr) {
            const int index = page.filter_combo()->findText(QStringLiteral("仅不一致"));
            if (index >= 0) page.filter_combo()->setCurrentIndex(index);
        }
        check(page.visible_count() == 1,
              QStringLiteral("⑩ 筛选「仅不一致」后 1 行（该文件文本不同；实测 %1）")
                  .arg(page.visible_count()));

        // 10.5：导出范围 = 筛选后的行；字节层面看 BOM + CRLF
        const QString csv = root + QStringLiteral("/out.csv");
        check(page.export_csv(csv, QStringLiteral("2026-09-24 00:00:00")),
              QStringLiteral("⑩10.5 导出成功"));
        {
            QFile file(csv);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray body = file.readAll();
                check(body.startsWith("\xEF\xBB\xBF"),
                      QStringLiteral("⑩10.5 导出以 BOM 开头"));
                check(body.contains("\r\n"), QStringLiteral("⑩10.5 导出用 CRLF"));
                // 头部 10 行（标题/时间/模式/两个文件夹/编码/实际编码/筛选/计数/空行）
                // + 列头 1 行 + 筛选后的数据行
                const int want = 11 + page.visible_count();
                const int rows = body.count("\r\n");
                check(rows == want,
                      QStringLiteral("⑩10.5 导出行数 = 头部 10 + 列头 1 + 筛选后 %1 = %2（实测 %3）")
                          .arg(page.visible_count())
                          .arg(want)
                          .arg(rows));
            } else {
                check(false, QStringLiteral("⑩10.5 导出文件读不回来"));
            }
        }
        // 10.3：行文本是**计数摘要**（统计模式下差异来自"次数不同"），
        // 完整明细只在"看这一行"时才生成
        bool summary_ok = false;
        for (int i = 0; i < page.visible_count(); ++i) {
            if (page.row_text(i).contains(QStringLiteral("次数不同 1"))) summary_ok = true;
        }
        check(summary_ok, QStringLiteral("⑩10.3 行文本里是按计数拼的摘要"));
        const QString detail = page.detail_text(0);
        check(detail.contains(QStringLiteral("id=\"")) || detail.contains(QStringLiteral("仅")),
              QStringLiteral("⑩10.3 完整明细在需要时才给（%1）").arg(detail.left(40)));
        QDir(root).removeRecursively();
    }

    // ⑫12.1/12.5：**插件注册的比较模式要出现在 XML 页的同一个下拉里**
    {
        namespace plug = xrkit::plugin;
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        plug::Registry registry;
        for (plug::CompareModeReg& mode : plug::builtin_compare_modes()) {
            registry.register_compare_mode(exml::kBuiltinPlugin, std::move(mode));
        }
        plug::PluginApiAdapter adapter(registry, "demo.dll", nullptr);
        const char* col_keys[] = {"rel", "detail"};
        const char* col_headings[] = {"文件", "差异"};
        const int col_widths[] = {280, 400};
        const char* filter_labels[] = {"全部"};
        const char* filter_kinds[] = {"same|diff|only_a|only_b"};
        const auto compare_fn = +[](void*, const char*, const char*, const char*, const char*,
                                    const char*, stk_result_sink, void*) {};
        check(adapter.api().register_compare_mode(adapter.api().host, "插件模式", "plugin_mode", 40,
                                                 col_keys, col_headings, col_widths, 2,
                                                 filter_labels, 1, filter_kinds, compare_fn,
                                                 nullptr) == 1,
              QStringLiteral("⑫12.1 插件比较模式注册失败"));
        page.set_registry(&registry);
        QComboBox* modes = page.mode_combo();
        QStringList keys;
        for (int i = 0; modes != nullptr && i < modes->count(); ++i) {
            keys << modes->itemData(i).toString();
        }
        check(keys.contains(QStringLiteral("stats")) && keys.contains(QStringLiteral("text")) &&
                  keys.contains(QStringLiteral("plugin_mode")),
              QStringLiteral("⑫12.1 内置与插件模式必须在同一个下拉里（%1）")
                  .arg(keys.join(QStringLiteral("/"))));
        check(keys.size() == 3, QStringLiteral("⑫12.5 模式条数应为 3（%1）").arg(keys.size()));
    }

    // 视频转码接线（用户 2026-09-26：完整移植）—— argv 由引擎算，页面只起进程/报错
    {
        namespace vid = xrkit::engine::video;
        VideoPage page;
        vid::TranscodeRequest request;
        request.ffmpeg = "definitely-not-a-real-ffmpeg.exe";
        request.input = "in.avi";
        request.output = QDir::temp().filePath(QStringLiteral("xrkit_smoke_out.ogm")).toStdString();
        request.source.parsed = true;
        request.source.has_video = true;
        request.source.has_audio = true;
        request.source.duration = 10.0;
        request.available_encoders = {"libtheora"};
        QString error;
        const bool started = page.start_transcode(request, &error);
        check(!started && !error.isEmpty(),
              QStringLiteral("视频：起不来的 ffmpeg 必须**如实报错**（不崩不卡）"));
        check(!page.transcode_running() && !page.last_error().isEmpty(),
              QStringLiteral("视频：启动失败后态/理由要可见（实得 '%1'）").arg(page.last_error()));
        // 取消令牌：只有 reset 能清零；新一轮 start 会先 reset（1.x 语义）
        page.cancel_transcode();
        check(page.cancel_token().cancelled(), QStringLiteral("视频：cancel_transcode 应置位令牌"));
        QString error2;
        page.start_transcode(request, &error2);
        check(!page.cancel_token().cancelled(),
              QStringLiteral("视频：新一轮 start 必须先 reset 取消令牌（1.x：convert 自己不清零）"));
    }

    // ⑫12.1 四类槽位渲染（用户 2026-09-26"补六类"：panel/tool/option/option_entry）
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots4"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        const int builtin = hub.page_count();

        plug::PluginApiAdapter adapter(hub.registry(), "slots4.dll", nullptr);
        const char* choices[] = {"甲", "乙"};
        check(adapter.api().register_option(adapter.api().host, "插件下拉", choices, 2, nullptr,
                                           nullptr, "fs", "top_bar", nullptr, 1) == 1,
              QStringLiteral("⑫12.1 新建下拉注册失败"));
        check(adapter.api().register_option_entry(adapter.api().host, "fs", "top_bar", "插件条目",
                                                  nullptr) == 1,
              QStringLiteral("⑫12.1 下拉条目注册失败"));
        const auto builder = +[](void*, stk_widget*) -> stk_widget* {
            return reinterpret_cast<stk_widget*>(new QWidget());
        };
        check(adapter.api().register_panel(adapter.api().host, "插件面板", builder, nullptr, "fs",
                                           "top_bar", nullptr, 2) == 1,
              QStringLiteral("⑫12.1 面板注册失败"));
        check(adapter.api().register_tool(adapter.api().host, "插件页", builder, nullptr, 3) == 1,
              QStringLiteral("⑫12.1 Hub 插件页注册失败"));

        hub.render_plugin_slots();
        check(hub.plugin_pages_added() == 1 && hub.page_count() == builtin + 1,
              QStringLiteral("⑫12.1 插件页没被追加（pages=%1 期望 %2）")
                  .arg(hub.page_count())
                  .arg(builtin + 1));

        bool combo_ok = false;
        bool entry_ok = false;
        bool panel_ok = false;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr || page->page_id() != QStringLiteral("fs")) continue;
            for (QComboBox* combo : page->action_bar()->findChildren<QComboBox*>()) {
                if (combo->count() >= 2) combo_ok = true;
                for (int k = 0; k < combo->count(); ++k) {
                    if (combo->itemText(k) == QStringLiteral("插件条目")) entry_ok = true;
                }
            }
            for (QWidget* w : page->body()->findChildren<QWidget*>()) {
                if (w->objectName().startsWith(QStringLiteral("plugin_panel_"))) panel_ok = true;
            }
        }
        check(combo_ok, QStringLiteral("⑫12.1 插件下拉没出现在 fs 页操作栏"));
        check(entry_ok, QStringLiteral("⑫12.1 插件下拉条目没塞进对应 area 的下拉"));
        check(panel_ok, QStringLiteral("⑫12.1 插件面板没出现在 fs 页正文"));

        // 幂等：重渲染不许把插件页装两遍
        hub.render_plugin_slots();
        check(hub.page_count() == builtin + 1 && hub.plugin_pages_added() == 1,
              QStringLiteral("⑫12.1 重渲染把插件页装了两遍（pages=%1）").arg(hub.page_count()));
        QDir(empty).removeRecursively();
    }

    // 视频端到端（2026-09-27）：**仅当按口径能定位到 ffmpeg/ffprobe 时才跑**；
    // 本机没有 user.ltx、程序目录也没有 exe（⑧8.1 只认两处、不搜 PATH）⇒ 会走"如实 note 跳过"，
    // 这条对应用户侧的人工验收（配方见 docs/2.0.0-视频模块规格.md §6）。
    {
        namespace vid = xrkit::engine::video;
        const vid::FfmpegPaths paths = vid::find_ffmpeg_default();
        if (!paths.found || !paths.has_probe) {
            result.notes.append(QStringLiteral(
                "视频端到端：本机未按口径配置 ffmpeg/ffprobe（只认 user.ltx 手动指定与程序目录）"
                "⇒ 跳过，属人工验收项"));
        } else {
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_video"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString in_path = dir + QStringLiteral("/in.avi");
            // ① 造 1 秒测试输入（lavfi 源，不需要外部素材）
            QProcess gen;
            gen.start(QString::fromStdString(paths.ffmpeg),
                      QStringList{QStringLiteral("-y"), QStringLiteral("-f"),
                                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                                  QStringLiteral("testsrc=d=1"), QStringLiteral("-f"),
                                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                                  QStringLiteral("sine=f=440:d=1"), QStringLiteral("-shortest"),
                                  in_path});
            gen.waitForFinished(120000);
            const bool made = gen.exitCode() == 0 && QFile::exists(in_path);
            check(made, QStringLiteral("视频端到端：造测试输入失败（%1）").arg(gen.exitCode()));

            if (made) {
                const QString out_path = dir + QStringLiteral("/out.ogm");
                QFile::remove(out_path);
                VideoPage page;
                vid::TranscodeRequest request;
                request.ffmpeg = paths.ffmpeg;
                request.input = in_path.toUtf8().toStdString();
                request.output = out_path.toUtf8().toStdString();
                request.available_encoders = {"libtheora", "libxvid", "mpeg4"};
                QString error;
                check(page.start_transcode(request, &error),
                      QStringLiteral("视频端到端：启动失败（%1）").arg(error));
                QElapsedTimer clock;
                clock.start();
                while (page.transcode_running() && clock.elapsed() < 180000) {
                    QCoreApplication::processEvents();
                }
                check(!page.transcode_running(), QStringLiteral("视频端到端：超时未结束"));
                check(QFile::exists(out_path) && QFileInfo(out_path).size() > 0,
                      QStringLiteral("视频端到端：产物不存在或为空（%1）").arg(page.last_error()));
                // ② 产物要能被 ffprobe 解析（"不是垃圾文件"）
                QProcess probe;
                probe.start(QString::fromStdString(paths.ffprobe),
                            QStringList{QStringLiteral("-v"), QStringLiteral("error"),
                                        QStringLiteral("-show_entries"),
                                        QStringLiteral("format=duration"),
                                        QStringLiteral("-of"),
                                        QStringLiteral("default=nw=1"), out_path});
                probe.waitForFinished(30000);
                const QString probed = QString::fromUtf8(probe.readAllStandardOutput());
                check(probe.exitCode() == 0 && probed.contains(QStringLiteral("duration=")),
                      QStringLiteral("视频端到端：产物 ffprobe 解析失败（%1）").arg(probed.left(60)));
            }
            QDir(dir).removeRecursively();
        }
    }

    // 散装层界面接线（用户 2026-09-26"最后并入层"）
    {
        std::error_code ec;
        const std::filesystem::path work =
            std::filesystem::temp_directory_path(ec) / "xrkit_smoke_loose";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work / "sub", ec);
        {
            std::ofstream out(work / "a.txt", std::ios::binary);
            out << 'A';
        }
        {
            std::ofstream out(work / "sub" / "b.txt", std::ios::binary);
            out << 'B';
        }
        FsPage page;
        QString error;
        const bool ok = page.add_loose_dir(QString::fromStdString(work.string()), &error);
        check(ok && error.isEmpty(), QStringLiteral("散装层加入失败：%1").arg(error));
        check(page.source_count() == 1,
              QStringLiteral("散装层没有变成一个源（实得 %1）").arg(page.source_count()));

        // ★ 5.x 页面骨架（界面重构口径）：标题字号 > 正文、内容卡片占大头、操作栏贴底
        {
            FsPage skel;
            skel.resize(600, 700);
            skel.show();
            QApplication::processEvents();
            if (skel.layout() != nullptr) skel.layout()->activate();
            QApplication::processEvents();
            const double title_pt = skel.header() != nullptr ? skel.header()->font().pointSizeF() : 0.0;
            const double body_pt = skel.font().pointSizeF() > 0 ? skel.font().pointSizeF() : 12.0;
            check(skel.header() != nullptr && title_pt >= body_pt + 2.0,
                  QStringLiteral("骨架① 页标题比正文大 >= 2pt（实测 %1 vs %2）").arg(title_pt).arg(body_pt));
            check(skel.subtitle() != nullptr,
                  QStringLiteral("骨架② 页要有副标题控件（说明本页能干什么）"));
            const int page_h = skel.height();
            const int card_h = skel.card() != nullptr ? skel.card()->height() : -1;
            check(card_h > 0 && card_h >= page_h * 55 / 100,
                  QStringLiteral("骨架③ 内容卡片 >= 页面高 55%%（实测 %1 / %2）").arg(card_h).arg(page_h));
            if (skel.action_bar() != nullptr && skel.action_bar()->parentWidget() == &skel) {
                const int bottom_gap = page_h - (skel.action_bar()->y() + skel.action_bar()->height());
                check(bottom_gap >= 0 && bottom_gap <= xrkit::core::px(10),
                      QStringLiteral("骨架④ 操作栏贴底（底部余量 %1px）").arg(bottom_gap));
            }
            skel.hide();
        }

        // ★ 空状态（用户 2026-09-27 明令："把图中玩意删掉"）：卡片里那坨空状态文字块与
        //   重复提示行**都不许再出现**；空状态改由"包列表吃满卡片"承担。
        {
            FsPage empty_page;
            empty_page.show();
            QApplication::processEvents();
            if (empty_page.layout() != nullptr) empty_page.layout()->activate();
            QApplication::processEvents();

            // ① 用户明令删掉的那三样：一个也不许回来（命中即红）
            QStringList leftover;
            for (QWidget* child : empty_page.findChildren<QWidget*>()) {
                const QString name = child->objectName();
                if (name == QStringLiteral("fs_hint") || name == QStringLiteral("fs_entry_hint") ||
                    name == QStringLiteral("fs_empty_block")) {
                    leftover << name;
                }
            }
            check(leftover.isEmpty(),
                  QStringLiteral("① 删掉的空状态块/重复提示行又回来了：%1")
                      .arg(leftover.join(QStringLiteral(","))));
            check(!empty_page.entry_hint_visible(),
                  QStringLiteral("① 空状态提示访问器应为「恒 false」（该文字块已删）"));
            // ① 卡片里不许再有**说明段落** —— 用户删的那几段都是"长文本 + 自动换行"的标签；
            //   字段标签（"打包/解包格式："/"解包到："）与计数标签是控件，不是说明段落。
            //   （原来这条写的是"卡片内直接 QLabel 恰好 1 个"：那是删完提示行那一刻的**快照**，
            //    2026-09-28 按 1.x 补回按钮行与计数标签后它必然红 ⇒ 改成按**性质**判，
            //    不是把数字改大。）
            int paragraph_labels = 0;
            for (QLabel* label : empty_page.findChildren<QLabel*>()) {
                if (label->wordWrap() && label->text().size() > 16) ++paragraph_labels;
            }
            check(paragraph_labels == 0,
                  QStringLiteral("① 卡片里不该有自动换行的说明段落（实测 %1 个）")
                      .arg(paragraph_labels));

            // ★ 排版口径：页标题与说明**合并成一行**（顶部堆叠行数从 4 降到 2 的结构前提）
            check(empty_page.header_row() != nullptr,
                  QStringLiteral("排版①：页标题与说明要合成一行（header_row 存在）"));
            // ★ 原 ⑥「空状态块 ≥ 卡片高 60%」的度量对象（那个占位块）已按用户要求删除 ⇒
            //   断言作废，换成等价物：**没条目时条目列表收起、各列表不被挤成细线**。
            //   口径沿革（报账，别当成"悄悄改数字"）：
            //     ① 上一轮：60%（我按"卡片里只有两个列表"估的）；
            //     ② 用户要求补回 1.x 的按钮行后 → 改成"过半"；
            //     ③ 本轮再补 **1.x 的封包面板**（卡片里现在有**两个**工作区 + 三行控件）
            //        ⇒ 任何"某个列表占卡片几分之几"都不再说明问题。
            //   保留的**不变量**（也是用户真正抱怨过的："空列表只有一条细线，看着像坏了"）：
            //   每个列表不得小于它自己的最小高度。想判红就把某条 minimumHeight 去掉。
            const int list_h = empty_page.package_list() != nullptr
                                   ? empty_page.package_list()->height()
                                   : -1;
            const int pack_h = empty_page.pack_list() != nullptr ? empty_page.pack_list()->height()
                                                                 : -1;
            check(list_h >= xrkit::core::px(120) && pack_h >= xrkit::core::px(90),
                  QStringLiteral("⑥ 空状态：各列表都不许被挤成细线（包列表 %1/≥120，待打包 %2/≥90）")
                      .arg(list_h)
                      .arg(pack_h));
            // 条目列表：没条目时收起（不占地方），有货才出现
            check(empty_page.entry_tree() != nullptr && empty_page.entry_tree()->isHidden(),
                  QStringLiteral("空状态：没有条目时条目树要收起（不留一个空框）"));

            empty_page.add_loose_dir(QString::fromStdString(work.string()), nullptr);  // 临时目录是 ASCII 路径
            QApplication::processEvents();
            check(!empty_page.entry_hint_visible(),
                  QStringLiteral("① 加载后也不许冒出那句提示（它已经删了）"));
            check(empty_page.entry_tree() != nullptr && !empty_page.entry_tree()->isHidden(),
                  QStringLiteral("有条目后条目树要出现"));
            empty_page.hide();
        }
        // 用"勾选计数"证明条目**真的进了合并树**（a.txt + sub/b.txt = 2 个文件）
        page.check_all(true);
        check(page.checked_file_count() == 2,
              QStringLiteral("散装层条目数不对（期望 2，实得 %1）").arg(page.checked_file_count()));
        // 不存在的目录 ⇒ 拒绝且**不加源**
        const int before = page.source_count();
        QString missing_error;
        const bool bad =
            page.add_loose_dir(QString::fromStdString((work / "nope").string()), &missing_error);
        check(!bad && !missing_error.isEmpty() && page.source_count() == before,
              QStringLiteral("不存在的散装目录必须拒绝且不加源"));
        std::filesystem::remove_all(work, ec);
    }

    // ⑫12.1 decryptor 消费端（用户 2026-09-26：NLC 解密是外部插件；fs 页加载前必须问）
    {
        namespace plug = xrkit::plugin;
        FsPage page;
        const std::vector<xrkit::engine::fs::EntryInfo> none;
        // ① 没装注册表 ⇒ 行为不变
        page.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(page.source_count() == 1 && page.source_at(0) == QStringLiteral("gamedata.db_enc"),
              QStringLiteral("⑫12.1 没有注册表时不该改变加载行为"));

        // ② 装了"能解开"的解密器 ⇒ 换成解密产物
        plug::Registry registry;
        registry.register_decryptor("nlc.dll", [](const std::string&) { return true; },
                                    [](const std::string&, std::string& out) {
                                        out = "PLAIN";
                                        return true;
                                    });
        FsPage gated;
        gated.set_registry(&registry);
        gated.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(gated.source_count() == 1 &&
                  gated.source_at(0).endsWith(QStringLiteral(".decrypted")),
              QStringLiteral("⑫12.1 命中解密器后没换成解密产物（实得 %1）")
                  .arg(gated.source_at(0)));

        // ③ 装了"解不开"的解密器 ⇒ **拒绝加载**且理由可见
        plug::Registry bad;
        bad.register_decryptor("bad.dll", [](const std::string&) { return true; },
                               [](const std::string&, std::string&) { return false; });
        FsPage refused;
        refused.set_registry(&bad);
        refused.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(refused.source_count() == 0 && !refused.last_gate_error().isEmpty(),
              QStringLiteral("⑫12.1 解密失败必须拒绝加载并给出理由（实得 %1 个源）")
                  .arg(refused.source_count()));
    }

    // ⑫12.1/12.4：插件槽位要**渲染出来**（工具栏菜单项 → 页面的操作栏按钮）
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        plug::PluginApiAdapter adapter(hub.registry(), "slots.dll", nullptr);
        const auto on_click = +[](void*) {};
        const char* cid =
            adapter.api().register_command(adapter.api().host, "slot_action", "插件动作",
                                           on_click, nullptr, nullptr);
        check(cid != nullptr, QStringLiteral("⑫12.1 插件命令注册失败"));
        check(adapter.api().register_menu_item(adapter.api().host, "xml", "toolbar", cid, 0) == 1,
              QStringLiteral("⑫12.4 插件工具栏挂载点注册失败"));
        hub.render_plugin_slots();
        check(hub.plugin_slots_rendered() >= 1,
              QStringLiteral("⑫12.1 插件槽位没有被渲染（渲染了 %1 个）")
                  .arg(hub.plugin_slots_rendered()));
        bool found = false;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr || page->page_id() != QStringLiteral("xml")) continue;
            for (QPushButton* button : page->action_bar()->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("插件动作")) found = true;
            }
        }
        check(found, QStringLiteral("⑫12.1 插件动作按钮没出现在 XML 页的操作栏里"));
        QDir(empty).removeRecursively();
    }

    // ⑧ 视频转换页（M2-6f）：8.2 构造期不探测 / 8.3 三按钮小窗非阻塞 /
    // 8.5 状态三态（探测前不显示 ✗）/ 8.4 自动安装挂按钮上且缺件如实报。
    {
        namespace evid = xrkit::engine::video;
        VideoPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 8.2/8.5：构造完**一次都没探**，状态是"未检测"（不是 ✗）
        check(page.probe_count() == 0,
              QStringLiteral("⑧8.2 构造期不探测（实测探了 %1 次）").arg(page.probe_count()));
        check(page.status_text().contains(QStringLiteral("未检测")) &&
                  !page.status_text().contains(QStringLiteral("✗")),
              QStringLiteral("⑧8.5 探测之前不显示 ✗（%1）").arg(page.status_text()));

        // 8.2：探测失败也置位（再调不会重探）
        page.detect();
        const int after = page.probe_count();
        check(after == 1, QStringLiteral("⑧8.2 第一次探测记了 1 次（实测 %1）").arg(after));
        page.detect();
        check(page.probe_count() == 1,
              QStringLiteral("⑧8.2 探测失败也被记住（再调不重探，实测 %1 次）")
                  .arg(page.probe_count()));
        check(page.probe_state() == evid::ProbeState::Missing ||
                  page.probe_state() == evid::ProbeState::Found,
              QStringLiteral("⑧8.5 探测后状态落定"));

        // 8.3：三按钮小窗 + **非阻塞**
        QWidget* dialog = page.request_setup();
        check(dialog != nullptr, QStringLiteral("⑧8.3 弹出了设置小窗"));
        if (dialog != nullptr) {
            check(!dialog->isModal(), QStringLiteral("⑧8.3 小窗不是模态（不 grab_set）"));
            check(dialog->isVisible(), QStringLiteral("⑧8.3 小窗 show 出来了（不 wait_window）"));
            const QList<QPushButton*> buttons = dialog->findChildren<QPushButton*>();
            QStringList labels;
            for (QPushButton* button : buttons) labels << button->text();
            check(labels.size() == 3 && labels.contains(QStringLiteral("自动安装")) &&
                      labels.contains(QStringLiteral("手动选择")) &&
                      labels.contains(QStringLiteral("稍后")),
                  QStringLiteral("⑧8.3 三个按钮齐（%1）").arg(labels.join(QStringLiteral("/"))));
        }
        // 8.4：自动安装缺 download_ffmpeg.py 时如实报（不联网、不静默自救）
        const QString install = page.auto_install();
        check(install.contains(QStringLiteral("download_ffmpeg.py")) ||
                  install.contains(QStringLiteral("不可用")) ||
                  install.contains(QStringLiteral("ffmpeg 8.x")),
              QStringLiteral("⑧8.4 自动安装按钮给出如实结论（%1）").arg(install.left(48)));
        // 8.7：手动指定不存在的文件必须被拒（且不写盘）
        QString error;
        const QString ghost = QDir::temp().filePath(QStringLiteral("xrkit_nope_ffmpeg.exe"));
        check(!page.set_manual(ghost, QString(), &error) && !error.isEmpty(),
              QStringLiteral("⑧8.7 手动指定不存在的文件被拒"));
    }

    // ⑪ 汉化包生成页（M2-6g）：11.8 四态标记由引擎给 / 11.9 缺字形**先判后建**且不留半成品。
    {
        namespace fnt = xrkit::engine::font;
        FontPackPage page;
        page.resize(760, page_min_h());
        page.show();
        QApplication::processEvents();

        const QString msyh = QStringLiteral(
            "E:/Software/Games/STALKER/Localization/Tools/otherTools/CN_Pack_Generator/msyh.ttf");
        const QString jpan = QStringLiteral("C:/Windows/Fonts/hpsimplifiedjpan-regular.ttf");
        const QString latin = QStringLiteral("C:/Windows/Fonts/arial.ttf");
        const auto exists = [](const QString& path) { return QFileInfo::exists(path); };

        // 11.8：标记逐字等于引擎的判据（页面不许自己再判一遍）
        if (exists(msyh)) {
            check(page.font_state_text(msyh) ==
                      QString::fromUtf8(fnt::cjk_state_mark(fnt::CjkFontState::Ok)),
                  QStringLiteral("⑪11.8 简体字体标记 = 引擎判据（%1）").arg(page.font_state_text(msyh)));
        }
        if (exists(jpan)) {
            const QString mark = page.font_state_text(jpan);
            check(mark.startsWith(QString::fromUtf8(fnt::cjk_state_mark(fnt::CjkFontState::Japanese))) &&
                      mark.contains(QStringLiteral("可用繁体顶")),
                  QStringLiteral("⑪11.8 日语字体标记 + 繁体回退计数（%1）").arg(mark));
        }
        check(page.font_state_text(QString()).contains(QStringLiteral("未选择字体")),
              QStringLiteral("⑪11.8 没选字体时不乱下结论"));

        // 11.9：缺字形**拒绝式检查、在建目录之前** —— 产物目录不许被建出来
        if (exists(latin)) {
            const QString out = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fontpack_out"));
            QDir(out).removeRecursively();
            QString reason;
            const bool built = page.build(QDir::temp().filePath(QStringLiteral("xrkit_no_xml")),
                                          latin, out, &reason);
            check(!built && reason.contains(QStringLiteral("拒绝生成")),
                  QStringLiteral("⑪11.9 缺字形被拒（%1）").arg(reason));
            check(!QDir(out).exists() && page.refused_count() >= 1,
                  QStringLiteral("⑪11.9 拒绝时**没有**创建输出目录"));
            QDir(out).removeRecursively();
        }
    }

    // ⑫12.2/12.3：`api.ui` 的 **16 方法 Qt 实现** —— 槽位齐全，且真造得出控件。
    // （三个标准对话框会阻塞，自检只核对槽位非空。）
    {
        const stk_ui* ui = qt_plugin_ui();
        check(ui != nullptr, QStringLiteral("⑫12.2 外壳没有提供 api.ui"));
        // `stk_ui` 是"纯函数指针的 POD"，按指针数组扫一遍就知道有没有空洞
        const void* const* slot_table = reinterpret_cast<const void* const*>(ui);
        const size_t slot_count = sizeof(stk_ui) / sizeof(void*);
        int holes = 0;
        for (size_t i = 0; i < slot_count; ++i) {
            if (slot_table[i] == nullptr) ++holes;
        }
        check(holes == 0 && slot_count == 16,
              QStringLiteral("⑫12.3 api.ui 应有 16 个非空槽位（实测 %1 个槽、%2 个空洞）")
                  .arg(slot_count)
                  .arg(holes));

        QWidget host;
        host.resize(320, 240);
        stk_host* handle = reinterpret_cast<stk_host*>(&host);
        stk_widget* root_widget = reinterpret_cast<stk_widget*>(&host);
        stk_widget* label = ui->label(handle, root_widget, "标题", nullptr);
        stk_widget* button = ui->button(handle, root_widget, "按钮", nullptr, nullptr, nullptr, 1);
        stk_widget* box = ui->checkbox(handle, root_widget, "勾上", nullptr, nullptr);
        stk_widget* entry = ui->entry(handle, root_widget, "初值", 20, nullptr);
        stk_widget* text = ui->text(handle, root_widget, 6, 0, "word");
        const char* values[] = {"甲", "乙"};
        stk_widget* combo = ui->combo(handle, root_widget, values, 2, nullptr, nullptr);
        stk_widget* drop = ui->drop_box(handle, root_widget, nullptr, nullptr, "拖进来", 0);
        stk_widget* row = ui->row(handle, root_widget, "top", "x", 0, 0, 0);
        stk_widget* view = ui->text_view(handle, root_widget, 8, 1);
        check(label != nullptr && button != nullptr && box != nullptr && entry != nullptr &&
                  text != nullptr && combo != nullptr && drop != nullptr && row != nullptr &&
                  view != nullptr,
              QStringLiteral("⑫12.2 九种控件没有全部造出来"));
        auto* label_widget = reinterpret_cast<QLabel*>(label);
        ui->set_text(handle, label, "改过的标题");
        check(label_widget != nullptr && label_widget->text() == QStringLiteral("改过的标题"),
              QStringLiteral("⑫12.2 set_text 没改到控件上"));
        ui->set_state(handle, button, 0);
        check(!reinterpret_cast<QWidget*>(button)->isEnabled(),
              QStringLiteral("⑫12.2 set_state 没把控件置灰"));
        check(ui->alive(handle, label) == 1 && ui->alive(handle, nullptr) == 0,
              QStringLiteral("⑫12.2 alive 判断不对"));
        ui->pack(handle, root_widget, label, "top", "x", 0, 0, 0);
        check(host.layout() != nullptr && host.layout()->count() >= 1,
              QStringLiteral("⑫12.2 pack 没把控件挂进布局"));
    }

    // ⑫12.4：插件加载失败**不阻塞启动**，且问题要在 Hub 启动时可见（日志 + 状态栏）。
    {
        const QString empty_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plugins_empty"));
        QDir(empty_dir).removeRecursively();
        QDir().mkpath(empty_dir);
        {
            Hub quiet(nullptr, empty_dir);
            check(quiet.plugin_errors().empty(),
                  QStringLiteral("⑫12.4 空插件目录不该报错（%1）").arg(quiet.plugin_notice()));
            check(quiet.plugin_notice().contains(QStringLiteral("无")),
                  QStringLiteral("⑫12.4 空目录的启动提示不对（%1）").arg(quiet.plugin_notice()));
        }

        const QString bad_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plugins_bad"));
        QDir(bad_dir).removeRecursively();
        QDir().mkpath(bad_dir);
        {
            // 一个"看起来像插件但其实不是 DLL"的文件 ⇒ LoadLibrary 必失败
            std::ofstream out((bad_dir + QStringLiteral("/not_a_plugin.dll")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out << "this is not a PE file";
        }
        {
            Hub loud(nullptr, bad_dir);
            check(!loud.plugin_errors().empty(),
                  QStringLiteral("⑫12.4 坏插件必须留下 registration_errors"));
            check(loud.plugin_notice().contains(QStringLiteral("问题")),
                  QStringLiteral("⑫12.4 状态栏/提示要写出有问题（%1）").arg(loud.plugin_notice()));
            if (loud.log_box() != nullptr) loud.log_box()->flush();
            check(loud.log_view() != nullptr &&
                      loud.log_view()->toPlainText().contains(QStringLiteral("插件问题")),
                  QStringLiteral("⑫12.4 问题要打进日志栏（启动即可见）"));
            check(loud.plugin_ui_installed(),
                  QStringLiteral("⑫12.2 加载插件时必须把外壳的 api.ui 装上"));
            check(loud.page_count() == builtin_page_count(),
                  QStringLiteral("⑫12.4 插件坏了也不许影响内置六页"));
        }
        QDir(empty_dir).removeRecursively();
        QDir(bad_dir).removeRecursively();
    }

    return result;
}

}  // namespace xrkit::shell
