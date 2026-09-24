#include "smoke.h"

#include "xrkit/core/theme.h"
#include "hub.h"
#include "log_box.h"
#include "pages/convert_page.h"
#include "pages/fs_page.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "pages/text_page.h"
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
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>
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

    // ② Hub 真的按这份清单建了控件（列表项 = 页栈页数 = 清单条数）
    Hub hub;
    check(hub.page_count() == builtin_page_count(), QStringLiteral("页栈页数 = 内置页数"));
    check(hub.page_list()->count() == builtin_page_count(), QStringLiteral("列表项数 = 内置页数"));
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
        check(send_wheel(native_text, -120), QStringLiteral("控件级拦：事件被吃掉（挡住原生自滚）"));
        check(native_text->verticalScrollBar()->value() == 0,
              QStringLiteral("控件级拦：QPlainTextEdit 自己没滚"));

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
        check(hub.top_min() == px(260) && hub.log_min() == px(110),
              QStringLiteral("4.5 两栏 minsize = px(260) / px(110)（实测 %1 / %2）")
                  .arg(hub.top_min())
                  .arg(hub.log_min()));

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
        check(!inside_viewport(hub.page_list()) && !inside_viewport(hub.stack()),
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

        // 6.1/6.2：两个包叠加，后者为准；行文本要能看出"这一份来自哪个包"
        page.add_source(QStringLiteral("sq_base"),
                        {{"a.txt", false}, {"dir/keep.txt", false}, {"dir/base_only.txt", false}});
        page.add_source(QStringLiteral("sqzy_patch"), {{"a.txt", false}, {"dir/keep.txt", false}});
        check(page.source_count() == 2, QStringLiteral("6.2 两个包都进了列表"));
        VirtualList* rows = page.entry_list();
        check(rows != nullptr && rows->row_count() == 4,
              QStringLiteral("6.1 合并后条目数 = 4（实测 %1）")
                  .arg(rows == nullptr ? -1 : rows->row_count()));
        bool patch_wins = false;
        for (int i = 0; rows != nullptr && i < rows->row_count(); ++i) {
            const QString text = rows->row_text(i);
            if (text.startsWith(QStringLiteral("a.txt")) &&
                text.contains(QStringLiteral("sqzy_patch"))) {
                patch_wins = true;
            }
        }
        check(patch_wins, QStringLiteral("6.1 同名条目以后加载的包为准（行里能看到来源包）"));

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

    return result;
}

}  // namespace xrkit::shell
