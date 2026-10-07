// xrkit_app —— 2.x Qt6 外壳入口（M2）。
//
// 两种模式：
//   · 正常：起 Hub 主窗口，进事件循环。
//   · `--smoke`：**离屏**构造真实的控件树并跑自检，输出 ASCII 收尾行
//     `SHELL-SMOKE ok checks=N failed=M`，退出码 0/1 —— 门禁靠它判外壳（不靠人眼截图）。

#include "xrkit/core/theme.h"
// ★ P2c：宿主诊断（`--python-host` / `--py-plugins`）走引擎层唯一实现（见下面的分支）
#include "xrkit/engine/plugin/py_host.h"
#include "hub.h"
#include "log_box.h"
#include "pages/convert_page.h"
#include "pages/fs_page.h"
#include "pages/fontpack_page.h"
#include "pages/page_base.h"
#include "pages/video_page.h"
#include "smoke.h"
#include "screen_scale.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>
#include <functional>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);

    // ★★ 插件改装 P2c/P7d：`--python-host` / `--py-plugins [目录]` / `--ui-facade` /
//   `--py-plugin-call [目录] [JSON]` —— **不建 QApplication**，
    //   直接跑宿主诊断后退出。为什么 GUI exe 也要开这两个开关：**发行包里只有
    //   `xrkit_app.exe`**（没有 CLI），而"包内自带的 Python 到底能不能起来、能不能加载 `.py`"
    //   必须在**包内**判（见 `scripts/package.ps1` 的包内自检）。与 CLI 的同名动作共用
    //   引擎层**唯一一份**实现 ⇒ 输出行格式逐字相同、不会分叉。
    if (args.contains(QStringLiteral("--python-host")) ||
        args.contains(QStringLiteral("--py-plugins")) ||
           args.contains(QStringLiteral("--py-plugin-call")) ||
           args.contains(QStringLiteral("--ui-facade"))) {
        std::vector<std::string> probe_args;
        probe_args.reserve(static_cast<size_t>(args.size()));
        for (const QString& arg : args) probe_args.push_back(arg.toStdString());
        QTextStream probe_out(stdout);
        const int probe_rc = xrkit::plugin::run_python_host_probe(
            probe_args, [&probe_out](const std::string& line) {
                probe_out << QString::fromStdString(line) << "\n";
                probe_out.flush();
            });
        return probe_rc;
    }
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
    // ★ 2026-10-06（D2 批）：`--font-picker` —— 抓图前把汉化包页的**系统字体选择器**打开，
    //   并把它**自己**也抓成 `<shot>.picker.png`。为什么需要这个开关：那个对话框是一个
    //   独立的顶层窗口，`hub.grab()` 抓不到它 ⇒ 不这样抓就等于"新做的对话框没有图"。
    //   （与 `--load` / `--video-in` 同一族诊断开关：让"有内容/有面板"的那种状态能被看见。）
    const bool shot_font_picker = args.contains(QStringLiteral("--font-picker"));
    // `--video-in <源文件>` / `--video-ref <参考 OGM>` / `--video-out <输出 OGM>`：
    //   抓视频页"有输入"的那个状态（空状态与有输入长得完全不同，只看空状态等于没看）。
    const int vin_arg = args.indexOf(QStringLiteral("--video-in"));
    const QString video_in =
        (vin_arg >= 0 && vin_arg + 1 < args.size()) ? args[vin_arg + 1] : QString();
    const int vref_arg = args.indexOf(QStringLiteral("--video-ref"));
    const QString video_ref =
        (vref_arg >= 0 && vref_arg + 1 < args.size()) ? args[vref_arg + 1] : QString();
    const int vout_arg = args.indexOf(QStringLiteral("--video-out"));
    const QString video_out =
        (vout_arg >= 0 && vout_arg + 1 < args.size()) ? args[vout_arg + 1] : QString();
    // ★ 2026-10-06：`--log-open` —— 抓图前把底部日志抽屉**展开**。
    //   为什么需要它：D1 批给日志面板加了「导出日志」（原来只有「清空」），而抽屉默认
    //   折叠 ⇒ 只看截图根本看不见那颗新按钮。与 `--filter`/`--video-in` 同一族诊断开关。
    const bool log_open = args.contains(QStringLiteral("--log-open"));

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
        const xrkit::core::ThemeMode mode = xrkit::core::load_theme_mode();
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

    // ★ 屏幕自适应 + 用户偏好（主题 / 界面大小）——**这条序列只有一份实现**
    //   （`xrkit::shell::apply_startup_ui`，理由见那边的 H-1 说明：这段原先是内联的，
    //   于是"字体与几何同源""界面大小不许翻主题"两条判据在离屏自检里复现不了）。
    //   顺序与原先逐条一致：先铺主题 → 屏幕自适应 → 保存档覆盖档位 → **再套一次主题**。
    const xrkit::core::ThemeMode mode = xrkit::shell::apply_startup_ui(app);

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
        // 日志抽屉展开（`--log-open`）：不展开的话「导出日志」按钮在抓图里是看不见的
        if (log_open && hub.log_toggle() != nullptr) hub.log_toggle()->click();
        // 视频页"有输入"：填三个框 + **等异步 ffprobe 落地**（不然面板还停在"正在解析"）
        if (!video_in.isEmpty() || !video_ref.isEmpty()) {
            if (auto* video_page =
                    dynamic_cast<xrkit::shell::VideoPage*>(hub.page_at(shot_page))) {
                if (!video_out.isEmpty()) video_page->output_edit()->setText(video_out);
                QElapsedTimer shot_clock;
                shot_clock.start();
                const auto settle = [&shot_clock](xrkit::shell::VideoPage* target,
                                                  bool want_source, bool want_reference) {
                    while (shot_clock.elapsed() < 30000) {
                        const bool source_ok = !want_source || target->source_ready();
                        const bool reference_ok = !want_reference || target->reference_ready();
                        if (source_ok && reference_ok) return;
                        QApplication::processEvents();
                        QThread::msleep(20);
                    }
                };
                // 走"拖入/选择…"那条**真实接线**（填框 → textChanged → 异步探测），
                // 页面口径：同一时刻只允许一次探测 ⇒ 两份文件**排队**等
                if (!video_in.isEmpty()) {
                    video_page->source_edit()->setText(QDir::toNativeSeparators(video_in));
                }
                settle(video_page, true, false);
                if (!video_ref.isEmpty()) {
                    video_page->reference_edit()->setText(QDir::toNativeSeparators(video_ref));
                }
                settle(video_page, true, true);
                QTextStream out(stdout);
                out << QStringLiteral("SHOT video src_ready=%1 ref_ready=%2 panel_lines=%3\n")
                           .arg(video_page->source_ready() ? 1 : 0)
                           .arg(video_page->reference_ready() ? 1 : 0)
                           .arg(video_page->info_panel_text().count(QLatin1Char('\n')) + 1);
                // 面板原文（换行折成 " | "）：截图里中文在离屏环境会退化成方块 ⇒ 判读靠这行文字
                out << QStringLiteral("SHOT video panel=%1\n")
                           .arg(QString(video_page->info_panel_text())
                                    .replace(QLatin1Char('\n'), QStringLiteral(" | ")));
                out.flush();
            }
        }
        if (!load_path.isEmpty()) {
            if (auto* fs_page = dynamic_cast<xrkit::shell::FsPage*>(hub.page_at(0))) {
                fs_page->add_paths(QStringList{load_path});
                hub.set_current_page(0);
                if (!filter_text.isEmpty() && fs_page->search_edit() != nullptr) {
                    fs_page->search_edit()->setText(filter_text);
                }
                QTextStream out(stdout);
                // ★ 2026-10-04：加 `entries=`（已加载源列出来的条目总数，与 CLI `--dump` 的
                //   `entries=` 同口径）—— 真机复验要拿它对数字（"GUI 的功能 CLI 全都有"
                //   不能靠看截图）。
                out << QStringLiteral("SHOT load=%1 sources=%2 entries=%3 errors=%4\n")
                           .arg(load_path)
                           .arg(fs_page->source_count())
                           .arg(fs_page->loaded_entry_count())
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
        // ★ 2026-10-05（D3 批）：编码转换页的下拉**逐项原文**打出来。
        //   为什么要这一行：离屏环境没有中文字体，截图里 combo 的**展开态**也抓不到；
        //   而"源/目标下拉里到底有哪几个编码"是这批的核心判据之一（big5/shift_jis/euc-kr
        //   三项要一一对应引擎的编码名）。数字与原文都得能从 stdout 读出来 ⇒ 打这行。
        if (auto* convert_page = dynamic_cast<xrkit::shell::ConvertPage*>(hub.page_at(1))) {
            const auto names = [](QComboBox* combo) {
                QStringList list;
                if (combo == nullptr) return list;
                for (int i = 0; i < combo->count(); ++i) list << combo->itemData(i).toString();
                return list;
            };
            out << QStringLiteral("SHOT convert source_enc=[%1]\n")
                       .arg(names(convert_page->source_combo()).join(QStringLiteral(",")));
            out << QStringLiteral("SHOT convert target_enc=[%1] target_default=%2\n")
                       .arg(names(convert_page->target_combo()).join(QStringLiteral(",")))
                       .arg(QString::fromStdString(convert_page->current_target_encoding()));
            out.flush();
        }
        // 顺带把**被抓那一页的真实几何**打出来（`名称 x y w h 可见`）—— 截图能看出"丑"，
        // 但"哪一块占了谁的位置"要看数字（2026-09-30 用户对 FS 页排版发火后加的诊断）。
        // ★ 2026-10-06：从"只量第 0 页"改成**量被抓的那一页**（D1 批要量文本页/XML 页的新控件）。
        if (xrkit::shell::PageBase* fs = hub.page_at(shot_page >= 0 && shot_page < hub.page_count()
                                                         ? shot_page
                                                         : 0)) {
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
            // ★ 2026-10-06（D1）：前缀不再写死 `fs_` —— 抓哪一页就量那一页的控件
            // ★ 2026-10-06（D1）：前缀不再写死 `fs_` —— 抓哪一页就量那一页的控件
            //   （文本页的 `text_stats`、XML 页的 `xml_stats`/`xml_clear`… 都要能量到）。
            const QString prefix = fs->page_id() + QStringLiteral("_");
            for (QWidget* child : fs->findChildren<QWidget*>()) {
                const QString name = child->objectName();
                // ★ 2026-10-06（D2）：汉化包页的页 id 是 `fontpack`、控件前缀却是 `font_`
                //   （历史命名）⇒ 只按页 id 取前缀会**一条都量不到**这页的控件。这里显式
                //   补上这个别名（纯诊断用；影响不了任何产品行为）。
                if (name.startsWith(prefix) || name.startsWith(QStringLiteral("page_")) ||
                    name.startsWith(QStringLiteral("font_")) ||
                    // ★ 2026-10-06（②）：任务壳面板也是这页的控件（`task_panel`/`task_status`）——
                    //   取消按钮删掉之后，这一页的 `task_cancel` **不该再被量到**（判据用数字说）。
                    name.startsWith(QStringLiteral("task_")) || name == QStringLiteral("card")) {
                    dump("child", child);
                }
            }
            // ★ 2026-10-06：离屏无字体（中文渲染成方块）⇒ 光看几何判不了文案。
            //   把**带对象名的 QLabel 原文**打出来（截图判几何、这行判文案，两处都留证据）。
            for (QLabel* label : fs->findChildren<QLabel*>()) {
                const QString name = label->objectName();
                if (!name.startsWith(prefix) && !name.startsWith(QStringLiteral("font_"))) continue;
                out << QStringLiteral("LBL %1 w=%2 h=%3 text=\"%4\"\n")
                           .arg(name)
                           .arg(label->width())
                           .arg(label->height())
                           .arg(QString(label->text()).replace(QLatin1Char('\n'),
                                                              QStringLiteral(" ⏎ ")));
            }
            // ★ 2026-10-06（③）：Hub 自己的日志区（`--log-open` 展开时才有几何）
            dump("hub", hub.log_toggle());
            dump("hub", hub.log_panel());
            dump("hub", hub.log_export_button());
            dump("hub", hub.log_clear_button());
            dump("hub", hub.log_box());
            // ★ 2026-10-02 用户圈着包列表表头打了个「？」（三列总宽 < 面板宽 ⇒ 右半边死区）：
            //   把**列宽合计 vs 视口宽**打出来，这种"没铺满"的毛病以后用数字判，不靠眯眼看图。
            if (auto* pkg = fs->findChild<QTreeWidget*>(QStringLiteral("fs_package_list"))) {
                QHeaderView* ph = pkg->header();
                QString widths;
                int total = 0;
                for (int c = 0; c < pkg->columnCount(); ++c) {
                    const int w = ph != nullptr ? ph->sectionSize(c) : -1;
                    total += w;
                    widths += QStringLiteral("%1 ").arg(w);
                }
                const int vp = pkg->viewport()->width();
                out << QStringLiteral("HDR pkg cols=%1 widths=[%2] total=%3 viewport=%4 fill=%5\n")
                           .arg(pkg->columnCount())
                           .arg(widths.trimmed())
                           .arg(total)
                           .arg(vp)
                           .arg(total >= vp - 2 ? 1 : 0);
                // ★ 2026-10-02 用户这次的头号诉求是"左边的包列表也该是文件树" ⇒
                //   把**层级**打出来（缩进 d / 文本 / 是不是源节点），用数字验收，不靠看图猜。
                int prow = 0;
                std::function<void(QTreeWidgetItem*, int)> dump_pkg = [&](QTreeWidgetItem* item,
                                                                         int depth) {
                    if (item == nullptr || prow >= 40) return;
                    ++prow;
                    out << QStringLiteral("PROW d=%1 text='%2' src=%3 exp=%4 chk=%5 kids=%6\n")
                               .arg(depth)
                               .arg(item->text(0))
                               .arg(item->data(0, Qt::UserRole).toInt())
                               .arg(item->isExpanded() ? 1 : 0)
                               .arg(static_cast<int>(item->checkState(0)))
                               .arg(item->childCount());
                    for (int i = 0; i < item->childCount(); ++i) {
                        dump_pkg(item->child(i), depth + 1);
                    }
                };
                for (int i = 0; i < pkg->topLevelItemCount(); ++i) {
                    dump_pkg(pkg->topLevelItem(i), 0);
                }
                out << QStringLiteral("PROW total_top=%1\n").arg(pkg->topLevelItemCount());
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
        // ★ D2 批：把"选择字体"对话框自己也抓一张（独立顶层窗口，hub.grab() 抓不到）。
        if (shot_font_picker) {
            if (auto* font_page = dynamic_cast<xrkit::shell::FontPackPage*>(
                    hub.page_at(shot_page >= 0 && shot_page < hub.page_count() ? shot_page : 0))) {
                if (QDialog* picker = font_page->make_font_picker(&hub)) {
                    picker->show();
                    QApplication::processEvents();
                    const QString picker_path = shot_path + QStringLiteral(".picker.png");
                    const bool picker_saved = picker->grab().save(picker_path);
                    auto* list = picker->findChild<QListWidget*>(QStringLiteral("font_picker_list"));
                    auto* preview =
                        picker->findChild<QLabel*>(QStringLiteral("font_picker_preview"));
                    auto* zoom = picker->findChild<QLabel*>(QStringLiteral("font_picker_zoom"));
                    auto* caption =
                        picker->findChild<QLabel*>(QStringLiteral("font_picker_caption"));
                    out << QStringLiteral(
                               "PICKER ok=%1 path=%2 size=%3x%4 items=%5 zoom=%6 "
                               "preview=%7x%8\n")
                               .arg(picker_saved ? QStringLiteral("ok") : QStringLiteral("fail"))
                               .arg(picker_path)
                               .arg(picker->width())
                               .arg(picker->height())
                               .arg(list != nullptr ? list->count() : -1)
                               .arg(zoom != nullptr ? zoom->text() : QStringLiteral("-"))
                               .arg(preview != nullptr ? preview->pixmap().width() : -1)
                               .arg(preview != nullptr ? preview->pixmap().height() : -1);
                    if (caption != nullptr) {
                        out << QStringLiteral("PICKER note caption=\"%1\"\n")
                                   .arg(QString(caption->text())
                                            .replace(QLatin1Char('\n'), QStringLiteral(" ⏎ ")));
                    }
                    picker->close();
                    delete picker;
                } else {
                    out << QStringLiteral("PICKER ok=fail reason=没有造出对话框\n");
                }
            } else {
                out << QStringLiteral("PICKER ok=fail reason=第 %1 页不是 FontPackPage（用 --page 5）\n")
                           .arg(shot_page);
            }
        }
        out.flush();
        return ok_save ? 0 : 1;
    }
    return QApplication::exec();
}
