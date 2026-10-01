// xrkit_app —— 2.x Qt6 外壳入口（M2）。
//
// 两种模式：
//   · 正常：起 Hub 主窗口，进事件循环。
//   · `--smoke`：**离屏**构造真实的控件树并跑自检，输出 ASCII 收尾行
//     `SHELL-SMOKE ok checks=N failed=M`，退出码 0/1 —— 门禁靠它判外壳（不靠人眼截图）。

#include "xrkit/core/theme.h"
#include "hub.h"
#include "pages/fs_page.h"
#include "pages/page_base.h"
#include "smoke.h"
#include "screen_scale.h"
#include "theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QScreen>
#include <QStringList>
#include <QTextStream>
#include <QTreeWidget>
#include <functional>

int main(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    const bool smoke = args.contains(QStringLiteral("--smoke"));
    // ★ 屏幕自适应诊断：打印「启动时按当前屏幕算出来的缩放与窗口尺寸」然后退出。
    //   为什么要有它：离屏自检只验了纯函数；这条分支验的是**真实启动路径**
    //   （QScreen 逻辑 DPI → snap → set_ui_scale → 窗口尺寸），可配合 QT_SCALE_FACTOR 实测。
    const bool scale_report = args.contains(QStringLiteral("--scale-report"));
    // ★ `--shot <png> [--size WxH] [--page N]`：**把真实窗口抓成图片**再退出。
    //   为什么要有它（2026-09-30 用户对着 FS 页排版发火）：我在离屏自检里量的几何"都对"，
    //   但他眼睛看到的是另一回事 —— 几何断言只能证明"我断言的那几条"，证明不了"看起来对不对"。
    //   有了截图我就能**自己看**，不用让他反复截图。离屏 + `QWidget::grab()` 不需要真实显示器。
    const int shot_arg = args.indexOf(QStringLiteral("--shot"));
    const QString shot_path = (shot_arg >= 0 && shot_arg + 1 < args.size()) ? args[shot_arg + 1]
                                                                           : QString();
    const int size_arg = args.indexOf(QStringLiteral("--size"));
    QSize shot_size(1600, 1000);
    if (size_arg >= 0 && size_arg + 1 < args.size()) {
        const QStringList wh = args[size_arg + 1].split(QLatin1Char('x'));
        if (wh.size() == 2) shot_size = QSize(wh[0].toInt(), wh[1].toInt());
    }
    const int page_arg = args.indexOf(QStringLiteral("--page"));
    const int shot_page = (page_arg >= 0 && page_arg + 1 < args.size()) ? args[page_arg + 1].toInt()
                                                                        : 0;
    // `--load <包路径>`：抓图前先往 FS 页里装一个真包（看"有内容"的那种状态 —— 空状态和装货状态
    // 长得完全不同，只看空状态等于没看）。
    const int load_arg = args.indexOf(QStringLiteral("--load"));
    const QString load_path = (load_arg >= 0 && load_arg + 1 < args.size()) ? args[load_arg + 1]
                                                                            : QString();
    // `--filter <文本>`：抓图前先往 FS 页的搜索框里打字（复现"筛选时多出一个空行"那类现象）
    const int filter_arg = args.indexOf(QStringLiteral("--filter"));
    const QString filter_text = (filter_arg >= 0 && filter_arg + 1 < args.size())
                                    ? args[filter_arg + 1]
                                    : QString();

    // 离屏：不依赖真实显示器（门禁机器上也可能没有交互桌面）
    if ((smoke || !shot_path.isEmpty()) && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // ★ 高 DPI 策略（屏幕自适应口径，2026-09-27）：让 Qt **保留分数缩放**。
    //   Qt6 默认 `Round` 会把 125%/133% 这类缩放先取整一次，而我们的 `snap_ui_scale()`
    //   还会再吸附一档 ⇒ 两处都在取整、结果不可预期。**必须在构造 QApplication 之前**设。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("STALKER Toolkit 2.x"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));

    if (scale_report) {
        const double applied = xrkit::shell::apply_screen_ui_scale();
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize avail = screen != nullptr ? screen->availableGeometry().size() : QSize(0, 0);
        const QSize win =
            xrkit::shell::adaptive_window_size(avail, xrkit::shell::app_min_window_size());
        const QSize minsz = xrkit::shell::app_min_window_size();
        QTextStream out(stdout);
        // ★ 关键取证（2026-09-27 实测发现的问题）：Windows 上 Qt6 的 **logicalDotsPerInch 恒为 96**
        //   （实测：150% 缩放的机器上 logical 可用区 1707x1019、logicalDotsPerInch 仍报 96.0）
        //   ⇒ 只看逻辑 DPI 的自适应**在真机上等于没做**。这里把三样都打出来，用来定正确口径：
        //   dpi=logicalDotsPerInch、pdpi=physicalDotsPerInch、dpr=devicePixelRatio、logical=屏幕逻辑尺寸。
        out << QStringLiteral("SCALE-APPLY dpi=%1 pdpi=%2 dpr=%3 scale=%4 screen=%5x%6 window=%7x%8 "
                              "min=%9x%10\n")
                   .arg(screen != nullptr ? screen->logicalDotsPerInch() : -1.0, 0, 'f', 1)
                   .arg(screen != nullptr ? screen->physicalDotsPerInch() : -1.0, 0, 'f', 1)
                   .arg(screen != nullptr ? screen->devicePixelRatio() : -1.0, 0, 'f', 2)
                   .arg(applied, 0, 'f', 2)
                   .arg(avail.width())
                   .arg(avail.height())
                   .arg(win.width())
                   .arg(win.height())
                   .arg(minsz.width())
                   .arg(minsz.height());
        out.flush();
        return 0;
    }

    if (smoke) {
        // 主题先按 user.ltx 的偏好铺上（读不到就用默认暗色）
        const std::string saved = xrkit::core::load_user_value("theme", "mode", "");
        xrkit::core::ThemeMode mode = xrkit::core::ThemeMode::Dark;
        if (!saved.empty()) xrkit::core::theme_mode_from_name(saved, mode);
        xrkit::shell::apply_theme(app, mode);

        const xrkit::shell::SmokeResult result = xrkit::shell::run_smoke();
        QTextStream out(stdout);
        for (const QString& note : result.notes) {
            out << "  SHELL-SMOKE note " << note << "\n";
        }
        for (const QString& failure : result.failures) {
            out << "  SHELL-SMOKE fail " << failure << "\n";
        }
        out << QStringLiteral("SHELL-SMOKE ok checks=%1 failed=%2 mode=%3\n")
                   .arg(result.checks)
                   .arg(result.failed)
                   .arg(QString::fromUtf8(xrkit::core::theme_mode_name(mode)));
        out.flush();
        return result.failed == 0 ? 0 : 1;
    }

    const std::string saved = xrkit::core::load_user_value("theme", "mode", "");
    xrkit::core::ThemeMode mode = xrkit::core::ThemeMode::Dark;
    if (!saved.empty()) xrkit::core::theme_mode_from_name(saved, mode);
    xrkit::shell::apply_theme(app, mode);

    // ★ 屏幕自适应（用户 2026-09-27："2.5K 屏…做一个自适应，所有屏幕都能正常用"）：
    //   ① 先按当前屏幕的逻辑 DPI 把全局 ui_scale 设对（**必须在建控件之前**，
    //      因为字体与几何都在构造时用 px() 换算）；
    //   ② 再按屏幕可用区给窗口一个合适的默认尺寸（小屏不溢出、大屏不缩成一小块）。
    xrkit::shell::apply_screen_ui_scale();

    xrkit::shell::Hub hub;
    xrkit::shell::apply_dark_titlebar(&hub, mode);
    {
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize avail = screen != nullptr ? screen->availableGeometry().size()
                                              : QSize(1280, 800);
        hub.resize(xrkit::shell::adaptive_window_size(avail, xrkit::shell::app_min_window_size()));
        hub.setMinimumSize(xrkit::shell::app_min_window_size());
    }
    hub.show();
    if (!shot_path.isEmpty()) {
        // 抓图前把布局彻底催到位（离屏下 `LayoutRequest` 不会自己派发）
        hub.resize(shot_size);
        hub.show();
        if (shot_page > 0 && shot_page < hub.page_count()) hub.set_current_page(shot_page);
        if (!load_path.isEmpty()) {
            if (auto* fs_page = dynamic_cast<xrkit::shell::FsPage*>(hub.page_at(0))) {
                fs_page->add_paths(QStringList{load_path});
                hub.set_current_page(0);
                if (!filter_text.isEmpty() && fs_page->search_edit() != nullptr) {
                    fs_page->search_edit()->setText(filter_text);
                }
                QTextStream out(stdout);
                out << QStringLiteral("SHOT load=%1 sources=%2 errors=%3\n")
                           .arg(load_path)
                           .arg(fs_page->source_count())
                           .arg(fs_page->load_errors().join(QStringLiteral(" | ")));
                out.flush();
            } else {
                QTextStream out(stdout);
                out << QStringLiteral("SHOT load=失败：第 0 页不是 FsPage\n");
                out.flush();
            }
        }
        for (int pass = 0; pass < 3; ++pass) {
            for (QWidget* child : hub.findChildren<QWidget*>()) {
                if (child->layout() != nullptr) child->layout()->activate();
            }
            if (hub.layout() != nullptr) hub.layout()->activate();
            QApplication::processEvents();
        }
        const bool ok_save = hub.grab().save(shot_path);
        QTextStream out(stdout);
        // 顺带把**解包页里的真实几何**打出来（`名称 x y w h 可见`）—— 截图能看出"丑"，
        // 但"哪一块占了谁的位置"要看数字（2026-09-30 用户对 FS 页排版发火后加的诊断）。
        if (xrkit::shell::PageBase* fs = hub.page_at(0)) {
            const auto dump = [&out](const char* tag, QWidget* w) {
                if (w == nullptr) return;
                out << QStringLiteral("GEOM %1 %2 x=%3 y=%4 w=%5 h=%6 vis=%7\n")
                           .arg(QString::fromLatin1(tag))
                           .arg(w->objectName())
                           .arg(w->x())
                           .arg(w->y())
                           .arg(w->width())
                           .arg(w->height())
                           .arg(w->isVisible() ? 1 : 0);
            };
            dump("page", fs);
            dump("body", fs->body());
            for (QWidget* child : fs->findChildren<QWidget*>()) {
                const QString name = child->objectName();
                if (name.startsWith(QStringLiteral("fs_")) ||
                    name.startsWith(QStringLiteral("page_")) ||
                    name == QStringLiteral("card")) {
                    dump("child", child);
                }
            }
            // 表头裁字专项诊断（用户 2026-09-30 与 2026-10-01 两次指出"那行字顶部被切"）
            if (auto* tree = fs->findChild<QTreeWidget*>(QStringLiteral("fs_entry_tree"))) {
                QHeaderView* h = tree->header();
                out << QStringLiteral(
                           "HDR tree_font_px=%1 tree_fm_h=%2 hdr_font_px=%3 hdr_fm_h=%4 "
                           "hdr_height=%5 hdr_sizeHint=%6\n")
                           .arg(tree->font().pixelSize())
                           .arg(tree->fontMetrics().height())
                           .arg(h != nullptr ? h->font().pixelSize() : -1)
                           .arg(h != nullptr ? h->fontMetrics().height() : -1)
                           .arg(h != nullptr ? h->height() : -1)
                           .arg(h != nullptr ? h->sizeHint().height() : -1);
                // 树里**实际建出来的行**（含缩进层级 + 文本 + 勾选态）——用来查"筛选时那个空行"
                int printed = 0;
                std::function<void(QTreeWidgetItem*, int)> dump_item = [&](QTreeWidgetItem* item,
                                                                           int depth) {
                    if (item == nullptr || printed >= 40) return;
                    ++printed;
                    out << QStringLiteral("ROW d=%1 text='%2' check=%3 kids=%4\n")
                               .arg(depth)
                               .arg(item->text(0))
                               .arg(static_cast<int>(item->checkState(0)))
                               .arg(item->childCount());
                    for (int i = 0; i < item->childCount(); ++i) dump_item(item->child(i), depth + 1);
                };
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    dump_item(tree->topLevelItem(i), 0);
                }
                out << QStringLiteral("ROW total_top=%1\n").arg(tree->topLevelItemCount());
            }
        }
        out << QStringLiteral("SHOT %1 path=%2 size=%3x%4 page=%5\n")
                   .arg(ok_save ? QStringLiteral("ok") : QStringLiteral("fail"))
                   .arg(shot_path)
                   .arg(hub.width())
                   .arg(hub.height())
                   .arg(shot_page);
        out.flush();
        return ok_save ? 0 : 1;
    }
    return QApplication::exec();
}
