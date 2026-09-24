#include "smoke.h"

#include "xrkit/core/theme.h"
#include "hub.h"
#include "scroll_viewport.h"
#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>

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

    return result;
}

}  // namespace xrkit::shell
