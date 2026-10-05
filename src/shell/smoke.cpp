#include "smoke.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/version.h"
#include "xrkit/core/log.h"
#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/plugin/abi_host.h"
#include "xrkit/engine/plugin/loader.h"
#include "xrkit/engine/text/encoding.h"
#include "hub.h"
#include "dialog_start.h"
#include "drop_filter.h"
#include "drop_zone.h"
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
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QDropEvent>
#include <QUrl>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#if defined(_WIN32)
// 末尾「⑬ 版本资源」那一组要读**本进程 exe 的真实资源字节**（VERSIONINFO）。
//   · 只在 Win32 下有意义（本仓库只出 Windows 包；PE 子系统之类判据本来也都在 Win32 上）；
//   · NOMINMAX 与全局口径一致（根 CMakeLists 的 xrkit_flags）—— 这里再兜一次：没有它的话
//     windows.h 会定义 min/max 宏，把本文件里的 std::max / std::min 全弄坏（会编译失败）。
//   · 不用 WIN32_LEAN_AND_MEAN：它会把 winver.h 排除掉，而本组要的 GetFileVersionInfoW /
//     VerQueryValueW 就在 winver.h 里。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>
#endif

#include <cstdio>
#include <QHeaderView>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace xrkit::shell {
namespace {

// 判据用的期望值：**契约**（页 id 是插件槽位/日志的稳定标识），不是"再抄一份清单"。
// 清单本身从 `builtin_page()`（真实清单）读，两边交叉核对。
constexpr const char* kExpectedIds[] = {"fs", "convert", "text", "xml", "video", "fontpack"};
constexpr const char* kExpectedTitles[] = {"文件系统", "编码转换", "文本提取",
                                           "XML校对", "视频转换", "汉化包生成"};
constexpr int kExpectedCount = 6;

// ★★ 2026-10-02 包列表**树化**（用户：「左边的包列表的结构应该也是一个文件树」）之后，
//   "第 0 行"不再等于"第 0 个源" —— 源节点散在树里、中间还夹着**纯分组节点**。
//   所以夹具要按**节点上的源下标**（`data(0, Qt::UserRole)` ≥0）来找源，别再数顶层行。
// 包列表树里的**源节点**总数（纯分组节点不算）
int package_source_node_count(QTreeWidget* list) {
    int total = 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toInt() >= 0) ++total;
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
    return total;
}

// 选中包列表树里**所有源节点**（旧夹具用 `topLevelItem(i)->setSelected(true)`，树化后那样选不到源）
void select_all_package_sources(QTreeWidget* list) {
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toInt() >= 0) item->setSelected(true);
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
}

// 按名字找源节点（树里到处找）
QTreeWidgetItem* find_package_source_node(QTreeWidget* list, const QString& name_contains) {
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (found == nullptr && item->data(0, Qt::UserRole).toInt() >= 0 &&
            item->text(0).contains(name_contains, Qt::CaseInsensitive)) {
            found = item;
        }
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
    return found;
}

#if defined(_WIN32)
// 读**本进程那个 exe**的版本资源里某一个字符串字段（读不到返回空串）。
// 为什么直读 Win32 而不另起进程问 PowerShell：字段里有中文，管道/控制台码页会把它读坏 ——
// 判据要的是**逐字**相等，只能自己读资源。
QString exe_version_field(const QString& name) {
    const QString path = QCoreApplication::applicationFilePath();
    const auto* wide_path = reinterpret_cast<LPCWSTR>(path.utf16());
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide_path, &handle);
    if (size == 0) return QString();
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(wide_path, 0, size, buffer.data())) return QString();
    struct LangPair {
        WORD language;
        WORD code_page;
    };
    LangPair* pair = nullptr;
    UINT pair_size = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&pair), &pair_size) ||
        pair == nullptr || pair_size < sizeof(LangPair)) {
        return QString();
    }
    const QString sub = QStringLiteral("\\StringFileInfo\\%1%2\\%3")
                            .arg(pair->language, 4, 16, QLatin1Char('0'))
                            .arg(pair->code_page, 4, 16, QLatin1Char('0'))
                            .arg(name);
    void* value = nullptr;
    UINT value_size = 0;
    if (!VerQueryValueW(buffer.data(), reinterpret_cast<LPCWSTR>(sub.utf16()), &value,
                        &value_size) ||
        value == nullptr) {
        return QString();
    }
    return QString::fromWCharArray(static_cast<const wchar_t*>(value));
}
#endif

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

    // ★★ 2026-10-06（③）：落盘日志的目录 —— **先把自检的落盘目录改成临时目录**，
    //   免得门禁每跑一次就往构建目录里写一堆 `logs/log_*.txt`。默认口径先在这里钉住。
    const QString smoke_log_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_logs"));
    check(hub_log_dir() == QDir(QCoreApplication::applicationDirPath()).filePath(
                               QStringLiteral("logs")),
          QStringLiteral("③ 默认落盘目录 = exe 目录/logs（1.x `app_dir()/logs`；实得 %1）")
              .arg(hub_log_dir()));
    QDir(smoke_log_dir).removeRecursively();
    set_hub_log_dir(smoke_log_dir);

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
            // ★ 2026-10-04（用户：「**我截图的文字全部删除。**」）四页副标题也删了 ⇒ 一并进锁。
            //   判据仍照 2026-09-27 那条经验：**挑只有被删对象才有的字串**
            //   （用 `批量转换` 会打到合法文案；cp1251 / ffmpeg / XML 这类词更是满地都是）。
            QStringLiteral("逐条对比转换前后"),  // 编码转换页副标题（独有的那半句）
            QStringLiteral("提取可翻译文本"),      // 文本提取页副标题（独有的那半句）
            QStringLiteral("结构与条目差异"),      // XML 校对页副标题（独有的那半句）
            QStringLiteral("缺失时可见跳过"),      // 视频转换页副标题（独有的那半句）
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

    // ★★ 2026-10-04 用户第 1 条（原话）：「**首先，所有目录都需要拖拽，而且是拖拽到相应区域。**」
    //   判据分三层，全部**可判红**：
    //     ① 结构：每一页"要选目录/文件"的输入（QLineEdit）都落在**比它更大**的一个拖放区域里，
    //        那个区域真的装了 `FileDropFilter`（区域 ≠ 输入框本身 —— 旧做法就是挂在框上）：
    //        ⚠ 判据**不能用** `acceptDrops()`：Qt6 的 `QLineEdit` 默认就是 true（实测断言过）；
    //     ② 行为：对**每个**区域构造真 `QDropEvent` ⇒ **该区域对应的那个输入框被填上**；
    //     ③ 反馈：DragEnter 后区域带上 `drop_hover`（QSS 高亮它），Drop 后撤掉。
    {
        QTemporaryDir drop_tmp;
        const QList<QWidget*> page_widgets = {
            create_page(0), create_page(1), create_page(2),
            create_page(3), create_page(4), create_page(5)};
        const auto zone_ok = [](PageBase* page, QWidget* zone) {
            // 区域自己必须是拖放目标、必须带反馈属性，且**过滤器真的挂在它身上**
            if (page == nullptr || zone == nullptr) return false;
            if (!zone->acceptDrops()) return false;
            if (!zone->property("drop_hover").isValid()) return false;
            const std::vector<QWidget*>& zones = page->drop_zones().containers();
            const std::vector<FileDropFilter*>& filters = page->drop_zones().filters();
            if (zones.size() != filters.size()) return false;
            for (size_t i = 0; i < zones.size(); ++i) {
                if (zones[i] == zone) return filters[i] != nullptr && filters[i]->is_attached_to(zone);
            }
            return false;
        };

        // ① 每个目录/文件输入都要有一个能接住它的区域（"所有目录都需要拖拽"）。
        //    这份名单就是**要选目录/文件**的那些输入（搜索框/行内编辑不在其中）。
        const QStringList input_names = {
            QStringLiteral("convert_source"), QStringLiteral("convert_output"),
            QStringLiteral("text_source"),    QStringLiteral("text_target"),
            QStringLiteral("xml_dir_a"),      QStringLiteral("xml_dir_b"),
            QStringLiteral("video_source"),   QStringLiteral("video_reference"),
            QStringLiteral("video_output"),
            QStringLiteral("font_xml_dir"),   QStringLiteral("font_path"),
            QStringLiteral("font_out_dir"),   QStringLiteral("fs_output_dir"),
            QStringLiteral("fs_pack_out")};
        int edit_total = 0;
        QStringList not_droppable;
        for (QWidget* page : page_widgets) {
            auto* as_page = dynamic_cast<PageBase*>(page);
            if (as_page == nullptr) continue;
            for (QLineEdit* edit : page->findChildren<QLineEdit*>()) {
                if (!input_names.contains(edit->objectName())) continue;
                ++edit_total;
                QWidget* holder = edit;
                QWidget* covering_zone = nullptr;
                while (holder != nullptr && covering_zone == nullptr) {
                    if (as_page->drop_zones().is_zone(holder)) covering_zone = holder;
                    holder = holder->parentWidget();
                }
                // 区域必须**真的是**注册过的那个区域，且**过滤器真的挂在区域身上**
                //   （`is_attached_to` 判的是接线；区域里有没有输入框由 `containers()` 那一份
                //    登记表说得清 —— 所以这条不用遍历子控件，也就不会踩
                //    `QObject::findChild` 的形式要求）。
                // ⚠ 这里**不能**用 `edit->acceptDrops()`：Qt6 的 `QLineEdit` 默认就是 true
                //   （实测：裸 QLineEdit acceptDrops()=1、裸 QWidget=0）⇒ 拿它当锁等于"永远为真"。
                //   "输入框没把拖放吞掉"这件事由**② 的行为判据**证明：drop 事件直接把路径填进了
                //   这个 region 的输入框 ⇒ 说明事件真的到了区域这一层。
                const auto& zone_list = as_page->drop_zones().containers();
                const auto& filter_list = as_page->drop_zones().filters();
                bool zone_wired = false;
                for (size_t zi = 0; zi < zone_list.size() && zi < filter_list.size(); ++zi) {
                    if (zone_list[zi] != covering_zone) continue;
                    zone_wired = filter_list[zi] != nullptr &&
                                 filter_list[zi]->is_attached_to(covering_zone);
                }
                // 区域本身**不许是那个输入框**（挂在框上的旧做法就是这么绿过去的）
                if (covering_zone == edit || !zone_wired) {
                    not_droppable << QStringLiteral("%1(%2)").arg(as_page->page_id(), edit->objectName());
                }
            }
        }
        check(edit_total >= 14 && not_droppable.isEmpty(),
              QStringLiteral("① 每个目录/文件输入都在拖放区域里、且输入框自己不吞拖放"
                             "（输入 %1 个；不合格 %2）")
                  .arg(edit_total)
                  .arg(not_droppable.isEmpty() ? QStringLiteral("无")
                                               : not_droppable.join(QStringLiteral(" "))));

        // ②③ 逐页逐区域**构造真 Drop/DragEnter 事件** ⇒ 对应输入被填上 + 反馈属性到位
        QStringList drop_fail;
        int zones_checked = 0;
        for (int i = 0; i < page_widgets.size(); ++i) {
            auto* as_page = dynamic_cast<PageBase*>(page_widgets[i]);
            if (as_page == nullptr) {
                drop_fail << QStringLiteral("page%1-not-page-base").arg(i);
                continue;
            }
            const std::vector<QWidget*>& zones = as_page->drop_zones().containers();
            const std::vector<FileDropFilter*>& filters = as_page->drop_zones().filters();
            if (zones.size() < 2 || zones.size() != filters.size()) {
                drop_fail << QStringLiteral("%1-zones=%2").arg(as_page->page_id()).arg(zones.size());
                continue;
            }
            // 同页每个区域用**不同**的路径（否则"填上了"可能是上一个区域那次的残留）
            for (size_t zone_index = 0; zone_index < zones.size(); ++zone_index) {
                QWidget* zone = zones[zone_index];
                FileDropFilter* filter = filters[zone_index];
                const QString probe = QDir::toNativeSeparators(
                    drop_tmp.path() +
                    QStringLiteral("/drop_p%1_z%2/data.txt").arg(i).arg(static_cast<int>(zone_index)));
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(probe)});
                QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton,
                                      Qt::NoModifier);
                const bool accepted = filter != nullptr && filter->dispatch(&enter, zone);
                const bool highlighted = zone->property("drop_hover").toBool();
                QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                const bool dropped = filter != nullptr && filter->dispatch(&drop, zone);
                const bool cleared = !zone->property("drop_hover").toBool();
                ++zones_checked;

                // 区域里正好一个输入 ⇒ 它必须变成这次丢进来的路径（多输入的按顺序，只查第一个）
                const QList<QLineEdit*> edits = zone->findChildren<QLineEdit*>();
                const bool filled = edits.isEmpty() ||
                                    edits.front()->text() == QDir::toNativeSeparators(probe);
                if (!accepted || !dropped || !filled || !highlighted || !cleared ||
                    !zone_ok(as_page, zone)) {
                    drop_fail << QStringLiteral("%1-%2(a%3 d%4 f%5 h%6 c%7)")
                                     .arg(as_page->page_id())
                                     .arg(static_cast<int>(zone_index))
                                     .arg(accepted ? 1 : 0)
                                     .arg(dropped ? 1 : 0)
                                     .arg(filled ? 1 : 0)
                                     .arg(highlighted ? 1 : 0)
                                     .arg(cleared ? 1 : 0);
                }
            }
        }
        check(drop_fail.isEmpty() && zones_checked >= 13,
              QStringLiteral("② 拖到**相应区域** ⇒ 该区域的输入被填上、且区域看得见接住了"
                             "（验了 %1 个区域；不合格 %2）")
                  .arg(zones_checked)
                  .arg(drop_fail.isEmpty() ? QStringLiteral("无")
                                           : drop_fail.join(QStringLiteral(" "))));
        for (QWidget* page : page_widgets) delete page;
    }

    // ★★ 2026-10-04 用户第 2 条（原话）：「比如**解包的目标目录**，点击"选择"，**默认定向到工具
    //   所在的目录**。**改成定向到输入的目录**。**如果输入目录多元或没有输入，那才定向到本身
    //   所在的文件夹**。然后**其他定向保持现有逻辑**，即**继承上次的选择**。」
    //   判据：三条优先级各一条断言（决策不许被改回去），且"求默认起点"只有**一份共用函数**。
    {
        QTemporaryDir pick_tmp;
        const QString exe_dir = QDir::cleanPath(QCoreApplication::applicationDirPath());
        const QString tmp_a = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickA"));
        const QString tmp_b = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickB"));
        const QString tmp_file = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickA/movie.avi"));
        QDir().mkpath(tmp_a);
        QDir().mkpath(tmp_b);
        QFile(tmp_file).open(QIODevice::WriteOnly);

        reset_last_dialog_start();
        const QString unique_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList{tmp_a}));
        const QString none_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList()));
        const QString many_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList{tmp_a, tmp_b}));
        const QString inherited =
            resolve_dialog_start(dialog_start_for_input_dir(tmp_b, QStringList{tmp_a, tmp_b}));
        const QString from_file =
            resolve_dialog_start(dialog_start_for_input_file(QString(), QStringList{tmp_file}));
        check(QDir::cleanPath(unique_in) == tmp_a && QDir::cleanPath(none_in) == exe_dir &&
                  QDir::cleanPath(many_in) == exe_dir && QDir::cleanPath(inherited) == tmp_b &&
                  QDir::cleanPath(from_file) == tmp_a,
              QStringLiteral("② 共用起点函数：唯一输入→输入目录 / 空→exe 目录 / 多元→exe 目录 / "
                             "有值→继承 / 文件输入→它所在目录（实测 %1 | %2 | %3 | %4 | %5）")
                  .arg(unique_in, none_in, many_in, inherited, from_file));
        check(last_dialog_start_calls() == 5 &&
                  QDir::cleanPath(last_dialog_start_dir()) == tmp_a,
              QStringLiteral("② 起点只由这一份共用函数给（调用 %1 次；最后一次 %2）")
                  .arg(last_dialog_start_calls())
                  .arg(last_dialog_start_dir()));
    }

    // ★★ 2026-10-04 逐页钉住"选择"的起点（三条规则在**真实页面**上各来一遍）——
    //   页面把这些字段交给**共用函数**算（`resolve_dialog_start`），这里断言算出来的值。
    {
        QTemporaryDir start_tmp;
        const QString exe_dir = QDir::cleanPath(QCoreApplication::applicationDirPath());
        const QString src_dir = QDir::cleanPath(start_tmp.path() + QStringLiteral("/start_src"));
        const QString other = QDir::cleanPath(start_tmp.path() + QStringLiteral("/start_other"));
        const QString pkg_dir = QDir::cleanPath(start_tmp.path() + QStringLiteral("/startpkg"));
        QDir().mkpath(src_dir);
        QDir().mkpath(other);
        QDir().mkpath(pkg_dir);
        const QString pack_file = pkg_dir + QStringLiteral("/gamedata.db0");
        QFile(pack_file).open(QIODevice::WriteOnly);
        const auto start_of = [](const DialogStart& spec) {
            return QDir::cleanPath(resolve_dialog_start(spec));
        };

        // （1）编码转换：输出目录 ← 源路径
        {
            ConvertPage page;
            check(start_of(page.output_dialog_start()) == exe_dir,
                  QStringLiteral("② 编码转换：源为空 ⇒ 输出起点 = 工具目录"));
            page.source_edit()->setText(src_dir);
            check(start_of(page.output_dialog_start()) == src_dir,
                  QStringLiteral("② 编码转换：源唯一 ⇒ 输出起点 = 源目录"));
            page.output_edit()->setText(other);
            check(start_of(page.output_dialog_start()) == other,
                  QStringLiteral("② 编码转换：框里已有值 ⇒ **继承优先**"));
            check(start_of(page.source_dialog_start()) == src_dir,
                  QStringLiteral("② 编码转换：源本身没有输入目录 ⇒ 继承自己的值"));
        }
        // （2）文本提取：输出目录 ← 源目录
        {
            TextPage page;
            page.source_edit()->setText(src_dir);
            check(start_of(page.target_dialog_start()) == src_dir,
                  QStringLiteral("② 文本提取：源唯一 ⇒ 输出起点 = 源目录"));
            page.source_edit()->clear();
            check(start_of(page.target_dialog_start()) == exe_dir,
                  QStringLiteral("② 文本提取：源空 ⇒ 输出起点 = 工具目录"));
        }
        // （3）XML 校对：B ← A
        {
            XmlPage page;
            page.dir_a_edit()->setText(src_dir);
            check(start_of(page.dir_b_dialog_start()) == src_dir,
                  QStringLiteral("② XML 校对：A 唯一 ⇒ B 起点 = A"));
            page.dir_a_edit()->clear();
            check(start_of(page.dir_a_dialog_start()) == exe_dir,
                  QStringLiteral("② XML 校对：A 为空 ⇒ A 起点 = 工具目录"));
        }
        // （4）视频转换：输出目录 ← 源视频**所在目录**（文件语义）
        {
            VideoPage page;
            const QString movie = src_dir + QStringLiteral("/intro.avi");
            QFile(movie).open(QIODevice::WriteOnly);
            page.source_edit()->setText(movie);
            check(start_of(page.output_dialog_start()) == src_dir,
                  QStringLiteral("② 视频转换：源是**文件** ⇒ 输出起点 = 它所在目录"));
        }
        // （5）汉化包生成：字体/输出 ← 文本 XML 目录
        {
            FontPackPage page;
            // ★ 2026-10-06（D2 批，1.x `:129-149`）：字体框在**构造时**就填好了默认字体
            //   （上次用过的 → 系统雅黑 → 仓库根）⇒ 先测"继承上次的选择"这条优先级。
            const QString preset = page.font_edit()->text().trimmed();
            page.xml_edit()->setText(src_dir);
            check(preset.isEmpty() ||
                      start_of(page.font_dialog_start()) == QDir::cleanPath(preset),
                  QStringLiteral("② 汉化包生成：已填默认字体 ⇒ 字体起点 = 它自己（继承优先；"
                                 "实得「%1」，默认字体「%2」）")
                      .arg(start_of(page.font_dialog_start()), preset));
            // 再清掉字体框 ⇒ 回到"XML 唯一 ⇒ 起点 = XML 目录"这条（这才是原来那条判据）
            page.font_edit()->clear();
            check(start_of(page.font_dialog_start()) == src_dir &&
                      start_of(page.out_dialog_start()) == src_dir,
                  QStringLiteral("② 汉化包生成：XML 唯一 ⇒ 字体/输出起点 = XML 目录"));
            page.xml_edit()->clear();
            check(start_of(page.out_dialog_start()) == exe_dir,
                  QStringLiteral("② 汉化包生成：XML 空 ⇒ 输出起点 = 工具目录"));
        }
        // （6）文件系统：**用户点名的那个例子** —— 解包的目标目录 ← 来源包所在目录
        {
            FsPage page;
            page.add_source(pack_file, {{"a.txt", false}});
            check(start_of(page.unpack_output_dialog_start()) == pkg_dir,
                  QStringLiteral("② 文件系统：解包目标起点 = **来源包所在目录**（用户原话那条）"));
            reset_last_dialog_start();
            const QString first = start_of(page.unpack_output_dialog_start());
            check(last_dialog_start_calls() == 1 && last_dialog_start_dir() == first,
                  QStringLiteral("② 文件系统的起点也出自共用函数（%1）").arg(first));
            page.add_source(other + QStringLiteral("/second.db0"), {{"b.txt", false}});
            check(start_of(page.unpack_output_dialog_start()) == exe_dir,
                  QStringLiteral("② 文件系统：来源**多元** ⇒ 解包目标起点 = 工具目录"));
            page.out_edit()->setText(other);
            check(start_of(page.unpack_output_dialog_start()) == other,
                  QStringLiteral("② 文件系统：框里已有值 ⇒ 继承优先"));
        }
    }

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

        // ★ 用户 2026-09-30：「很多UI控件可以稍微硬朗一些，现在有点太圆润了。」
        //   圆角令牌本来就小（r2/r3）⇒ 圆润来自平台原生样式 + **这几类控件我们原本没写 QSS**。
        //   判据：标签栏/分组框/表头/菜单现在必须有规则（修前一条都没有 ⇒ 红）。
        //   （样式名探不出来：Fusion 被 QStyleSheetStyle 包住，objectName/name 都回 QStyleSheetStyle，
        //    除非引 Qt 私有头 ⇒ 改为钉住这些控件有 QSS，再靠既有的 QSS 警告必须为 0 那条守语法。）
        {
            const QString qss =
                xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
            const QStringList need{QStringLiteral("QTabBar::tab"),
                                   QStringLiteral("QGroupBox"),
                                   QStringLiteral("QHeaderView::section"),
                                   QStringLiteral("QToolTip")};
            QStringList missing;
            for (const QString& frag : need) {
                if (!qss.contains(frag)) missing << frag;
            }
            check(missing.isEmpty(),
                  QStringLiteral("硬朗化：原来没写 QSS 的控件现在有规则（缺 %1）")
                      .arg(missing.isEmpty() ? QStringLiteral("无")
                                             : missing.join(QStringLiteral(","))));
        }

        // ★ 用户 2026-09-30：「主题之外，再增加一个调整 UI 控件大小（包括里面的字体）的下拉」
        //   判据：缩放 1.25 ⇒ `ui_scale()==1.25`、字体点数变大、`px(100)==125`；再调回 1.0 ⇒ 复原。
        {
            const double font_before = QApplication::font().pointSizeF();
            const double scale_before = xrkit::core::ui_scale();
            xrkit::shell::apply_ui_size(*qApp, 1.25);
            const double font_after = QApplication::font().pointSizeF();
            check(std::abs(xrkit::core::ui_scale() - 1.25) < 0.001 && font_after > font_before &&
                      xrkit::core::px(100) == 125,
                  QStringLiteral("界面大小缩放：ui_scale %1→1.25、字体 %2→%3、px(100)=%4")
                      .arg(scale_before)
                      .arg(font_before)
                      .arg(font_after)
                      .arg(xrkit::core::px(100)));
            xrkit::shell::apply_ui_size(*qApp, 1.0);   // 复原，别影响后面的判据
            // ★ 用户 2026-09-30：「小有点偏小，默认有点偏大，大和特大就不用说了。」⇒ 档位必须是
            //   0.9 / 1.0 / 1.1 / 1.2（修前是 0.85/1.0/1.25/1.5 ⇒ 这条红）。
            {
                const auto& choices = xrkit::shell::ui_size_choices();
                const bool ladder_ok =
                    choices.size() == 4 && std::abs(choices[0].scale - 0.9) < 0.001 &&
                    std::abs(choices[1].scale - 1.0) < 0.001 &&
                    std::abs(choices[2].scale - 1.1) < 0.001 &&
                    std::abs(choices[3].scale - 1.2) < 0.001;
                check(ladder_ok,
                      QStringLiteral("界面大小档位＝0.9/1.0/1.1/1.2（实测 %1 档：%2/%3/%4/%5）")
                          .arg(choices.size())
                          .arg(choices.size() > 0 ? choices[0].scale : -1.0)
                          .arg(choices.size() > 1 ? choices[1].scale : -1.0)
                          .arg(choices.size() > 2 ? choices[2].scale : -1.0)
                          .arg(choices.size() > 3 ? choices[3].scale : -1.0));
            }
            // ★ 用户 2026-09-30：「包和内容物的字体大小太小。」⇒ 内容区（树/列表）字号必须由样式表
            //   单独放大，且走令牌（跟着界面大小一起缩）。修前没有任何 item view 的字号规则 ⇒ 红。
            {
                const QString qss = xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
                check(qss.contains(QStringLiteral("QTreeWidget, QTreeView, QListView { font-size")) ||
                          qss.contains(QStringLiteral("font-size: @{font_content}")),
                      QStringLiteral("内容区字号要单独放大（样式表里要有 font_content 规则）"));
            }
            check(std::abs(xrkit::core::ui_scale() - 1.0) < 0.001,
                  QStringLiteral("界面大小缩放：调回 1.0 要复原（实测 %1）")
                      .arg(xrkit::core::ui_scale()));
        }

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

        // ⑦-g ★★ 用户 2026-09-30 真机缺陷的**能红断言**（原话：「把日志调出来，光标放在小滚动条
        //      的窗口内滚轮就不是控制小滚动条而是控制大滚动条，把日志收起就正常。日志内也是正常。」）
        //      机制：树/文本框是 `QAbstractScrollArea` 而**不是** `ScrollViewport`，旧实现第一圈
        //      "找第一个能滚的视口"会把它们整圈跳过 ⇒ 直接命中**祖先页视口**并把它滚了；
        //      "日志一收起就正常"正是因为那时外层视口装得下（range==0），才轮到内层自己滚。
        //      判据用**公开决策入口**（`decide_wheel`，无副作用）—— 该滚谁的条子。
        //      判红办法：把 `decide_wheel` 改回"先只找 ScrollViewport、找不到再看原生滚动区"。
        {
            QWidget native_top;
            native_top.resize(400, 200);
            auto* native_outer = new ScrollViewport(&native_top);
            native_outer->setGeometry(0, 0, 400, 200);
            native_outer->set_fit(ScrollViewport::Fit::Content);
            auto* native_body = new QWidget;
            auto* native_lay = new QVBoxLayout(native_body);
            auto* native_ink = new QWidget;
            native_ink->setFixedHeight(900);
            native_lay->addWidget(native_ink);
            auto* inner_tree = new QTreeWidget(native_body);
            inner_tree->setObjectName(QStringLiteral("smoke_native_tree"));
            inner_tree->setColumnCount(1);
            for (int i = 0; i < 60; ++i) {
                inner_tree->addTopLevelItem(
                    new QTreeWidgetItem(QStringList{QStringLiteral("row %1").arg(i)}));
            }
            native_lay->addWidget(inner_tree);
            native_outer->set_body(native_body);
            native_outer->sync_layout();
            native_top.show();
            native_outer->show();
            QApplication::processEvents();

            QScrollBar* tree_bar = inner_tree->verticalScrollBar();
            check(native_outer->can_scroll_vertical() && tree_bar != nullptr &&
                      tree_bar->maximum() > tree_bar->minimum(),
                  QStringLiteral("⑦-g 前置：外层视口与内层树**都能滚**（树 max=%1）")
                      .arg(tree_bar != nullptr ? tree_bar->maximum() : -1));
            check(tree_bar != nullptr && ScrollViewport::bar_shown(tree_bar),
                  QStringLiteral("⑦-g 前置：内层树的条子看得见"));

            const ScrollViewport::WheelDecision tree_decision =
                ScrollViewport::decide_wheel(inner_tree->viewport(), -120, 0);
            check(tree_decision.bar == tree_bar && tree_decision.owner == inner_tree,
                  QStringLiteral("⑦-g 光标在树里 ⇒ 该滚的是**树自己的条子**（实得 owner=%1）")
                      .arg(tree_decision.owner == nullptr
                               ? QStringLiteral("nullptr")
                               : QString::fromLatin1(tree_decision.owner->metaObject()->className())));
            check(ScrollViewport::decide_wheel(native_body, -120, 0).bar == native_outer->vbar(),
                  QStringLiteral("⑦-g 光标在视口空白处 ⇒ 才轮到外层视口"));
            check(ScrollViewport::decide_wheel(native_ink, -120, 0).bar == native_outer->vbar(),
                  QStringLiteral("⑦-g 光标在**装得下**的普通控件上 ⇒ 外层视口（1.1 冒泡）"));

            // 真发一次：内层要真的动，外层**一步都不许动**（1.3 同一事件只一个条子动）
            const int outer_before = native_outer->offset_vertical();
            const int tree_before = tree_bar != nullptr ? tree_bar->value() : 0;
            check(send_wheel(inner_tree->viewport(), -120),
                  QStringLiteral("⑦-g 事件被内层消费（不再冒泡给外层）"));
            check(tree_bar != nullptr && tree_bar->value() > tree_before,
                  QStringLiteral("⑦-g 内层树自己滚了（%1 → %2，步进 %3）")
                      .arg(tree_before)
                      .arg(tree_bar != nullptr ? tree_bar->value() : -1)
                      .arg(tree_bar != nullptr ? tree_bar->singleStep() : -1));
            check(native_outer->offset_vertical() == outer_before,
                  QStringLiteral("⑦-g 外层视口一步没动（实测 %1，期望 %2）")
                      .arg(native_outer->offset_vertical())
                      .arg(outer_before));
        }
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

        // ★★ 2026-10-06（③）Hub 日志**落盘 + 导出**（1.x `stalker_toolkit.py:355-391`）——
        //   改动前 `导出` / `set_log_file` 在 hub.cpp 里**零命中** ⇒ GUI 日志一条都不落盘。
        {
            // 先把 `core::set_log_file` 的全局状态**清空**再建 Hub：
            // 这样"Hub 构造期挂了落盘文件"这条判据就是**在测 Hub 自己**（拆掉 ctor 里那行 ⇒ 红），
            // 而不是被上一个 Hub 顺手挂上的状态兜着。
            xrkit::core::set_log_file(std::string());
            Hub log_hub;
            check(!log_hub.log_file_path().isEmpty() && log_hub.log_file_active() &&
                      QFile::exists(log_hub.log_file_path()),
                  QStringLiteral("③ Hub 启动即挂上落盘日志（%1，active=%2）")
                      .arg(log_hub.log_file_path())
                      .arg(log_hub.log_file_active() ? 1 : 0));
            check(log_hub.log_export_button() != nullptr,
                  QStringLiteral("③ 日志面板里有「导出日志」按钮"));
            // 写一条**标记**：内存日志（日志栏）与落盘日志（core::log）两条通道都走
            const QString marker =
                QStringLiteral("SMOKE-MARKER-EXPORT-LOG-%1")
                    .arg(QDateTime::currentMSecsSinceEpoch());
            log_hub.log_line(marker);
            xrkit::core::log(xrkit::core::Level::Info, marker.toStdString());
            QString exported;
            QString why;
            const bool export_ok = log_hub.export_log(&exported, &why);
            check(export_ok && QFile::exists(exported),
                  QStringLiteral("③ 导出日志文件存在（%1；失败理由「%2」）").arg(exported, why));
            check(QFileInfo(exported).fileName().startsWith(QStringLiteral("log_")) &&
                      QFileInfo(exported).suffix() == QStringLiteral("txt"),
                  QStringLiteral("③ 导出文件名口径 = logs/log_YYYYmmdd_HHMMSS.txt（实得 %1）")
                      .arg(QFileInfo(exported).fileName()));
            QString body;
            {
                QFile file(exported);
                if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            }
            check(body.contains(marker) && body.contains(QStringLiteral("简略日志")) &&
                      body.contains(QStringLiteral("详细日志")),
                  QStringLiteral("③ 导出内容 = 内存日志 + 落盘日志**合并**（含刚写的标记；%1 字节）")
                      .arg(body.size()));
            // 写失败必须**明确报**：把落盘目录指到一个**文件**下面（mkpath 必失败）
            const QString blocker =
                QDir::temp().filePath(QStringLiteral("xrkit_smoke_log_blocker.txt"));
            {
                QFile file(blocker);
                if (file.open(QIODevice::WriteOnly)) file.write("x");
            }
            set_hub_log_dir(blocker + QStringLiteral("/logs"));
            QString bad_path;
            QString bad_why;
            const bool bad_ok = log_hub.export_log(&bad_path, &bad_why);
            check(!bad_ok && !bad_why.isEmpty() && !log_hub.last_export_error().isEmpty(),
                  QStringLiteral("③ 路径不可写 ⇒ 返回 false 且错误文本非空（实得「%1」）").arg(bad_why));
            set_hub_log_dir(smoke_log_dir);
            QFile::remove(blocker);
            QFile::remove(exported);
        }

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

        // ★★ 2026-10-06（①）取消：**真停 + 留痕 + 理由可见**
        //   事实（改动前）：`task_shell.cpp` 里 `cancel_->setEnabled(...)` 三处、
        //   **一处 `connect` 都没有** ⇒ 六页那颗「取消」是一颗**死按钮**。
        {
            // ⚠ 声明顺序：worker 线程按引用捕获下面这几个原子 ⇒ 它们必须**先于** TaskShell
            //   声明（TaskShell 的析构会 join 线程；后声明的先销毁 ⇒ 反了就是 use-after-free）。
            std::atomic<bool> worker_stopped{false};
            std::atomic<int> steps{0};
            TaskShell cancel_shell;
            cancel_shell.set_idle_status(QStringLiteral("就绪"));
            check(cancel_shell.run(
                      QStringLiteral("长任务"),
                      [&cancel_shell, &worker_stopped, &steps](const ProgressSink& progress,
                                                               const MessageSink& message) {
                          for (int i = 0; i < 400; ++i) {
                              if (cancel_shell.cancel_requested()) {
                                  message("已取消：在循环边界停下（已跑 " + std::to_string(i) +
                                          " 步）");
                                  worker_stopped = true;
                                  return;
                              }
                              steps = i;
                              std::this_thread::sleep_for(std::chrono::milliseconds(5));
                              progress(i * 100 / 400);
                          }
                      }),
                  QStringLiteral("① 取消：长任务起得来（13.1 闸门不变）"));
            QElapsedTimer cancel_clock;
            cancel_clock.start();
            while (steps.load() < 3 && cancel_clock.elapsed() < 5000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            check(cancel_shell.cancel_button() != nullptr &&
                      cancel_shell.cancel_button()->isEnabled(),
                  QStringLiteral("① 取消：跑起来时按钮可点（实得 enabled=%1）")
                      .arg(cancel_shell.cancel_button() != nullptr &&
                                   cancel_shell.cancel_button()->isEnabled()
                               ? 1
                               : 0));
            // 点那颗**以前是死的**按钮
            if (cancel_shell.cancel_button() != nullptr) cancel_shell.cancel_button()->click();
            check(cancel_shell.cancel_requested() && cancel_shell.cancel_requests() == 1,
                  QStringLiteral("① 取消：点按钮真的置上取消位（requests=%1）")
                      .arg(cancel_shell.cancel_requests()));
            cancel_clock.restart();
            while (cancel_shell.running() && cancel_clock.elapsed() < 5000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            const qint64 stop_ms = cancel_clock.elapsed();
            check(!cancel_shell.running() && worker_stopped.load() && stop_ms < 2000,
                  QStringLiteral("① 取消：点后 worker 在 %1 ms 内退出（真停；已跑 %2 步）")
                      .arg(stop_ms)
                      .arg(steps.load()));
            bool saw_cancel_event = false;
            for (const TaskShell::Event& e : cancel_shell.take_events()) {
                if (e.kind == 'c') saw_cancel_event = true;
            }
            check(saw_cancel_event, QStringLiteral("① 取消：事件流里留痕（'c'）"));
            check(!cancel_shell.cancel_reason().isEmpty() &&
                      cancel_shell.status().contains(QStringLiteral("已取消")) &&
                      cancel_shell.status() == cancel_shell.cancel_reason(),
                  QStringLiteral("① 取消：理由可见且**优先收尾**（状态栏＝「%1」）")
                      .arg(cancel_shell.status()));
            check(cancel_shell.cancel_button() != nullptr &&
                      !cancel_shell.cancel_button()->isEnabled() &&
                      cancel_shell.cancel_button()->text() == QStringLiteral("取消"),
                  QStringLiteral("① 取消：收尾后按钮复位（disabled + 文案回「取消」）"));
            result.notes.append(
                QStringLiteral("① 取消：点后 %1 ms 内真停（已跑 %2 步；理由＝%3）")
                    .arg(stop_ms)
                    .arg(steps.load())
                    .arg(cancel_shell.cancel_reason()));
            // 反向：**没点取消**的正常任务不许被这位置位
            std::atomic<int> ran{0};
            TaskShell plain_shell;
            plain_shell.run(QStringLiteral("跑完"), [&ran](const ProgressSink&, const MessageSink&) {
                ++ran;
            });
            for (int i = 0; i < 200 && plain_shell.running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            check(ran.load() == 1 && !plain_shell.cancel_requested() &&
                      plain_shell.cancel_requests() == 0,
                  QStringLiteral("① 取消：没点取消时取消位恒为假（反向锁）"));
        }
    }

    // ★★ 2026-10-06（②，用户拍板）**封包与字库的「取消」整块删除**
    //   用户原话：「**封包和字库的取消，在这里本质是完全没有的，故直接删除即可。**」
    //   口径：这两页**连按钮对象都不许存在**（藏起来/置灰/点不动的"取消"仍是假的取消）；
    //        其余三页（编码转换 / 文本提取 / XML 校对）共用任务壳的那颗取消**照旧保留**。
    //   反向锁的形态：删掉 `CancelButton::No` 的参数（＝按钮又回来了）⇒ 立刻红。
    {
        FsPage fs;
        FontPackPage fontpack;
        ConvertPage convert;
        TextPage text;
        XmlPage xml;
        check(fs.task() != nullptr && fs.task()->cancel_button() == nullptr,
              QStringLiteral("② 取消：文件系统页（封包/解包）不许有取消按钮（实得 %1）")
                  .arg(fs.task() != nullptr && fs.task()->cancel_button() != nullptr ? 1 : 0));
        check(fontpack.task() != nullptr && fontpack.task()->cancel_button() == nullptr,
              QStringLiteral("② 取消：汉化包生成页（字库）不许有取消按钮（实得 %1）")
                  .arg(fontpack.task() != nullptr && fontpack.task()->cancel_button() != nullptr
                           ? 1
                           : 0));
        check(convert.task() != nullptr && convert.task()->cancel_button() != nullptr &&
                  convert.task()->cancel_button()->text() == QStringLiteral("取消"),
              QStringLiteral("② 取消：编码转换页的取消**保留**（共用任务壳那颗）"));
        check(text.task() != nullptr && text.task()->cancel_button() != nullptr,
              QStringLiteral("② 取消：文本提取页的取消**保留**"));
        check(xml.task() != nullptr && xml.task()->cancel_button() != nullptr,
              QStringLiteral("② 取消：XML 校对页的取消**保留**"));
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
        // ★★ 2026-10-06（④）：1.x `fs_app.py:154` 的默认值就是 "auto"、`:193` 的
        //   `fmt_keys` 第一位也是 "auto" ⇒ 2.x 补上它，且**必须在第一项**。
        check(combo != nullptr && combo->itemData(0).toString() == QStringLiteral("auto"),
              QStringLiteral("6.13 auto 是格式下拉第一项（实得 %1）")
                  .arg(combo != nullptr ? combo->itemData(0).toString() : QStringLiteral("<无下拉>")));
        check(efs::format_key_is_auto("auto") && !efs::format_key_to_db("auto").has_value() &&
                  !efs::format_key_is_sqfs("auto"),
              QStringLiteral("6.13 auto ＝**不给格式**（既不是 DB 六格式、也不是 SquashFS）"));
        // ④ 锁②：选 auto ⇒ 交给引擎的 fallback 就是 `nullopt`（与"不传格式"同一条路）
        check(!efs::format_key_to_db(combo->currentData().toString().toStdString()).has_value(),
              QStringLiteral("6.13 选 auto ⇒ 不传格式（fallback=nullopt）"));
        // ② 说明标签：非空、选 auto 时含"自动检测"、换成具体格式后**跟着变**
        {
            const QString auto_hint = page.format_hint_text();
            check(page.format_hint() != nullptr && !auto_hint.isEmpty() &&
                      auto_hint.contains(QStringLiteral("自动检测")),
                  QStringLiteral("6.13 选 auto ⇒ 说明标签非空且说明是「自动检测」（实得 %1）")
                      .arg(auto_hint));
            QString other_hint;
            if (combo != nullptr && combo->count() > 1) {
                combo->setCurrentIndex(1);
                other_hint = page.format_hint_text();
                combo->setCurrentIndex(0);
            }
            check(!other_hint.isEmpty() && other_hint != auto_hint &&
                      page.format_hint() != nullptr &&
                      page.format_hint()->text() == auto_hint,
                  QStringLiteral("6.13 说明标签随所选格式变化（auto=「%1」/ 具体=「%2」）")
                      .arg(auto_hint, other_hint));
        }
        // ④ 封包下拉**不含 auto**（1.x `fs_app.py:196` `[k for k in fmt_keys if k != "auto"]`）
        {
            int auto_items = 0;
            QComboBox* pack_combo = page.pack_format_combo();
            for (int i = 0; pack_combo != nullptr && i < pack_combo->count(); ++i) {
                if (efs::format_key_is_auto(pack_combo->itemData(i).toString().toStdString())) {
                    ++auto_items;
                }
            }
            check(pack_combo != nullptr && auto_items == 0 &&
                      pack_combo->count() == static_cast<int>(formats.size()) - 1,
                  QStringLiteral("6.13 封包下拉 = 非 auto 的全部格式（auto 项 %1、条数 %2 / 期望 %3）")
                      .arg(auto_items)
                      .arg(pack_combo != nullptr ? pack_combo->count() : -1)
                      .arg(static_cast<int>(formats.size()) - 1));
        }
        int key_bad = 0;
        int sqfs_items = 0;
        for (int i = 0; combo != nullptr && i < combo->count(); ++i) {
            const std::string key = combo->itemData(i).toString().toStdString();
            const std::string label = combo->itemText(i).toStdString();
            // ★ 2026-10-06（④）：`auto` 也是一个**引擎认得的内部键**（它＝不给格式），
            //   所以"取值合法性"这条判据要把它算进来（`format_key_to_db` 仍不认它 —— 见上）。
            const bool key_ok = efs::format_key_is_sqfs(key) ||
                                efs::format_key_to_db(key).has_value() ||
                                efs::format_key_is_auto(key);
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

        // ★★ 2026-10-01 用户第 6 条（原话）：「包列表和文件列表引用的子组件是不是不一样？…
        //   所以，使用同一个小组件来完成。包列表的内容和文件列表的结构实际上是一样的。
        //   统一使用文件列表的那个，因为那个已经打磨过。」
        //   可判红的形式：两者**同一套关键配置**（原来包列表是 `QListWidget`，这几条天然对不上）。
        {
            // 对齐的是**组件模板**那一组属性：列数 / ExtendedSelection / uniformRowHeights /
            // alternatingRowColors / 表头策略（三列 Interactive + 同一个 minimumSectionSize）。
            // **`acceptDrops` 不算"对齐项"**：它是**实例级特性**（包列表是拖放目标，`drop_filter_`
            // 只挂在它身上；条目树就算设了也没有 handler ⇒ 拖上去没反应，反而误导）——
            // 所以这里只单独要求包列表可接收拖放（6.4 的要求），不拿它跟条目树比。
            // ★ 2026-10-02：口径**有意放宽一处**（用户圈着表头打了个「？」之后）。
            //   旧写法要求"两列表三列都 Interactive、拉伸交给条目树末列"，而**包列表要的是"名称列吃
            //   剩余宽度、大小/✕ 靠右钉住"**（否则表头右边 ~400px 是死区 —— 正是用户圈的那块）。
            //   ⇒ 对齐改成"**同一套表头行为**：恰好一列吃剩余宽度（哪一列由各表自己的语义定）＋
            //   其余列一律 Interactive（用户 09-29 要的可拖不许丢）＋ minimumSectionSize 相同"。
            //   放宽的只是"哪一列拉伸"；"同一个组件、同一族行为"没有放宽。
            const auto header_stretch_columns = [](QTreeWidget* tree) {
                if (tree == nullptr || tree->header() == nullptr) return -1;
                int stretched = 0;
                for (int col = 0; col < tree->columnCount(); ++col) {
                    const auto mode = tree->header()->sectionResizeMode(col);
                    if (mode == QHeaderView::Stretch) {
                        ++stretched;
                    } else if (mode != QHeaderView::Interactive) {
                        return -1;   // 出现 Fixed/ResizeToContents 之类 ⇒ 不算同一族
                    }
                }
                if (tree->header()->stretchLastSection()) ++stretched;
                return stretched;
            };
            // ★★「？」的硬判据：**列的总宽必须铺满视口**，否则右半边就是死区（改回去立刻红：
            //   真机 318 < 717，包列表这一条会红，而条目树那边仍然是铺满的 —— 差异看得见）。
            const auto header_fills_viewport = [](QTreeWidget* tree) {
                if (tree == nullptr || tree->header() == nullptr) return false;
                int total = 0;
                for (int col = 0; col < tree->columnCount(); ++col) {
                    total += tree->header()->sectionSize(col);
                }
                return tree->viewport()->width() > 0 &&
                       total >= tree->viewport()->width() - xrkit::core::px(2);
            };
            check(qobject_cast<QTreeWidget*>(page.package_list()) != nullptr &&
                      page.package_list()->columnCount() == 3 &&
                      page.package_list()->selectionMode() ==
                          page.entry_tree()->selectionMode() &&
                      page.package_list()->selectionMode() ==
                          QAbstractItemView::ExtendedSelection &&
                      page.package_list()->uniformRowHeights() ==
                          page.entry_tree()->uniformRowHeights() &&
                      page.package_list()->uniformRowHeights() &&
                      page.package_list()->alternatingRowColors() ==
                          page.entry_tree()->alternatingRowColors() &&
                      header_stretch_columns(page.package_list()) == 1 &&
                      header_stretch_columns(page.entry_tree()) == 1 &&
                      // ★★ 2026-10-02 用户：「为什么包树和文件树的形式不同。文件树的形式已经相当
                      //   成熟，包树直接引用树的统一函数就行啊。」⇒ 装配已收敛成同一个
                      //   `apply_pair_tree_spec`，这里把它**对外可测的那几项**钉住：
                      //   · 两树的"展开语义"必须一致：**单击展开、双击不展开**；
                      //   · 两树都**要有根装饰**（包列表在平表时代留了 `setRootIsDecorated(false)`，
                      //     树化 + 默认收起之后**顶层节点的孩子就再也点不出来了**）——
                      //     把这条去掉立刻红。
                      page.package_list()->expandsOnDoubleClick() == false &&
                      page.entry_tree()->expandsOnDoubleClick() == false &&
                      page.package_list()->rootIsDecorated() &&
                      page.entry_tree()->rootIsDecorated() &&
                      page.package_list()->header()->minimumSectionSize() ==
                          page.entry_tree()->header()->minimumSectionSize() &&
                      page.package_list()->acceptDrops(),
                  QStringLiteral("第6条 包列表与条目树同一套小组件配置（列 %1 / selection %2==%3 / "
                                 "uniform %4==%5 / alt %6==%7 / 表头恰好一列拉伸 %8==%9 / "
                                 "minSection %10==%11 / 包列表可接收拖放 %12）")
                      .arg(page.package_list()->columnCount())
                      .arg(static_cast<int>(page.package_list()->selectionMode()))
                      .arg(static_cast<int>(page.entry_tree()->selectionMode()))
                      .arg(page.package_list()->uniformRowHeights() ? 1 : 0)
                      .arg(page.entry_tree()->uniformRowHeights() ? 1 : 0)
                      .arg(page.package_list()->alternatingRowColors() ? 1 : 0)
                      .arg(page.entry_tree()->alternatingRowColors() ? 1 : 0)
                      .arg(header_stretch_columns(page.package_list()))
                      .arg(header_stretch_columns(page.entry_tree()))
                      .arg(page.package_list()->header()->minimumSectionSize())
                      .arg(page.entry_tree()->header()->minimumSectionSize())
                      .arg(page.package_list()->acceptDrops() ? 1 : 0));
            QApplication::processEvents();
            // ★★ 2026-10-02 用户「？」：**表头/行必须铺满面板**（三列总宽 < 视口宽 ⇒ 右边是死区）。
            check(header_fills_viewport(page.package_list()) &&
                      header_fills_viewport(page.entry_tree()),
                  QStringLiteral("第6条 表头铺满面板无死区（包列表列宽 %1 / 视口 %2，条目树 %3 / 视口 %4）")
                      .arg([&] {
                          int t = 0;
                          for (int c = 0; c < page.package_list()->columnCount(); ++c) {
                              t += page.package_list()->header()->sectionSize(c);
                          }
                          return t;
                      }())
                      .arg(page.package_list()->viewport()->width())
                      .arg([&] {
                          int t = 0;
                          for (int c = 0; c < page.entry_tree()->columnCount(); ++c) {
                              t += page.entry_tree()->header()->sectionSize(c);
                          }
                          return t;
                      }())
                      .arg(page.entry_tree()->viewport()->width()));
        }

        // ★★ 2026-10-01 用户第 3 条（原话）：「并且扫描时有Bug的，选择包模式的时候，是扫不出
        //   金球奖的db的。这是个Bug。不要想着增加后缀的样式（.db0，.db_base等），因为这是
        //   遍历不完的。然后点击"所有文件"才能看见那些包。」
        //   判据：过滤器里必须有 `*.db*` / `*.sq*` 这两段通配，且**不许**再枚举具体后缀
        //   （枚举是"扫不出"的根因：`gamedata.db_textures_1` 不在任何一张名单里）。
        {
            const QString filter = FsPage::package_dialog_filter();
            // ★★ 2026-10-04 补 `*.xdb*`：用户问「`.xdb`，我们的工具能识别吗？」⇒ 旧过滤器要求字面 `.db`
            //   ⇒ "选包"对话框里看不见 `.xdb`。**删掉这条断言立刻红**。
            check(filter.contains(QStringLiteral("*.db*")) &&
                      filter.contains(QStringLiteral("*.sq*")) &&
                      filter.contains(QStringLiteral("*.xdb*")) &&
                      !filter.contains(QStringLiteral("*.db0")) &&
                      !filter.contains(QStringLiteral("*.db_base")),
                  QStringLiteral("第3条 加包过滤器=「%1」（含 *.db* / *.sq* / ★*.xdb*，不枚举具体后缀）")
                      .arg(filter));
        }

        // ★ 2026-09-30 结构重排（用户第四轮 ①「你不认为，堆起来很丑吗？」+ 授权「按自己审美来改」）
        //   判据＝**工具行归位**：工具行与它服务的那个列表**同一父控件**、且行在列表**上面**；
        //   两个面板一起进同一个分隔条。**修前第一条就红**：条目工具行（搜索/全选）的父控件是
        //   解包页、而且落在分隔条**下面**（看着像页级控件）。
        {
            // ★★ ①-a0 **空状态**（一个包都没有）的排版 —— 2026-09-30 用户对着 FS 页发火的真根因：
            //   条目树在空状态被 `setVisible(false)`，而 **Qt 布局不给隐藏控件分配空间** ⇒
            //   那 340 px 全落进 `entry_bar`（普通 QWidget、垂直策略默认 Preferred）⇒ 里面的按钮被
            //   QHBoxLayout **垂直居中** ⇒ 看上去就是"搜索行孤零零飘在两块空白中间"。
            //   修法：工具行钉成 `QSizePolicy::Fixed` + 条目树**永远可见**（空树带表头本身就是
            //   "这里会出现包内条目"的指示，不是解释文字 ⇒ 不违反"界面不要解释段"）。
            //   判红办法：把这两处改回去 ⇒ 下面第一条立刻红（实测会从 42 变成 386）。
            FsPage empty_page;
            empty_page.resize(900, 700);
            empty_page.show();
            QApplication::processEvents();
            check(empty_page.entry_pane() != nullptr && empty_page.entry_pane()->isHidden() &&
                      empty_page.package_list() != nullptr &&
                      empty_page.package_list()->height() > xrkit::core::px(300),
                  QStringLiteral("① 空状态＝条目面板整块收起、包列表填满工作区"
                                 "（包列表实测 %1 > %2）")
                      .arg(empty_page.package_list() != nullptr
                               ? empty_page.package_list()->height()
                               : -1)
                      .arg(xrkit::core::px(300)));
            check(empty_page.entry_tree() != nullptr && empty_page.entry_pane() != nullptr &&
                      empty_page.entry_pane()->isHidden(),
                  QStringLiteral("① 空状态下**整个条目面板收掉**（解包页空状态＝两行 + 一个列表填满，"
                                 "与封包页同形）"));
            // ①-a1 操作栏按钮**贴右**（`add_action` 里 stretch 在 index 0 ⇒ 必须追加，不许前插）
            if (empty_page.action_bar() != nullptr && empty_page.unpack_all_button() != nullptr) {
                QPushButton* last = empty_page.unpack_all_button();
                const int right_gap =
                    empty_page.action_bar()->width() - (last->x() + last->width());
                check(right_gap >= 0 && right_gap <= xrkit::core::px(12),
                      QStringLiteral("① 操作栏按钮贴右（右侧留白 %1 ≤ %2；修前按钮被顶到左下角）")
                          .arg(right_gap)
                          .arg(xrkit::core::px(12)));
            }
            empty_page.hide();

            // ★★ 2026-10-01 用户三连（都在这一个夹具里判）：
            //   ①「在筛选的时候，文件夹下会多出一个空的行」⇒ 真因＝`make_entry_item` 给"有子节点的
            //     目录"塞的**懒建占位子项**没被摘掉，筛选是一次性建树 ⇒ 它留在第一位当空行。
            //   ②「我不知道搜索字段和结果有什么直接关系」⇒ 旧实现拿**整条路径** contains，
            //     `ai` 会命中 `crouch_down_aim.anm` 这种"凑巧有这两个字母"的；现改成**按名字匹配**
            //     （文件与目录一视同仁）+ 命中项加粗。
            //   ③「DB栏能拖动多选，文件栏不行」⇒ 条目树补 ExtendedSelection。
            {
                FsPage filter_page;
                filter_page.add_source(QStringLiteral("smoke_filter"),
                                       {efs::EntryInfo{std::string("dir"), true, 0},
                                        efs::EntryInfo{std::string("dir/aim.txt"), false, 10},
                                        efs::EntryInfo{std::string("dir/other.bin"), false, 20}});
                filter_page.search_edit()->setText(QStringLiteral("aim"));
                QApplication::processEvents();
                QTreeWidget* tree = filter_page.entry_tree();
                int blanks = 0;
                int rows = 0;
                bool has_other = false;
                bool matched_bold = false;
                std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
                    ++rows;
                    if (item->text(0).isEmpty()) ++blanks;
                    if (item->text(0).contains(QStringLiteral("other"))) has_other = true;
                    if (item->text(0) == QStringLiteral("aim.txt") && item->font(0).bold()) {
                        matched_bold = true;
                    }
                    for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
                };
                for (int i = 0; i < tree->topLevelItemCount(); ++i) walk(tree->topLevelItem(i));
                check(blanks == 0 && rows > 0,
                      QStringLiteral("① 筛选后树里没有空名字的行（空行 %1 行 / 共 %2 行）")
                          .arg(blanks)
                          .arg(rows));
                check(!has_other,
                      QStringLiteral("② 搜索按**名字**匹配：不命中 aim 的 other.bin 不许出现"));
                check(matched_bold, QStringLiteral("② 命中项加粗（一眼看出哪些行是命中的）"));
                check(filter_page.entry_tree()->selectionMode() == QAbstractItemView::ExtendedSelection,
                      QStringLiteral("③ 条目树能框选/拖选多个（ExtendedSelection）"));
                filter_page.hide();
            }

            // ★ 几何必须在**装了东西**的状态下量：空夹具的树是隐藏的（历史现象），不适合量几何。
            //   所以这里用一个**有内容**的独立夹具，不动上面的 `page`。
            FsPage geo_page;
            geo_page.add_source(QStringLiteral("smoke_src"),
                                {efs::EntryInfo{std::string("dir"), true, 0},
                                 efs::EntryInfo{std::string("dir/a.txt"), false, 10}});
            geo_page.show();

            QWidget* pkg_pane = geo_page.package_pane();
            QWidget* entry_pane = geo_page.entry_pane();
            QWidget* pkg_bar = geo_page.package_bar();
            QWidget* entry_bar = geo_page.entry_bar();
            check(pkg_pane != nullptr && entry_pane != nullptr &&
                      geo_page.package_list() != nullptr && geo_page.entry_tree() != nullptr &&
                      geo_page.package_list()->parentWidget() == pkg_pane &&
                      geo_page.entry_tree()->parentWidget() == entry_pane,
                  QStringLiteral("① 两个列表各自在自己的面板里"));
            check(pkg_bar != nullptr && entry_bar != nullptr &&
                      pkg_bar->parentWidget() == pkg_pane &&
                      entry_bar->parentWidget() == entry_pane &&
                      geo_page.add_button() != nullptr &&
                      geo_page.add_button()->parentWidget() == pkg_bar,
                  QStringLiteral("① 工具行归位：「加包」回到包工具行、条目工具行归条目面板"));
            check(geo_page.split_list_tree() != nullptr &&
                      geo_page.split_list_tree()->count() == 2 &&
                      geo_page.split_list_tree()->widget(0) == pkg_pane &&                      geo_page.split_list_tree()->widget(1) == entry_pane,
                  QStringLiteral("① 分隔条拖的是两个**面板**（工具行跟着自己的列表走）"));

            // ① 几何取两档：**正常窗口**（900×700，用户实际就在这附近）与**最小页高**
            //   `page_min_h()`。第一档要求"工具行真在列表上方 + 两个面板都够高"；第二档只要求
            //   "都还在、没归零"—— 430 px 里要塞 行+两个面板+任务面板，两个列表各自的
            //   minimumHeight(120/160) 本来就装不下（这是**尺寸下限**，不是这次重排引入的）。
            check(entry_bar != nullptr && entry_pane != nullptr && !entry_pane->isHidden() &&
                      entry_bar->height() > 0 && entry_bar->height() <= xrkit::core::px(60),
                  QStringLiteral("① 有内容时条目工具行只占一行（实测 %1 ≤ %2；被拉满就是"
                                 "\"隐藏控件的空间落进兄弟控件\"那个坑）")
                      .arg(entry_bar != nullptr ? entry_bar->height() : -1)
                      .arg(xrkit::core::px(60)));

            const auto measure = [&](int w, int h) {
                geo_page.resize(w, h);
                if (geo_page.layout() != nullptr) geo_page.layout()->activate();
                QApplication::processEvents();
            };

            measure(900, 700);
            const int big_pkg = pkg_pane != nullptr ? pkg_pane->height() : 0;
            const int big_entry = entry_pane != nullptr ? entry_pane->height() : 0;
            const int bar_y = entry_bar != nullptr ? entry_bar->y() : -1;
            const int tree_y = geo_page.entry_tree() != nullptr ? geo_page.entry_tree()->y() : -1;
            check(bar_y >= 0 && tree_y > bar_y,
                  QStringLiteral("① 正常窗口下条目工具行在树**上面**（行 y=%1 < 树 y=%2）")
                      .arg(bar_y)
                      .arg(tree_y));
            check(big_pkg >= xrkit::core::px(120) && big_entry >= xrkit::core::px(160),
                  QStringLiteral("① 正常窗口 900×700 下两个面板都够高（包 %1 ≥ %2；条目 %3 ≥ %4）")
                      .arg(big_pkg)
                      .arg(xrkit::core::px(120))
                      .arg(big_entry)
                      .arg(xrkit::core::px(160)));

            measure(720, page_min_h());
            const int small_pkg = pkg_pane != nullptr ? pkg_pane->height() : 0;
            const int small_entry = entry_pane != nullptr ? entry_pane->height() : 0;
            check(small_pkg > xrkit::core::px(40) && small_entry > xrkit::core::px(40),
                  QStringLiteral("① 最小页高 %1 下两个面板都没被挤成一条（包 %2 / 条目 %3）")
                      .arg(page_min_h())
                      .arg(small_pkg)
                      .arg(small_entry));
            result.notes.append(
                QStringLiteral("① 结构重排几何：900×700 ⇒ 包 %1 / 条目 %2（工具行 y=%3 < 树 y=%4）；"
                               "720×%5 ⇒ 包 %6 / 条目 %7")
                    .arg(big_pkg)
                    .arg(big_entry)
                    .arg(bar_y)
                    .arg(tree_y)
                    .arg(page_min_h())
                    .arg(small_pkg)
                    .arg(small_entry));
        }

        // ★ 2026-09-30 用户第四轮 ⑦（「现在还是慢得不行。最起码，要追上 X-Ray Engine 这个优化
        //   奇烂的老东西吧。」）：DB 列条目改 **seek 式**（只读块头 + 尾部条目表）。
        //   旧实现整文件读进内存 —— 实测 1900 MB 的包光读盘 8,063 ms（只与文件大小成正比）。
        //   判据三条，全部落在**真实字节**上：
        //     ① 撑到 32 MB 的包，`list_db_seek` 必须走 seek 且实际读进来的字节 ≪ 文件大小；
        //     ② 条目表与内存版 `unpack_db` **逐字段一致**（路径/offset/两个尺寸/is_dir/use_lzhuf）；
        //     ③ 单条目 `extract_entry_seek` 与内存版 `extract_entry` **逐字节一致**。
        //   判红办法：把 `list_db_seek` 的 seek 分支短路掉（直接走整文件读）⇒ ① 立刻红。
        {
            namespace efs = xrkit::engine::fs;
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dbseek"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);

            const auto rd32 = [](const std::vector<uint8_t>& v, size_t p) {
                return static_cast<uint32_t>(v[p]) | (static_cast<uint32_t>(v[p + 1]) << 8) |
                       (static_cast<uint32_t>(v[p + 2]) << 16) |
                       (static_cast<uint32_t>(v[p + 3]) << 24);
            };
            std::vector<efs::DbPackItem> items;
            items.push_back({std::string("a.txt"), std::vector<uint8_t>(64, 'a'), false});
            items.push_back({std::string("dir"), {}, true});
            items.push_back({std::string("dir/b.bin"), std::vector<uint8_t>(4096, 7), false});
            const efs::DbFormat fmt = *efs::db_format_from_any("xdb");
            const std::vector<uint8_t> packed = efs::pack_db(items, fmt);
            const uint32_t old_data_sz = rd32(packed, 4);
            const size_t tail = 8 + static_cast<size_t>(old_data_sz);   // 尾部 HEADER 块的起点

            // 把 DATA 块撑到 32 MB（真档案里 DATA 就是几百 MB～几 GB），尾部那一块原样搬过去
            const uint32_t big_sz = 32u * 1024u * 1024u;
            std::vector<uint8_t> big;
            big.reserve(8 + static_cast<size_t>(big_sz) + (packed.size() - tail));
            big.push_back(0);
            big.push_back(0);
            big.push_back(0);
            big.push_back(0);
            big.push_back(static_cast<uint8_t>(big_sz & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 8) & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 16) & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 24) & 0xFF));
            big.resize(8 + static_cast<size_t>(big_sz), 0);
            big.insert(big.end(), packed.begin() + static_cast<long>(tail), packed.end());

            const QString big_path = dir + QStringLiteral("/big.db");
            {
                QFile file(big_path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(reinterpret_cast<const char*>(big.data()),
                               static_cast<qint64>(big.size()));
                    file.close();
                }
            }

            const efs::DbList mem = efs::unpack_db(big, fmt);
            const efs::DbListing seek = efs::list_db_seek(big_path.toStdString(), fmt);
            check(seek.ok && seek.used_seek && seek.file_size == big.size() &&
                      seek.header_bytes > 0 &&
                      seek.header_bytes < static_cast<uint64_t>(big.size()) / 100,
                  QStringLiteral("⑦' DB seek 式：32 MB 的包只读了 %1 B / 共 %2 B（used_seek=%3）")
                      .arg(seek.header_bytes)
                      .arg(seek.file_size)
                      .arg(seek.used_seek ? 1 : 0));

            const auto entry_row = [](const efs::DbEntry& e) {
                return QStringLiteral("%1|%2|%3|%4|%5|%6")
                    .arg(e.is_dir ? 1 : 0)
                    .arg(QString::fromStdString(e.path))
                    .arg(static_cast<qulonglong>(e.offset))
                    .arg(static_cast<qulonglong>(e.size_real))
                    .arg(static_cast<qulonglong>(e.size_comp))
                    .arg(e.use_lzhuf ? 1 : 0);
            };
            QStringList mem_rows;
            for (const efs::DbEntry& e : mem.entries) mem_rows << entry_row(e);
            QStringList seek_rows;
            for (const efs::DbEntry& e : seek.entries) seek_rows << entry_row(e);
            check(!seek_rows.isEmpty() && seek_rows == mem_rows,
                  QStringLiteral("⑦' seek 版条目表与内存版**逐字段一致**（%1 vs %2 条）")
                      .arg(seek_rows.size())
                      .arg(mem_rows.size()));

            int extract_bad = 0;
            for (const efs::DbEntry& e : seek.entries) {
                if (e.is_dir) continue;
                const efs::DbExtract a = efs::extract_entry(big, e);
                const efs::DbExtract b = efs::extract_entry_seek(big_path.toStdString(), e);
                if (a.ok != b.ok || a.data != b.data) ++extract_bad;
            }
            check(extract_bad == 0,
                  QStringLiteral("⑦' 单条目 seek 提取与内存版逐字节一致（%1 条不符 / %2 条条目）")
                      .arg(extract_bad)
                      .arg(seek.entries.size()));
            result.notes.append(
                QStringLiteral("⑦' DB seek：32 MB 包读取 %1 B（占 %2%%），条目 %3 条")
                    .arg(seek.header_bytes)
                    .arg(seek.file_size > 0 ? static_cast<int>(seek.header_bytes * 100 /
                                                               seek.file_size)
                                            : -1)
                    .arg(seek.entries.size()));
            QDir(dir).removeRecursively();
        }

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
        // ★★ 2026-10-04 用户问「`.xdb`，我们的工具能识别吗？」⇒ 实测**不能**（旧规则只取"点后两位"＝`xd`）
        //   ⇒ 漏了 `.xdb` 这个家族（真机依据：`sop_standalone.7z` 里的 `patch_00.xdb` 就是 `format=xdb`）。
        //   这条锁**同时钉三个家族**：把 `.xdb` 分支删掉/改回两位比较，立刻红。
        check(efs2::is_package_file_name("x.db0") && efs2::is_package_file_name("x.db_base") &&
                  efs2::is_package_file_name("x.sqfs") && efs2::is_package_file_name("x.sq_textures") &&
                  efs2::is_package_file_name("x.xdb") && efs2::is_package_file_name("x.xdb_base") &&
                  efs2::is_package_file_name("patch_00.xdb"),
              QStringLiteral("1.x 包名规则：.db*/​.sq*/★.xdb* 都算包（引擎只看 basename 最后一个扩展）"));
        check(!efs2::is_package_file_name("x.txt") && !efs2::is_package_file_name("xdbc") &&
                  !efs2::is_package_file_name("x.db.bak2") && !efs2::is_package_file_name("x.xdoc") &&
                  !efs2::is_package_file_name("x") && !efs2::is_package_file_name("x."),
              QStringLiteral("1.x 包名规则：非包文件不许误认（x.txt / xdbc / x.db.bak2 / ★x.xdoc / x / x.）"));

        // ★★ 同一条缺陷的**真实症状锁**：一个含 `.xdb` 的目录走**用户真实入口** `add_paths()`（拖入/加文件夹
        //   模式都是它；`scan_dir()` 只负责收集、加载在调用方）⇒ 必须**收进来并加载**（源 1 / 已加载 1）。
        //   旧规则下 `.xdb` 被判为非包 ⇒ **静默忽略**（源 0）⇒ 改回旧规则立刻红。
        {
            const QString xdir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xdb"));
            QDir(xdir).removeRecursively();
            QDir().mkpath(xdir);
            std::vector<efs2::DbPackItem> xitems;
            efs2::DbPackItem xone;
            xone.path = "t/a.txt";
            xone.data = {'x'};
            xitems.push_back(xone);
            const std::vector<uint8_t> xraw = efs2::pack_db(xitems, efs2::DbFormat::Xdb);
            std::ofstream xout((xdir + QStringLiteral("/patch_00.xdb")).toStdString(),
                               std::ios::binary | std::ios::trunc);
            xout.write(reinterpret_cast<const char*>(xraw.data()),
                       static_cast<std::streamsize>(xraw.size()));
            xout.close();
            FsPage xdb_page;
            xdb_page.resize(720, page_min_h());
            xdb_page.show();
            QApplication::processEvents();
            xdb_page.add_paths(QStringList{xdir});
            QApplication::processEvents();
            check(xdb_page.source_count() == 1 && xdb_page.loaded_source_count() == 1,
                  QStringLiteral("★.xdb 也是包：含 patch_00.xdb 的目录走 add_paths 要被收进来并加载"
                                 "（源 %1 期望 1 / 已加载 %2 期望 1）")
                      .arg(xdb_page.source_count())
                      .arg(xdb_page.loaded_source_count()));
            xdb_page.hide();
            QDir(xdir).removeRecursively();
        }

        // ★★ 2026-10-04 用户拍的**架构红线**（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI
        //   的细节功能比 CLI 还多的。**」⇒ 解包**落点规则**从 FS 页搬进**引擎**
        //   （`efs::package_folder_name` / `unpack_target_dir` / `plan_unpack_targets`），
        //   shell 与 CLI（`--unpack-package`）**共用同一份**。下面三条锁把"两处实现"钉死。
        {
            const std::string fn = efs2::package_folder_name("patch_00.xdb");
            check(efs2::package_folder_name("gamedata.db0") == "gamedata" &&
                      efs2::package_folder_name("gamedata.sq_base") == "gamedata" &&
                      efs2::package_folder_name("a.b.db0") == "a.b" && fn == "patch_00",
                  QStringLiteral("落点① 引擎包名去扩展名（gamedata.db0→gamedata / sq_base→gamedata / "
                                 "a.b.db0→a.b / patch_00.xdb→%1）")
                      .arg(QString::fromStdString(fn)));
            const std::string root = "E:/out";
            const std::string carry =
                efs2::unpack_target_dir(root, "E:/x/configs.db", {"configs/a.txt", "configs/b/c.ltx"});
            // ★★ 真机形状：条目表里**有一条名字正好等于壳的裸目录项**（`configs.db` 就是这样）
            //   —— 这也必须算"自带壳"，否则真机会叠出 `configs/configs`（2026-10-04 实测抓到的缺陷）。
            const std::string carry_bare =
                efs2::unpack_target_dir(root, "E:/x/configs.db", {"configs", "configs/ai_tweaks/x.ltx"});
            const std::string plain = efs2::unpack_target_dir(root, "E:/x/gamedata.db0", {"levels/a"});
            const std::string empty = efs2::unpack_target_dir(root, "E:/x/gamedata.db0", {});
            // ★★ 2026-10-04 **判红锁①**（缺陷①：量词"全部"⇒"过半"）：真机 9 个 VESSEL 包的形状 ——
            //   裸壳项 `ai` + **一条根级 `readme.md`** + 一堆 `ai/…`。旧量词（"**全部**条目都带壳"）
            //   被 `readme.md` 一条掀翻 ⇒ 判"不带壳" ⇒ 解出 `ai/ai/alife/…`，违背用户口径
            //   「这个案例，就解出来一个 configs。」⇒ 新量词"**过半**带壳"必须判"自带壳"＝落点即输出根。
            //   （把 `shelled * 2 > entry_paths.size()` 改回"任一条不带壳就 false" ⇒ 立刻红。）
            const std::string mixed =
                efs2::unpack_target_dir(root, "E:/x/ai.db", {"ai", "readme.md", "ai/alife/x.efd"});
            check(carry == root && carry_bare == root && plain == "E:/out/gamedata" &&
                      empty == "E:/out/gamedata" && carry.find("/.") == std::string::npos &&
                      mixed == root,
                  QStringLiteral("落点② 自带壳不叠第二层（%1）/ **裸壳目录项也算自带壳**（%2，真机形状）/ "
                                 "不带壳＝<根>/<包名>（%3）/ 空条目也叠（%4）/ 绝不出现 <根>/.（会被 "
                                 "safe_out_path 拒）/ **过半带壳就算自带壳**（%5：VESSEL 形状 = 裸壳项 + "
                                 "一条根级 readme.md + 一堆 壳/…）")
                      .arg(QString::fromStdString(carry), QString::fromStdString(carry_bare),
                           QString::fromStdString(plain), QString::fromStdString(empty),
                           QString::fromStdString(mixed)));
        }
        // ★★ 2026-10-04 **判红锁②**（缺陷②：报数与"解包产物预览"口径不符）—— 合成一枚
        //   **条目表重复登记**的 DB 包：`pack_db` 不查重，同一条路径给两条记录 ⇒ 包内两条、
        //   盘上一份（后写覆盖前写）—— 正是真机 `IWP\database\maps\*.db` 的形状（除根记录外的
        //   每条记录登记两遍；独立解码已证实重复在包内，不是 dump 的 bug）。
        //   判据：计数走**最终产物**口径（files=1 / bytes=2 / dirs=1），并如实报出重复记录数。
        //   （把计数改回"每处理一条记录就 +1" ⇒ files=2 / bytes=4 / dups=0 ⇒ 立刻红。）
        {
            namespace efs3 = xrkit::engine::fs;
            const QString dup_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dup"));
            QDir(dup_root).removeRecursively();
            QDir().mkpath(dup_root);
            const QString dup_pkg = dup_root + QStringLiteral("/dup.db");
            std::vector<efs3::DbPackItem> items;
            efs3::DbPackItem dup_dir;
            dup_dir.path = "dupdir";
            dup_dir.is_dir = true;
            items.push_back(dup_dir);
            efs3::DbPackItem dup_file;
            dup_file.path = "dupdir/a.txt";
            dup_file.data = {'X', 'Y'};
            items.push_back(dup_file);
            items.push_back(dup_file);   // ← 同路径的**第二条记录**（真机 maps\*.db 的形状）
            const std::vector<uint8_t> dup_raw = efs3::pack_db(items, efs3::DbFormat::Xdb);
            std::ofstream dup_out(dup_pkg.toStdString(), std::ios::binary | std::ios::trunc);
            dup_out.write(reinterpret_cast<const char*>(dup_raw.data()),
                          static_cast<std::streamsize>(dup_raw.size()));
            dup_out.close();

            const QString dup_target_root = dup_root + QStringLiteral("/out");
            // 落点照真机那条路算（CLI/GUI 共用的"包名壳"规则）：壳 `dup`、条目首段 `dupdir`
            //（不带壳）⇒ <根>/dup；然后才把**算好的落点**交给引擎导出（真机也是这个顺序）。
            const std::string dup_target = efs3::unpack_target_dir(
                dup_target_root.toStdString(), dup_pkg.toStdString(), {"dupdir", "dupdir/a.txt"});
            const efs3::ExtractTreeResult dup_res =
                efs3::extract_package(dup_pkg.toStdString(), dup_target);
            // 文件内容 2 字节，而且盘上只有**一份**（第二条记录是后写覆盖前写）
            const qint64 landed = QFileInfo(QString::fromStdString(dup_target) +
                                            QStringLiteral("/dupdir/a.txt")).size();
            const bool landed_at_shell =
                dup_target == (dup_target_root + QStringLiteral("/dup")).toStdString();
            QString dup_tree;
            {
                QDirIterator it(dup_root, QDir::AllEntries | QDir::NoDotAndDotDot,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) dup_tree += it.next() + QStringLiteral(" | ");
            }
            dup_tree = QString::fromStdString(dup_target) + QStringLiteral(" => ") + dup_tree;
            check(dup_res.ok && landed_at_shell && dup_res.files_written == 1 &&
                      dup_res.bytes_written == 2 && dup_res.dirs_created == 1 &&
                      dup_res.duplicate_records == 1 && landed == 2,
                  QStringLiteral("产物计数① 包内条目表重复登记 ⇒ 报数走**最终产物**口径"
                                 "（files=%1/期望 1 · bytes=%2/期望 2 · dirs=%3/期望 1 · "
                                 "dups=%4/期望 1 · 落盘 a.txt=%5/期望 2 字节 · 树=%6）")
                      .arg(dup_res.files_written)
                      .arg(dup_res.bytes_written)
                      .arg(dup_res.dirs_created)
                      .arg(dup_res.duplicate_records)
                      .arg(landed)
                      .arg(dup_tree));
            // 同一条判据的第二半：**计划行**（协议层的记录数）与**产物口径**必须是两个数
            check(efs3::unique_entry_count({"dupdir", "dupdir/a.txt", "dupdir/a.txt"}) == 2,
                  QStringLiteral("产物计数② 条目表去重：records=3 ⇒ entries=2（唯一路径数）"));
            QDir(dup_root).removeRecursively();
        }
        // ★ 2026-10-04（同一条架构红线）：**输出根缺省**与**封包落点/扩展名**这两条原先也只写在 FS 页里
        //   ⇒ CLI 复现不了（命令行与界面会算出不同的根/落点）。现在实现搬到引擎（`resolve_output_root`
        //   / `pack_ext_for` / `resolve_pack_target`），这里钉住引擎那一份的值，并断言**页面侧==引擎侧**
        //   （页面侧现已是对引擎的薄包装 ⇒ 谁把规则搬回页面或改任一侧，立刻红）。
        {
            const std::string eng_root = efs2::resolve_output_root("E:/x/a.db", "C:/cur");
            const std::string eng_root_empty = efs2::resolve_output_root("", "C:/cur");
            check(eng_root == "E:/x" && eng_root_empty == "C:/cur",
                  QStringLiteral("落点④ 输出根缺省（引擎）：有源⇒源所在目录「%1」；无源⇒当前目录「%2」")
                      .arg(QString::fromStdString(eng_root), QString::fromStdString(eng_root_empty)));
            check(FsPage::pack_ext_for(QStringLiteral("xdb")) ==
                          QString::fromStdString(efs2::pack_ext_for("xdb")) &&
                      FsPage::pack_ext_for(QStringLiteral("sqfs")) ==
                          QString::fromStdString(efs2::pack_ext_for("sqfs")) &&
                      efs2::pack_ext_for("xdb") == ".xdb.db" &&
                      efs2::pack_ext_for("sqfs") == ".sqfs",
                  QStringLiteral("落点⑤ 封包扩展名：页面==引擎（xdb「%1」/ sqfs「%2」）")
                      .arg(FsPage::pack_ext_for(QStringLiteral("xdb")),
                           FsPage::pack_ext_for(QStringLiteral("sqfs"))));
            // ⚠ 目录分支必须喂**真实存在的目录**（引擎照 GUI 用 `is_directory()` 判 —— 不存在的路径
            //   会按"无扩展名的路径"处理 ⇒ 补扩展名到它后面；这正是 1.x/GUI 的行为，别改语义）。
            const QString tdir = QDir::temp().filePath(QStringLiteral("xrkit_pack_target"));
            QDir(tdir).removeRecursively();
            QDir().mkpath(tdir);
            const efs2::PackTarget t_dir =
                efs2::resolve_pack_target(tdir.toStdString(), "mypack", "xdb");
            const efs2::PackTarget t_ext = efs2::resolve_pack_target("E:/out/mypack", "", "xdb");
            const efs2::PackTarget t_full = efs2::resolve_pack_target("E:/out/keep.db0", "", "xdb");
            const efs2::PackTarget t_none = efs2::resolve_pack_target("", "mypack", "xdb");
            const QString dir_path = QString::fromStdString(t_dir.path);
            check(!t_dir.needs_frontend_dialog && dir_path.endsWith(QStringLiteral("mypack.xdb.db")) &&
                      t_ext.path == "E:/out/mypack.xdb.db" &&   // 无扩展名 ⇒ 补 `.xdb.db`
                      t_full.path == "E:/out/keep.db0" &&       // 给了全路径 ⇒ 原样不动
                      t_none.needs_frontend_dialog && t_none.path == "mypack.xdb.db",
                  QStringLiteral("落点⑤ 封包落点：目录+基名「%1」/ 无扩展名「%2」/ 全路径「%3」/ 空⇒需前端弹框「%4」")
                      .arg(dir_path, QString::fromStdString(t_ext.path),
                           QString::fromStdString(t_full.path), QString::fromStdString(t_none.path)));
        }
        // ★★ **GUI 与 CLI 必须同落点**：本页 `plan_unpack()` 算出的 target 集合 == 引擎
        //   `plan_unpack_targets()` 算的（谁把落点规则搬回页面里、或改动任一侧 ⇒ 立刻红）。
        {
            const QString plan_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plan"));
            QDir(plan_root).removeRecursively();
            QDir().mkpath(plan_root);
            const QString a_path = plan_root + QStringLiteral("/plan_a.db0");
            const QString b_path = plan_root + QStringLiteral("/plan_b.db0");
            std::vector<efs2::DbPackItem> a_items;   // 条目**不带**同名壳 ⇒ 落点 = <根>/plan_a
            efs2::DbPackItem ai;
            ai.path = "levels/a.txt";
            ai.data = {'a'};
            a_items.push_back(ai);
            std::vector<efs2::DbPackItem> b_items;   // 条目**自带**同名壳 `plan_b/` ⇒ 落点 = <根>
            efs2::DbPackItem bi;
            bi.path = "plan_b/x.txt";
            bi.data = {'x'};
            b_items.push_back(bi);
            const std::pair<QString, std::vector<efs2::DbPackItem>*> pairs[2] = {{a_path, &a_items},
                                                                                {b_path, &b_items}};
            for (const auto& item : pairs) {
                const std::vector<uint8_t> raw = efs2::pack_db(*item.second, efs2::DbFormat::Xdb);
                std::ofstream out(item.first.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            FsPage plan_page;
            plan_page.resize(720, page_min_h());
            plan_page.show();
            QApplication::processEvents();
            plan_page.out_edit()->setText(QDir::toNativeSeparators(plan_root));
            plan_page.add_paths(QStringList{a_path, b_path});
            QApplication::processEvents();
            QString root_used;
            const std::vector<FsPage::UnpackJob> jobs = plan_page.plan_unpack(false, &root_used);
            std::vector<std::pair<std::string, std::vector<std::string>>> srcs;
            for (const FsPage::UnpackJob& j : jobs) {
                std::string err;
                const std::vector<efs2::EntryInfo> entries =
                    efs2::package_entries(j.image, std::nullopt, err);
                std::vector<std::string> paths;
                paths.reserve(entries.size());
                for (const efs2::EntryInfo& e : entries) paths.push_back(e.path);
                srcs.emplace_back(j.image, std::move(paths));
            }
            const std::vector<efs2::UnpackTarget> eng =
                efs2::plan_unpack_targets(root_used.toStdString(), srcs);
            bool same = !jobs.empty() && eng.size() == jobs.size();
            QString detail;
            for (size_t i = 0; same && i < jobs.size(); ++i) {
                if (eng[i].image != jobs[i].image || eng[i].target != jobs[i].target) same = false;
                detail += QString::fromStdString(eng[i].target) + QStringLiteral(" | ");
            }
            check(same && jobs.size() == 2,
                  QStringLiteral("落点③ GUI 与引擎**同落点**（%1 个源；引擎算的：%2）")
                      .arg(jobs.size())
                      .arg(detail));
            plan_page.hide();
            QDir(plan_root).removeRecursively();
        }

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

        // ★★ 2026-10-03 **新增锁**（用户定案的产物布局口径，原话）：「如果就拖入 `E:\...\IWP\database\
        //   config` 的 `configs`，那么**解包出来是一个 `configs` 文件夹**（和 `gamedata.db*`/`sq*`
        //   解出 `gamedata` 一样），**不包含 `database` 和 `config`**。」；我追问"包内路径本身就带一层
        //   `configs/` 会不会变成 `configs/configs`"他定案：**「这个案例，就解出来一个 configs。」**
        //   ⇒ 夹具刻意造成真机那种形状：包**嵌在 `database/config/` 里**、文件名 `configs.db0`、
        //     而且**包内路径自带 `configs/` 前缀**。判据＝产物顶层**恰好一个 `configs`**、
        //     **不出现 `configs/configs`**、**不出现 `database`/`config`**。
        //   （把 `plan_unpack` 里"包内已自带同名壳就不再叠壳"那段去掉 ⇒ 立刻冒出 `configs/configs` ⇒ 红。）
        {
            const QString nest = dir + QStringLiteral("/database/config");
            QDir().mkpath(nest);
            const QString nest_pkg = nest + QStringLiteral("/configs.db0");
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem dir_item;
            dir_item.path = "configs/ai_tweaks";
            dir_item.is_dir = true;
            items.push_back(dir_item);
            efs::DbPackItem file_item;
            file_item.path = "configs/ai_tweaks/ai.txt";
            file_item.data = {'K'};
            items.push_back(file_item);
            const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
            std::ofstream raw_out(nest_pkg.toStdString(), std::ios::binary | std::ios::trunc);
            raw_out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            raw_out.close();

            FsPage nest_page;
            nest_page.resize(760, page_min_h());
            nest_page.show();
            QApplication::processEvents();
            nest_page.add_paths({nest_pkg});
            const QString nest_out = dir + QStringLiteral("/nest_out");
            QDir().mkpath(nest_out);
            nest_page.out_edit()->setText(nest_out);
            QString nest_summary;
            nest_page.unpack_all(&nest_summary);
            const bool one_shell =
                QFileInfo(nest_out + QStringLiteral("/configs/ai_tweaks/ai.txt")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/configs/configs")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/database")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/config")).exists();
            check(one_shell,
                  QStringLiteral("产物布局① 包嵌在 database/config 里、包内自带 configs/ 前缀 ⇒ 顶层仍是"
                                 "**一个** configs（不出现 configs/configs、database、config）｜%1")
                      .arg(nest_summary.left(160)));
            nest_page.hide();
        }

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
        check(page.package_list()->topLevelItemCount() == 1 && page.loaded_source_count() == 1,
              QStringLiteral("① 包列表行数/已加载数对得上（行 %1 / 已加载 %2）")
                  .arg(page.package_list()->topLevelItemCount())
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
            // ★ 2026-10-02 树化：源节点散在树里（中间夹着分组节点）⇒ 必须**递归选源节点**，
            //   数顶层行是选不到源的（`<dir>/pk/gamedata.db0` 现在挂在分组节点 `pk/` 下面）。
            select_all_package_sources(scan_page.package_list());
            check(package_source_node_count(scan_page.package_list()) == scan_page.source_count(),
                  QStringLiteral("① 树化后**每个源**都有一个节点（源节点 %1 / 源 %2）")
                      .arg(package_source_node_count(scan_page.package_list()))
                      .arg(scan_page.source_count()));
            scan_page.load_selected();
            check(scan_page.loaded_source_count() == scan_page.source_count(),
                  QStringLiteral("① 加载选中：选中的都进树（已加载 %1 / 共 %2）")
                      .arg(scan_page.loaded_source_count())
                      .arg(scan_page.source_count()));
            const int before_unload = scan_page.entry_built_file_count();
            select_all_package_sources(scan_page.package_list());
            scan_page.unload_selected();
            check(before_unload > 0 && scan_page.loaded_source_count() == 0 &&
                      scan_page.entry_top_level_count() == 0 &&
                      scan_page.entry_built_file_count() == 0,
                  QStringLiteral("① 取消加载：条目从树里摘掉（%1 个文件项 → %2，顶层 %3）")
                      .arg(before_unload)
                      .arg(scan_page.entry_built_file_count())
                      .arg(scan_page.entry_top_level_count()));
            scan_page.clear_all();
            check(scan_page.source_count() == 0 &&
                      scan_page.package_list()->topLevelItemCount() == 0,
                  QStringLiteral("① 清空：已知包与列表都空（源 %1 / 行 %2）")
                      .arg(scan_page.source_count())
                      .arg(scan_page.package_list()->topLevelItemCount()));
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
            check(check_page.package_list()->topLevelItemCount() == 1 &&
                      check_page.package_list()->topLevelItem(0)->checkState(0) == Qt::Unchecked,
                  QStringLiteral("② 包行有勾选框且初始未勾"));
            check_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 2,
                  QStringLiteral("② 勾一个包 = 该包条目全勾（实测 %1 个文件）")
                      .arg(check_page.checked_file_count()));
            check_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 0,
                  QStringLiteral("② 取消勾选 = 该包条目全不勾（实测 %1）")
                      .arg(check_page.checked_file_count()));
            check_page.hide();
            pack_page.hide();
            QDir(pk_root).removeRecursively();
        }

        // ★★ 2026-10-01 用户第 7 条（原话）：「每次勾选或取消勾选包列表当中的文件，右边的文件列表
        //   似乎都会重建（因为在文件列表里勾选已展开的母文件夹，不会收起，而操作包，文件列表的
        //   所有文件夹都会收起）。这明明不需要重建，只有包列表内容改变（增加，减少，替换等）
        //   才需要重建右边的文件树。」
        //   判据（能判红：把 `on_package_item_changed` 结尾改回 `refresh_rows()` 立刻红）：
        //   勾/取消勾**已加载**的包 ⇒ 条目树**不重建**（同一个 QTreeWidgetItem 对象、仍是展开的、
        //   顶层项数不变）。
        {
            FsPage keep_page;
            keep_page.add_source(QStringLiteral("keep_a"),
                                 {efs::EntryInfo{std::string("d"), true, 0},
                                  efs::EntryInfo{std::string("d/a.txt"), false, 10}});
            keep_page.add_source(QStringLiteral("keep_b"),
                                 {efs::EntryInfo{std::string("d"), true, 0},
                                  efs::EntryInfo{std::string("d/b.txt"), false, 20}});
            keep_page.resize(900, page_min_h());
            keep_page.show();
            QApplication::processEvents();
            QTreeWidgetItem* dir_item = keep_page.entry_tree()->topLevelItem(0);
            if (dir_item != nullptr) dir_item->setExpanded(true);
            QApplication::processEvents();
            const int top_before = keep_page.entry_tree()->topLevelItemCount();
            const int kids_before = dir_item != nullptr ? dir_item->childCount() : -1;
            const bool expanded_before = dir_item != nullptr && dir_item->isExpanded();
            // 勾这一行的勾选框（这一包是"已加载"的 ⇒ 只该 toggle_source，不该重建树）
            keep_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            QTreeWidgetItem* dir_after = keep_page.entry_tree()->topLevelItem(0);
            check(expanded_before && dir_after == dir_item &&
                      dir_after->isExpanded() &&
                      keep_page.entry_tree()->topLevelItemCount() == top_before &&
                      dir_after->childCount() == kids_before,
                  QStringLiteral("第7条 勾包**不重建**右树：同一对象 %1 / expanded %2 / "
                                 "顶层 %3→%4 / 子项 %5→%6")
                      .arg(dir_after == dir_item ? 1 : 0)
                      .arg(dir_after != nullptr && dir_after->isExpanded() ? 1 : 0)
                      .arg(top_before)
                      .arg(keep_page.entry_tree()->topLevelItemCount())
                      .arg(kids_before)
                      .arg(dir_after != nullptr ? dir_after->childCount() : -1));
            // 取消勾选同理
            keep_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
            QApplication::processEvents();
            QTreeWidgetItem* dir_last = keep_page.entry_tree()->topLevelItem(0);
            check(dir_last == dir_item && dir_last->isExpanded(),
                  QStringLiteral("第7条 取消勾选同样不重建（同一对象 %1 / expanded %2）")
                      .arg(dir_last == dir_item ? 1 : 0)
                      .arg(dir_last != nullptr && dir_last->isExpanded() ? 1 : 0));
            keep_page.hide();
        }

        // ★ 2026-10-01 用户第 5 条：「移除这个按钮也可以优化掉，在包的条目后面增加一个叉叉按钮。」
        //   判据＝**点行尾 ✕ 真的移除那一行**：用合成的 `MouseButtonRelease` 驱动 col2 的委托
        //   `editorEvent`（判红：拿掉落点判定或回调 ⇒ 这条立刻红），并且"移除"只有
        //   `remove_source_at` 一个入口（工具行里的「移除」按钮已删）。
        {
            FsPage chai_page;
            chai_page.add_source(QStringLiteral("chai_a"),
                                 {efs::EntryInfo{std::string("x.txt"), false, 5}});
            chai_page.resize(760, page_min_h());
            chai_page.show();
            QApplication::processEvents();
            check(chai_page.source_count() == 1 &&
                      chai_page.package_list()->itemDelegateForColumn(2) != nullptr,
                  QStringLiteral("第5条 ✕ 列有自己的委托（源 %1 个）").arg(chai_page.source_count()));
            QAbstractItemDelegate* del = chai_page.package_list()->itemDelegateForColumn(2);
            const QModelIndex idx = chai_page.package_list()->model()->index(0, 2);
            QStyleOptionViewItem opt;
            opt.rect = chai_page.package_list()->visualRect(idx);
            // ★ 2026-10-02：✕ 改成**贴行尾**（末列吃剩余宽度，居中的话它飘在那一大块中间）⇒
            //   合成点击也要落在**行尾那个小方块**里（画的与命中共用同一个 `hit_rect`）。
            const QPoint click_at(opt.rect.right() - xrkit::core::px(18), opt.rect.center().y());
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(click_at),
                                QPointF(chai_page.package_list()->viewport()->mapToGlobal(click_at)),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            const bool handled = del != nullptr &&
                                 del->editorEvent(&release, chai_page.package_list()->model(), opt,
                                                  idx);
            QApplication::processEvents();
            check(handled && chai_page.source_count() == 0 &&
                      chai_page.package_list()->topLevelItemCount() == 0,
                  QStringLiteral("第5条 点行尾 ✕ = 移除该行（handled=%1 / 源 %2 / 行 %3）")
                      .arg(handled ? 1 : 0)
                      .arg(chai_page.source_count())
                      .arg(chai_page.package_list()->topLevelItemCount()));
            chai_page.hide();
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
                                  sp->widget(0) != nullptr && sp->widget(1) != nullptr &&
                                  sp->widget(0)->isAncestorOf(tab_page.package_list()) &&
                                  sp->widget(1)->isAncestorOf(tab_page.entry_tree()) &&
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
            // ★★ 2026-10-03 口径**有意改写**（用户：「这里母文件夹的"已加载"指的是**文件夹**已加载
            //   而不是**包的内容**已加载。怎么能这样呢？**右边永远不显示包体，除非包体本身在左边的
            //   包内，而不是文件夹内。**」）
            //   ⇒ **文件夹不再当"源"**：拖目录只剩"把里面的包登记进列表"。
            //   ⚠ 旧期望 2 = 1(先拖进来的那个包) + **1(散装层：目录自己也成了一个源)** —— 那正是被
            //     作废的 2026-09-29 口径；本夹具的目录里那个包**先前已经拖进来过**，所以现在
            //     `scan_dir` 去重后**源数不涨**。
            {
                QStringList seen;
                int dup = 0;
                for (int i = 0; i < drop_page.source_count(); ++i) {
                    const QString one = drop_page.source_at(i);
                    if (seen.contains(one, Qt::CaseInsensitive)) ++dup;
                    seen << one;
                }
                check(drop_page.source_count() == 1 && dup == 0,
                      QStringLiteral("⑦ 拖目录：**目录自己不成源**、里面的包已被登记过（实测 %1 个源 / "
                                     "重复 %2，期望 1/0 —— 旧期望 2 里那个散装层源已作废）")
                          .arg(drop_page.source_count())
                          .arg(dup));
            }
            // ★★ 2026-10-03：同一个目录拖两次 —— **源数一个都不涨**（目录不再是源，里面的包也被
            //   `scan_dir` 去重），并且要留下一行可见理由（本夹具目录里没有"新的"包）。
            {
                const int before = drop_page.source_count();
                QMimeData again_mime;
                again_mime.setUrls({QUrl::fromLocalFile(dir)});
                QDropEvent again_drop(QPointF(5, 5), Qt::CopyAction, &again_mime, Qt::LeftButton,
                                      Qt::NoModifier);
                if (drop_page.drop_filter() != nullptr) {
                    drop_page.drop_filter()->dispatch(&again_drop);
                }
                QApplication::processEvents();
                check(drop_page.source_count() == before && !drop_page.load_errors().isEmpty(),
                      QStringLiteral("⑦ 同一个目录拖两次不重复加源（源 %1→%2，可见理由 %3 条）")
                          .arg(before)
                          .arg(drop_page.source_count())
                          .arg(drop_page.load_errors().size()));
            }
            // ★ 正向断言（别让上面那条只证明"去重"）：**另建一个目录**、里面放一个**新**包 ⇒ 拖它 ⇒
            //   **源数 +1**（只有那个包；目录自己不算源）、**已加载 +0**（"拖进来的东西才加载"这条
            //   2026-10-03 起对目录不再适用：目录不加载任何东西）。
            {
                const QString dir2 = QDir::temp().filePath(QStringLiteral("xrkit_smoke_drop2"));
                QDir(dir2).removeRecursively();
                QDir().mkpath(dir2);
                std::vector<efs::DbPackItem> items2;
                efs::DbPackItem one2;
                one2.path = "z.txt";
                one2.data = {static_cast<uint8_t>('Z')};
                items2.push_back(one2);
                const std::vector<uint8_t> raw2 = efs::pack_db(items2, *efs::format_key_to_db("xdb"));
                std::ofstream out2((dir2 + QStringLiteral("/extra.db0")).toStdString(),
                                   std::ios::binary | std::ios::trunc);
                out2.write(reinterpret_cast<const char*>(raw2.data()),
                           static_cast<std::streamsize>(raw2.size()));
                out2.close();
                const int before = drop_page.source_count();
                const int loaded_before = drop_page.loaded_source_count();
                QMimeData mime2;
                mime2.setUrls({QUrl::fromLocalFile(dir2)});
                QDropEvent drop2(QPointF(5, 5), Qt::CopyAction, &mime2, Qt::LeftButton,
                                 Qt::NoModifier);
                if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&drop2);
                QApplication::processEvents();
                // ★★ 2026-10-03 口径**有意改写**（用户原话「**只要拖入包或者文件夹就加载上去**」；
                //   并实测「`IWP\database` 加载时长零点几秒，完全不影响」）⇒
                //   旧期望「+1 源 / **+0** 已加载」来自 2026-10-02 的"扫出来的只入列表不加载"，**已作废**。
                check(drop_page.source_count() == before + 1 &&
                          drop_page.loaded_source_count() == loaded_before + 1,
                      QStringLiteral("⑦ 目录里的包进列表并**当场加载**（拖入即加载）"
                                     "（源 %1→%2 期望 +1 / 已加载 %3→%4 期望 +1）")
                          .arg(before)
                          .arg(drop_page.source_count())
                          .arg(loaded_before)
                          .arg(drop_page.loaded_source_count()));
                QDir(dir2).removeRecursively();
            }
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
                    // ★ 2026-10-06（④）：格式下拉的**默认值现在是 auto**（1.x `fs_app.py:154`
                    //   `tk.StringVar(value="auto")`）⇒ 判定不再被"默认 xdb"兜着，而是走
                    //   引擎的**自洽判定**（歧义时显式拒绝，这是既有正确行为）。
                    //   夹具必须像**工具自己产出的包**那样带上目录条目：只有一条文件条目的
                    //   60 字节包在六种 DB 格式里**本来就分不开**（那是夹具不真实，不是回归）。
                    efs::DbPackItem dir_item;
                    dir_item.path = "gamedata";
                    dir_item.is_dir = true;
                    items.push_back(dir_item);
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
                // ★★ 2026-10-03 口径**两处有意改写**：
                //   ① 用户「**只要拖入包或者文件夹就加载上去**」⇒ 旧期望"0 加载、只登记"（2026-10-02 的
                //      "扫出来的只入列表"）**已作废**：拖目录 ⇒ 里面的包**当场全部加载**。
                //   ② 用户「**右边永远不显示包体，除非包体本身在左边的包内，而不是文件夹内**」＋
                //      「**右边罗列的是包的内容，换句话说就是解包产物的预览**」⇒ **散装层取消**：
                //      那个松散 `readme.txt` **既不在左树**（不是源）、**也不在右栏**（不进产物预览）。
                check(n == 1 && dir_page.loaded_source_count() == 1 && dir_page.source_count() == 1,
                      QStringLiteral("树③ 拖目录 ⇒ 里面的包**当场加载**（拖入即加载）"
                                     "（返回 %1 / 已加载 %2 / 源 %3，期望 1/1/1；松散文件不算源）")
                          .arg(n)
                          .arg(dir_page.loaded_source_count())
                          .arg(dir_page.source_count()));
                // 懒建：先展开 `gamedata`（以及 `gamedata/loose`）才找得到深层条目
                dir_page.expand_entry(QStringLiteral("gamedata"));
                dir_page.expand_entry(QStringLiteral("gamedata/loose"));
                QApplication::processEvents();
                // 右栏＝解包产物预览：**包内文件在、松散文件不在**（散装层已取消）
                check(dir_page.entry_top_level_count() > 0 &&
                          dir_page.find_entry_item(QStringLiteral("gamedata/loose/readme.txt")) == nullptr &&
                          dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr,
                      QStringLiteral("树③ 右栏＝解包产物预览：**包内文件在、松散文件不在**"
                                     "（顶层 %1 / 松散文件 %2 / 包内文件 %3，期望 >0/0/1）")
                          .arg(dir_page.entry_top_level_count())
                          .arg(dir_page.find_entry_item(QStringLiteral("gamedata/loose/readme.txt")) != nullptr ? 1 : 0)
                          .arg(dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr ? 1 : 0));
                // ★ 新增（"散装层取消"的可判红形式）：**松散文件不许进左树**（左树只有"文件夹 + 7 种包"）
                check(find_package_source_node(dir_page.package_list(), QStringLiteral("readme.txt")) == nullptr &&
                          find_package_source_node(dir_page.package_list(),
                                                   QStringLiteral("configs.db0")) != nullptr,
                      QStringLiteral("树③ 左树只有包（松散 readme.txt 不是源）"));
                // ★ 正向（钉住"拖入即加载"的边界）：**没有可加载的了** ⇒ `load_selected()` 返回 0
                //   （既不重复加载、也不会把松散文件当源）。
                {
                    QTreeWidgetItem* pkg_node = find_package_source_node(
                        dir_page.package_list(), QStringLiteral("configs.db0"));
                    if (pkg_node == nullptr) {
                        check(false, QStringLiteral("树③ 包列表里要能找到那个 `configs.db0` 源节点"));
                    } else {
                        pkg_node->setSelected(true);
                        const int loaded_now = dir_page.load_selected();
                        QApplication::processEvents();
                        dir_page.expand_entry(QStringLiteral("gamedata"));
                        QApplication::processEvents();
                        check(loaded_now == 0 && dir_page.loaded_source_count() == 1 &&
                                  dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr,
                              QStringLiteral("树③ 已加载的包不会重复加载（本次加载 %1 / 已加载 %2 / 包内文件 %3）")
                                  .arg(loaded_now)
                                  .arg(dir_page.loaded_source_count())
                                  .arg(dir_page.find_entry_item(QStringLiteral("gamedata/from_package.txt")) != nullptr ? 1 : 0));
                    }
                }
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
            //   **57 个包 / 18.9 GB**。
            //   ★★ 2026-10-03 口径改写：旧期望「已加载 1」来自"目录＝散装层源（拖进来的那层加载）"；
            //   现在**文件夹不贡献任何条目** ⇒ 拖目录**一个都不加载**，9 个包全进列表。
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
                // ★★ 2026-10-03 口径**改写**：这条旧锁原本钉的是"**包多也不加载**"（9 个包 ⇒ 0 加载），
                //   依据是 2026-10-02 的"扫出来的只入列表"＋担心一次解 18.9 GB。
                //   用户已改口径并给了实测：**「只要拖入包或者文件夹就加载上去」**、
                //   **「实测 `IWP\database` 加载时长零点几秒，完全不影响」**
                //   ⇒ 现在断的是**反面**：9 个包**全部加载**（且不许再出现任何数量阈值/分档）。
                check(many_page.loaded_source_count() == 9 && many_page.source_count() == 9,
                      QStringLiteral("包多也**全部加载**（无阈值、无分档）：目录里的 9 个包"
                                     "（已加载 %1 期望 9、已知源 %2 期望 9）")
                          .arg(many_page.loaded_source_count())
                          .arg(many_page.source_count()));
                many_page.hide();
                QDir(many).removeRecursively();
            }

            // ★★ 2026-10-02 **阈值已删** + 2026-10-03 口径**改写**（用户：「**只要拖入包或者文件夹
            //   就加载上去**」，并实测"零点几秒，完全不影响"）：
            //   旧 `kAutoLoadMax = 8`：≤8 个包就全自动加载（1 散装层 + 3 包 = 4）；
            //   上一版：只加载"拖进来的那一层"（散装层）= 1；
            //   **现在：目录里的 3 个包全部加载 = 3**（"目录不成源"这半仍然成立 ⇒ 源数＝包数）。
            {
                const QString few = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fewpkg"));
                QDir(few).removeRecursively();
                QDir().mkpath(few);
                for (int i = 0; i < 3; ++i) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "g" + std::to_string(i) + ".txt";
                    one.is_dir = false;
                    one.data = {'y'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(few.toStdString() + "/q" + std::to_string(i) + ".db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage few_page;
                few_page.resize(760, page_min_h());
                few_page.show();
                QApplication::processEvents();
                few_page.add_paths({few});
                QApplication::processEvents();
                check(few_page.loaded_source_count() == 3 && few_page.source_count() == 3,
                      QStringLiteral("阈值已删且目录不成源：3 个包的目录 ⇒ **3 个全加载 / 3 个已知源**"
                                     "（已加载 %1 期望 3 / 源 %2 期望 3）")
                          .arg(few_page.loaded_source_count())
                          .arg(few_page.source_count()));
                few_page.hide();
                QDir(few).removeRecursively();
            }

            // ★★ 2026-10-03 新增锁（用户：「**包的内容没有全部勾选的时候，包的勾选框也应当处于
            //   部分勾选的状态。**」）—— 判据：包内全勾 ⇒ 包节点 ✓；**勾掉其中一个条目 ⇒ 包节点 ■**。
            //   （把 `on_entry_item_changed` 里那句 `refresh_package_checks()` 去掉立刻红：卡在 ✓。）
            {
                FsPage part_page;
                part_page.add_source(QStringLiteral("part_a"),
                                     {efs::EntryInfo{std::string("x.txt"), false, 5},
                                      efs::EntryInfo{std::string("y.txt"), false, 7}});
                part_page.resize(760, page_min_h());
                part_page.show();
                QApplication::processEvents();
                if (part_page.check_all_button() != nullptr) part_page.check_all_button()->click();
                QApplication::processEvents();
                QTreeWidgetItem* pkg_node = find_package_source_node(part_page.package_list(),
                                                                     QStringLiteral("part_a"));
                const Qt::CheckState before =
                    pkg_node != nullptr ? pkg_node->checkState(0) : Qt::Unchecked;
                QTreeWidgetItem* one_entry = part_page.find_entry_item(QStringLiteral("x.txt"));
                if (one_entry != nullptr) one_entry->setCheckState(0, Qt::Unchecked);
                QApplication::processEvents();
                check(pkg_node != nullptr && before == Qt::Checked &&
                          pkg_node->checkState(0) == Qt::PartiallyChecked,
                      QStringLiteral("包的三态：全勾时 ✓（实测 %1）、勾掉一个条目后 ■（实测 %2）")
                          .arg(static_cast<int>(before))
                          .arg(pkg_node != nullptr ? static_cast<int>(pkg_node->checkState(0)) : -1));
                part_page.hide();
            }

            // ★★ 2026-10-03 新增锁（用户：「**右边永远不显示包体，除非包体本身在左边的包内，
            //   而不是文件夹内。**」＋「右边的包树和左边是一模一样的…很明显不应该这样」）——
            //   夹具：目录下有**两个子目录**各放一个包 ⇒ 左树**根应当是分组节点**（不是源节点）：
            //   `src<0`、无大小、目录里的包各归各位；**0 加载**（文件夹不贡献任何条目）。
            //   （旧的"目录＝散装层源"实现下，根会是一个**源**节点 ⇒ 这条红。）
            {
                const QString root2 = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pkgroot"));
                QDir(root2).removeRecursively();
                QDir().mkpath(root2 + QStringLiteral("/s1"));
                QDir().mkpath(root2 + QStringLiteral("/s2"));
                for (const QString& rel : {QStringLiteral("/s1/p1.db0"),
                                           QStringLiteral("/s2/p2.db0")}) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "t.txt";
                    one.data = {'t'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out((root2 + rel).toStdString(),
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage root_page;
                root_page.resize(760, page_min_h());
                root_page.show();
                QApplication::processEvents();
                root_page.add_paths({root2});
                QApplication::processEvents();
                QTreeWidget* list = root_page.package_list();
                QTreeWidgetItem* root_node = list != nullptr ? list->topLevelItem(0) : nullptr;
                const bool root_is_group =
                    root_node != nullptr && root_node->data(0, Qt::UserRole).toInt() < 0 &&
                    root_node->text(1).isEmpty();
                // ★★ 2026-10-03 口径改写：旧期望"已加载 0"来自 2026-10-02 的"扫出来的只入列表"；
                //   现在用户定了「拖入包或者文件夹就加载上去」⇒ 两个包都加载 ⇒ **已加载 2**。
                //   （"顶层恰好 1 个分组节点、根不是源"这一半照旧 ✓）
                check(root_page.source_count() == 2 && root_page.loaded_source_count() == 2 &&
                          list != nullptr && list->topLevelItemCount() == 1 && root_is_group &&
                          root_node->childCount() == 2,
                      QStringLiteral("包树根＝分组节点：源 %1（期望 2）/ 已加载 %2（期望 2）/ 顶层 %3"
                                     "（期望 1）/ 根是分组 %4 / 根的子项 %5（期望 2）")
                          .arg(root_page.source_count())
                          .arg(root_page.loaded_source_count())
                          .arg(list != nullptr ? list->topLevelItemCount() : -1)
                          .arg(root_is_group ? 1 : 0)
                          .arg(root_node != nullptr ? root_node->childCount() : -1));
                root_page.hide();
                QDir(root2).removeRecursively();
            }

            // ★★ 2026-10-02 包列表**树化**（用户：「左边的包列表的结构应该也是一个文件树」）——
            //   一条夹具同时判三件事：**分组节点存在**、**分组节点不可勾选/无 ✕/无大小**、
            //   **子目录里的包挂在分组节点下面**。夹具：`tmp/a.db0` + `tmp/sub/b.db0`。
            {
                const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pkgtree"));
                QDir(root).removeRecursively();
                QDir().mkpath(root + QStringLiteral("/sub"));
                const QStringList rels{QStringLiteral("/a.db0"), QStringLiteral("/sub/b.db0")};
                for (const QString& rel : rels) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "t.txt";
                    one.is_dir = false;
                    one.data = {'t'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out((root + rel).toStdString(), std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage tree_list_page;
                tree_list_page.resize(760, page_min_h());
                tree_list_page.show();
                QApplication::processEvents();
                tree_list_page.add_paths({root});
                QApplication::processEvents();
                QTreeWidget* list = tree_list_page.package_list();
                // 顶层第一项 = 散装层（目录名带 '/'）；它的子项里应当有 `sub/`（纯分组节点）与 `a.db0`
                QTreeWidgetItem* root_item = list != nullptr ? list->topLevelItem(0) : nullptr;
                QTreeWidgetItem* sub_item = nullptr;
                QTreeWidgetItem* pkg_a_item = nullptr;
                QTreeWidgetItem* pkg_b_item = nullptr;
                if (root_item != nullptr) {
                    for (int i = 0; i < root_item->childCount(); ++i) {
                        QTreeWidgetItem* child = root_item->child(i);
                        if (child->text(0) == QStringLiteral("sub/")) sub_item = child;
                        if (child->text(0).startsWith(QStringLiteral("a.db0"))) pkg_a_item = child;
                    }
                }
                if (sub_item != nullptr) {
                    for (int i = 0; i < sub_item->childCount(); ++i) {
                        if (sub_item->child(i)->text(0).startsWith(QStringLiteral("b.db0"))) {
                            pkg_b_item = sub_item->child(i);
                        }
                    }
                }
                check(root_item != nullptr && pkg_a_item != nullptr && sub_item != nullptr &&
                          pkg_b_item != nullptr,
                      QStringLiteral("包列表树① 分组节点 + 子目录里的包挂在它下面"
                                     "（根 %1 / a.db0 %2 / sub %3 / b.db0 %4）")
                          .arg(root_item != nullptr ? 1 : 0)
                          .arg(pkg_a_item != nullptr ? 1 : 0)
                          .arg(sub_item != nullptr ? 1 : 0)
                          .arg(pkg_b_item != nullptr ? 1 : 0));
                const bool group_checkable =
                    sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable);
                // ★★ 2026-10-02（用户「包树直接引用树的统一函数就行啊」之后）：**默认收起**的树
                //   还必须**点得开** —— 发一条 `itemClicked`（等价于用户点条目身体）⇒ 该节点应被展开。
                //   离屏时 `click_on_indicator` 判为"不在那一小块上"⇒ 走"点身体"这条路（见它的注释）。
                //   ★ 这条同时守住那个坑：包列表在平表时代留过 `setRootIsDecorated(false)`，
                //     树化 + 默认收起之后顶层节点的孩子就**再也点不出来**了（那条断言在上面第 6 条里）。
                {
                    const bool was_expanded = root_item != nullptr && root_item->isExpanded();
                    if (root_item != nullptr) {
                        QMetaObject::invokeMethod(list, "itemClicked", Q_ARG(QTreeWidgetItem*, root_item),
                                                  Q_ARG(int, 0));
                        QApplication::processEvents();
                    }
                    check(root_item != nullptr && !was_expanded && root_item->isExpanded(),
                          QStringLiteral("包列表树④ 单击节点身体＝展开（收起 %1 → 展开 %2）")
                              .arg(was_expanded ? 1 : 0)
                              .arg(root_item != nullptr && root_item->isExpanded() ? 1 : 0));
                }
                // ★★ 2026-10-02 口径**有意改写**（用户：「包树和文件树逻辑的唯一差别，就是包树多一个
                //   是否加载。**其他行为应当完全一样**。」）⇒ 分组节点**改成可勾选**（带三态，
                //   勾它＝连带勾/去勾它下面的所有源），不再要求"不可勾选"。仍然无大小、无 ✕（下标 <0）。
                check(sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable) &&
                          sub_item->text(1).isEmpty() &&
                          sub_item->data(0, Qt::UserRole).toInt() < 0,
                      QStringLiteral("包列表树③ 分组节点：可勾选（三态）%1、大小「%2」、源下标 %3（应 <0，"
                                     "委托就是按它不画 ✕）")
                          .arg(sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable) ? 1 : 0)
                          .arg(sub_item != nullptr ? sub_item->text(1) : QString())
                          .arg(sub_item != nullptr ? sub_item->data(0, Qt::UserRole).toInt() : -999));
                // ★★ 2026-10-03 口径**改写**：旧判据「连带勾 + **不代加载**」（2026-10-02 为防
                //   "一次点击解 57 个包"而定）**已作废** —— 用户：「勾文件夹要不要顺带加载
                //   **肯定要加载**。」⇒ 这里保留"连带勾"这条（本夹具里的包在**拖入时就已经加载**，
                //   所以已加载数不变是正常的）；"顺带加载"由下面那条独立锁（⑥，用 `scan_dir()`
                //   造出真正的"未加载源"）判。
                {
                    const int before_loaded = tree_list_page.loaded_source_count();
                    if (sub_item != nullptr) {
                        sub_item->setCheckState(0, Qt::Checked);
                        QApplication::processEvents();
                    }
                    bool all_children_checked = sub_item != nullptr && sub_item->childCount() > 0;
                    for (int i = 0; sub_item != nullptr && i < sub_item->childCount(); ++i) {
                        if (sub_item->child(i)->checkState(0) == Qt::Unchecked) all_children_checked = false;
                    }
                    check(all_children_checked &&
                              tree_list_page.loaded_source_count() == before_loaded,
                          QStringLiteral("包列表树⑤ 勾分组＝连带勾它下面所有源 %1（本夹具已全部加载，"
                                         "已加载 %2→%3 不变）")
                              .arg(all_children_checked ? 1 : 0)
                              .arg(before_loaded)
                              .arg(tree_list_page.loaded_source_count()));
                }
                // ★★ 2026-10-03 **新增锁**（用户：「勾文件夹要不要顺带加载**肯定要加载**。」）：
                //   夹具必须用 `scan_dir()` —— 它是"拖入即加载"之后**唯一还能造出"未加载源"的入口**
                //   （`add_paths` 现在一律当场加载）。勾那个分组节点 ⇒ **两个包都变成已加载**、
                //   右栏随即出现它们的条目。**把 `toggle_group_at` 里的加载那步去掉立刻红**（停在 0）。
                {
                    const QString gdir =
                        QDir::temp().filePath(QStringLiteral("xrkit_smoke_groupcheck"));
                    QDir(gdir).removeRecursively();
                    QDir().mkpath(gdir);
                    // ⚠ 两个包必须放在**不同子目录**里：都在同一层时左树顶层是**源节点**（没有分组），
                    //   拿它当"分组"去勾就只是在勾一个包（第一版就这么写错了，当场红：已加载 0→1）。
                    for (int i = 0; i < 2; ++i) {
                        QDir().mkpath(gdir + QStringLiteral("/sub%1").arg(i));
                        std::vector<efs::DbPackItem> items;
                        efs::DbPackItem one;
                        one.path = "pack" + std::to_string(i) + ".txt";
                        one.is_dir = false;
                        one.data = {'G'};
                        items.push_back(one);
                        const std::vector<uint8_t> raw =
                            efs::pack_db(items, *efs::format_key_to_db("xdb"));
                        std::ofstream out(
                            (gdir + QStringLiteral("/sub%1/g%1.db0").arg(i)).toStdString(),
                            std::ios::binary | std::ios::trunc);
                        out.write(reinterpret_cast<const char*>(raw.data()),
                                  static_cast<std::streamsize>(raw.size()));
                    }
                    FsPage gc_page;
                    gc_page.resize(760, page_min_h());
                    gc_page.show();
                    QApplication::processEvents();
                    gc_page.scan_dir(gdir);   // ★ 只登记、不加载（造出"未加载源"）
                    QApplication::processEvents();
                    QTreeWidget* gcl = gc_page.package_list();
                    QTreeWidgetItem* ggroup = gcl != nullptr ? gcl->topLevelItem(0) : nullptr;
                    const bool is_group =
                        ggroup != nullptr && ggroup->data(0, Qt::UserRole).toInt() < 0;
                    const int gbefore = gc_page.loaded_source_count();
                    if (ggroup != nullptr) {
                        ggroup->setCheckState(0, Qt::Checked);
                        QApplication::processEvents();
                    }
                    check(is_group && gbefore == 0 && gc_page.loaded_source_count() == 2 &&
                              gc_page.entry_top_level_count() >= 2,
                          QStringLiteral("包列表树⑥ 勾分组 ⇒ **顺带真的加载**（是分组 %1 / 已加载 %2→%3 / "
                                         "右栏顶层 %4，期望 1 / 0→2 / ≥2）")
                              .arg(is_group ? 1 : 0)
                              .arg(gbefore)
                              .arg(gc_page.loaded_source_count())
                              .arg(gc_page.entry_top_level_count()));
                    gc_page.hide();
                    QDir(gdir).removeRecursively();
                }
                check(package_source_node_count(list) == tree_list_page.source_count(),
                      QStringLiteral("包列表树 源节点数 == 源数（节点 %1 / 源 %2）")
                          .arg(package_source_node_count(list))
                          .arg(tree_list_page.source_count()));
                tree_list_page.hide();
                QDir(root).removeRecursively();
            }

            // ★★ 2026-10-02 用户报「包树崩了」。真因：**在 `itemChanged` 槽里重建了包树**
        //   （`load_selected()` → `rebuild_package_list()` → `clear()` 把**正在处理的那个 item 删掉**，
        //   槽返回后 Qt 继续用它 ⇒ use-after-free ⇒ 崩）。同族的第二处＝✕ 委托在 `editorEvent` 里直接重建。
        //   判据（可判红）：勾一个**未加载**的包之后，**同一个 `QTreeWidgetItem` 对象还在**（说明没重建）。
        //   把 `load_one_source` 换回 `load_selected() + rebuild_package_list()` ⇒ 指针变了 ⇒ 红。
        {
            const QString cdir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_crash"));
            QDir(cdir).removeRecursively();
            QDir().mkpath(cdir);
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem one;
            one.path = "p/x.txt";
            one.data = {'t'};
            items.push_back(one);
            const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
            std::ofstream out((cdir + QStringLiteral("/gamedata.db0")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
            out.close();
            FsPage crash_page;
            crash_page.resize(760, page_min_h());
            crash_page.show();
            QApplication::processEvents();
            crash_page.scan_dir(cdir);       // 只入列表、未加载
            QApplication::processEvents();
            QTreeWidgetItem* row0 = crash_page.package_list()->topLevelItem(0);
            const int top_before = crash_page.package_list()->topLevelItemCount();
            const bool was_loaded = crash_page.loaded_source_count() > 0;
            if (row0 != nullptr) row0->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(row0 != nullptr && !was_loaded &&
                      crash_page.package_list()->topLevelItemCount() == top_before &&
                      crash_page.package_list()->topLevelItem(0) == row0 &&
                      crash_page.loaded_source_count() > 0,
                  QStringLiteral("包树不崩① 勾未加载的包：**就地加载、不重建包树**（同一对象 %1 / 顶层 %2→%3 / "
                                 "已加载 %4→%5）")
                      .arg(crash_page.package_list()->topLevelItem(0) == row0 ? 1 : 0)
                      .arg(top_before)
                      .arg(crash_page.package_list()->topLevelItemCount())
                      .arg(was_loaded ? 1 : 0)
                      .arg(crash_page.loaded_source_count()));
            crash_page.hide();
            QDir(cdir).removeRecursively();
        }

        // ★ 用户 2026-09-30：三层结构里勾最内层文件 ⇒ **最外层祖先也要显示"部分勾选"**
            //   （修前只算直接父目录 ⇒ 最外层恒未选中 ⇒ 这条红）。
            {
                FsPage tri_page;
                tri_page.resize(760, page_min_h());
                tri_page.show();
                QApplication::processEvents();
                tri_page.add_source(QStringLiteral("s"),
                                    {{"meshes/actors/actor_models/green_stalker_10.ogf", false},
                                     {"meshes/actors/actor_models/green_stalker_15.ogf", false},
                                     {"configs/ai_tweaks/ai_tweaks.ltx", false}});
                QApplication::processEvents();
                // 懒建要**逐级**展开（每展开一层才建下一层的子项）
                tri_page.expand_entry(QStringLiteral("meshes"));
                tri_page.expand_entry(QStringLiteral("meshes/actors"));
                tri_page.expand_entry(QStringLiteral("meshes/actors/actor_models"));
                QApplication::processEvents();
                QTreeWidgetItem* deep = tri_page.find_entry_item(
                    QStringLiteral("meshes/actors/actor_models/green_stalker_10.ogf"));
                if (deep == nullptr) {
                    check(false, QStringLiteral("三态传播：判据自己要能跑到 —— 找不到最内层文件项"));
                } else {
                    deep->setCheckState(0, Qt::Checked);   // 触发 itemChanged ⇒ 回灌模型
                }
                QApplication::processEvents();
                QTreeWidgetItem* mid = tri_page.find_entry_item(QStringLiteral("meshes/actors"));
                QTreeWidgetItem* top = tri_page.find_entry_item(QStringLiteral("meshes"));
                const Qt::CheckState mid_state = mid != nullptr ? mid->checkState(0) : Qt::Unchecked;
                const Qt::CheckState top_state = top != nullptr ? top->checkState(0) : Qt::Unchecked;
                check(top_state == Qt::PartiallyChecked,
                      QStringLiteral("三态传播：勾最内层文件 ⇒ 最外层要部分勾选（实测 中间层=%1 最外层=%2）")
                          .arg(static_cast<int>(mid_state))
                          .arg(static_cast<int>(top_state)));
                tri_page.hide();
            }
            // ★ 用户 2026-09-30 与 2026-10-01 **两次**指出「'包内条目'那一行字顶部被切」
            //   ⇒ 这次把机制量清楚了（`--shot` 的 HDR 行）：表头高度由 QHeaderView 按**它自己的
            //   字体**算，而 QSS 的 `padding` 又被用作内容区的内缩 ⇒ 8 px 上下内边距会把
            //   15 px 的字压在 9 px 的内容区里、顶部直接切掉（实测表头 25 px）。
            //   判据：**表头高 ≥ 表头自己的字高 + 2×QSS 上下内边距(现为 pad4)** —— 两边任一处
            //   回退（字号不跟内容走 / 内边距又调回 8）这条就会红。
            {
                FsPage head_page;
                head_page.resize(760, page_min_h());
                head_page.show();
                QApplication::processEvents();
                QHeaderView* hh = head_page.entry_tree()->header();
                const int font_h = hh != nullptr ? hh->fontMetrics().height() : -1;
                const int need = font_h + 2 * xrkit::core::px(4);
                check(hh != nullptr && hh->height() >= need,
                      QStringLiteral("表头不许被裁：表头高 %1 ≥ 字高 %2 + 2×内边距 %3 = %4")
                          .arg(hh != nullptr ? hh->height() : -1)
                          .arg(font_h)
                          .arg(xrkit::core::px(4))
                          .arg(need));
                // ★ 加一条**有牙**的锁：表头高度不随 QSS 内边距变（padding 只影响内容区），
                //   所以光比高度抓不住"有人把内边距调回 8px" —— 直接钉住样式表里那一节的
                //   上下内边距必须是 px(4)（8px 那版实测把 15px 的字压在 9px 内容区里、顶部被切）。
                {
                    const QString qss = xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
                    const int at = qss.lastIndexOf(QStringLiteral("QHeaderView::section {"));
                    const QString rule = at >= 0 ? qss.mid(at) : QString();
                    check(rule.contains(QStringLiteral("padding: %1px ").arg(xrkit::core::px(4))),
                          QStringLiteral("表头 QSS 的上下内边距必须是 px(%1)（回退到 8px 就会裁字）")
                              .arg(xrkit::core::px(4)));
                }
                head_page.hide();
            }
            // ★ 用户 2026-09-30：「日志记录的是什么东西，为什么没有在 app 当中的操作（例如，加载包）？」
            //   判据：在**真 Hub** 里让 fs 页加载一个包 ⇒ 日志栏必须出现「已加载」行。
            //   （页面单独构造时 `window()` 不是 Hub，这条路测不到 ⇒ 必须在 Hub 里测。）
            {
                Hub log_hub(nullptr, QString());
                const QString tmp = QDir::temp().filePath(QStringLiteral("xrkit_smoke_log"));
                QDir(tmp).removeRecursively();
                QDir().mkpath(tmp);
                {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "configs/ai_tweaks/ai_tweaks.ltx";
                    one.is_dir = false;
                    one.data = {'x', 'y'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(tmp.toStdString() + "/p.db0", std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage* fs = nullptr;
                for (int i = 0; i < log_hub.page_count(); ++i) {
                    if (log_hub.page_id(i) == QStringLiteral("fs"))
                        fs = dynamic_cast<FsPage*>(log_hub.page_at(i));
                }
                if (fs != nullptr) {
                    fs->add_paths({tmp + QStringLiteral("/p.db0")});
                }
                // 日志是**攒批**写的 ⇒ 等真时间（最多 1.5 s），别用"跑几次 processEvents"当"已经刷出来了"
                QString text;
                for (int i = 0; i < 150; ++i) {
                    QApplication::processEvents();
                    text = log_hub.log_view() != nullptr ? log_hub.log_view()->toPlainText() : QString();
                    if (text.contains(QStringLiteral("已加载"))) break;
                    QThread::msleep(10);
                }
                check(fs != nullptr && text.contains(QStringLiteral("已加载")),
                      QStringLiteral("日志要记 app 内操作：Hub 里加载包后日志要有「已加载」（页 %1 / 命中 %2 / 日志长度 %3）")
                          .arg(fs != nullptr ? 1 : 0)
                          .arg(text.contains(QStringLiteral("已加载")) ? 1 : 0)
                          .arg(text.size()));
                QDir(tmp).removeRecursively();
            }
            // ★ 用户 2026-09-30 ⑦：「现在还是慢得不行。最起码，要追上X-Ray Engine这个优化奇烂的老东西吧。」
            //   先量数字再改：引擎侧实测「15,374 条目 / 106 ms」（SquashFS 真镜像），这里量 **UI 侧**
            //   （`add_paths` ＝过闸门 + 解条目表 + 合进 MergeTree + 建界面树）用同一量级的夹具。
            {
                const QString big = QDir::temp().filePath(QStringLiteral("xrkit_smoke_big.db0"));
                {
                    std::vector<efs::DbPackItem> items;
                    items.reserve(15000);
                    for (int i = 0; i < 15000; ++i) {
                        efs::DbPackItem one;
                        one.path = "configs/ai_tweaks/dir" + std::to_string(i / 100) + "/f" +
                                   std::to_string(i) + ".ltx";
                        one.is_dir = false;
                        one.data = {'x'};
                        items.push_back(std::move(one));
                    }
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(big.toStdString(), std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage big_page;
                big_page.resize(760, page_min_h());
                big_page.show();
                QApplication::processEvents();
                const auto began = std::chrono::steady_clock::now();
                big_page.add_paths({big});
                QApplication::processEvents();
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - began)
                                         .count();
                std::fprintf(stderr, "SMOKE-TIMING add_paths_15000_entries=%lld ms\n", ms);
                check(ms < 5000,
                      QStringLiteral("载 1.5 万条目不许数量级回归：实测 %1 ms（引擎侧同量级 106 ms）").arg(ms));
                big_page.hide();
                QFile::remove(big);
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

        // 6.3：两个"全选"
        page.select_all_button()->click();
        check(page.selection().size() == 2, QStringLiteral("6.3 「全选」改了包列表选中态"));
        // ★★ 2026-10-01 **口径再次改写（回退）**——用户原话：「全选就直接勾选了？不能这样。」
        //   「全选包，功能是选中而非勾选。」⇒ 「全选」（原「全选包」）**只改选中态**，
        //   勾选态一个都不碰（上一轮我把它改成"选中+勾选"，被用户当场否掉）。
        check(page.checked_file_count() == 0,
              QStringLiteral("6.3 「全选」**不碰**包内勾选态（实测勾了 %1 个）")
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

        // ★★ 2026-10-06（②）统计框三行：`gameplay / scripts / 总计`
        //   （1.x `text_extract_app.py:647-656` 的 `stat_grid` + `:725-729` 写值）
        //   ① 真跑一遍：标签上的数字必须**来自引擎给的 `ExtractStats`**（页面不自己数）
        {
            const etxt::ExtractStats real = page.extract_one(
                root + QStringLiteral("/scripts"), root + QStringLiteral("/out_d"),
                QStringLiteral("smoke"), QStringLiteral("smoke_sc_0"), true);
            // 与 worker 里那条 `QMetaObject::invokeMethod(... note_stats ...)` 是**同一条入口**
            page.note_stats(real);
            const QStringList lines = page.stats_text().split(QLatin1Char('\n'));
            check(real.ok && lines.size() == 3 && lines[1].startsWith(QStringLiteral("scripts")) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.strings))) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.files))) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.chars))) &&
                      page.stats_label() != nullptr &&
                      page.stats_label()->text() == page.stats_text(),
                  QStringLiteral("② 真提取后 scripts 行 = ExtractStats（strings=%1 files=%2 chars=%3 →「%4」）")
                      .arg(real.strings)
                      .arg(real.files)
                      .arg(real.chars)
                      .arg(lines.size() > 1 ? lines[1] : QString()));
        }
        //   ② 固定夹具：三行数字**逐字段**对得上（把某一行删掉 ⇒ 立刻红）
        {
            etxt::ExtractStats gp;
            gp.ok = true;
            gp.kind = etxt::ExtractKind::Gameplay;
            gp.files = 7;
            gp.strings = 11;
            gp.chars = 1234;
            etxt::ExtractStats sc;
            sc.ok = true;
            sc.kind = etxt::ExtractKind::Scripts;
            sc.files = 3;
            sc.strings = 5;
            sc.chars = 66;
            page.note_stats(gp);
            page.note_stats(sc);
            const QStringList lines = page.stats_text().split(QLatin1Char('\n'));
            check(lines.size() == 3 && lines[0].startsWith(QStringLiteral("gameplay")) &&
                      lines[1].startsWith(QStringLiteral("scripts")) &&
                      lines[2].startsWith(QStringLiteral("总计")),
                  QStringLiteral("② 统计框**三行**：gameplay / scripts / 总计（实得 %1 行）")
                      .arg(lines.size()));
            check(lines.size() == 3 &&
                      lines[0] == QStringLiteral("gameplay：11 字符串 · 7 文件 · 1234 字符") &&
                      lines[1] == QStringLiteral("scripts：5 字符串 · 3 文件 · 66 字符"),
                  QStringLiteral("② 三行数字逐字段 = ExtractStats（实得「%1」/「%2」）")
                      .arg(lines.value(0), lines.value(1)));
            check(lines.size() == 3 &&
                      lines[2] == QStringLiteral("总计：16 字符串 · 10 文件 · 1300 字符"),
                  QStringLiteral("② 总计 = 两支逐字段之和（实得「%1」）").arg(lines.value(2)));
        }
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

        // ★★ 2026-10-06（⑤）三个小件：清除 / 统计标签（文件夹名）/ CSV 真实时间
        //   ① 「清除」（1.x `:391-392` + `_clear_entries` `:514-518`）：清两个目录栏
        {
            check(page.clear_button() != nullptr, QStringLiteral("⑤ 页面上有「清除」按钮"));
            page.dir_a_edit()->setText(QStringLiteral("E:/somewhere/a"));
            page.dir_b_edit()->setText(QStringLiteral("E:/somewhere/b"));
            if (page.clear_button() != nullptr) page.clear_button()->click();
            check(page.dir_a_edit()->text().isEmpty() && page.dir_b_edit()->text().isEmpty(),
                  QStringLiteral("⑤ 清除后两栏为空（实得「%1」/「%2」）")
                      .arg(page.dir_a_edit()->text(), page.dir_b_edit()->text()));
            // 复位成"刚比较过的那两个目录"（后面的名字/CSV 判据要用）
            page.dir_a_edit()->setText(root + QStringLiteral("/a"));
            page.dir_b_edit()->setText(root + QStringLiteral("/b"));
        }
        //   ② 统计标签：`一致 N | 不一致 N | 仅 {A名} N | 仅 {B名} N`，且**用文件夹名**
        {
            const QString stats = page.stats_text();
            check(stats == QStringLiteral("一致 2  |  不一致 1  |  仅 a 1  |  仅 b 0"),
                  QStringLiteral("⑤ 统计标签逐字段对得上夹具（实得「%1」）").arg(stats));
            check(stats.contains(QStringLiteral("仅 a ")) &&
                      stats.contains(QStringLiteral("仅 b ")),
                  QStringLiteral("⑤ 统计标签用**文件夹名**（a / b），不是字面量 A/B"));
            check(page.stats_label() != nullptr && page.stats_label()->text() == stats,
                  QStringLiteral("⑤ 统计标签真的挂在界面上"));
        }
        //   ③ CSV：生成时间 = **真实时间**（不是 1970）；统计行用文件夹名
        {
            const QString csv2 = root + QStringLiteral("/out_auto.csv");
            check(page.export_csv_auto(csv2), QStringLiteral("⑤ CSV 自动时间导出成功"));
            QFile file(csv2);
            QString body;
            if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            const QStringList lines = body.split(QStringLiteral("\r\n"));
            const QString stamp_line =
                lines.size() > 1 ? lines[1] : QString();   // "生成时间: yyyy-MM-dd HH:mm:ss"
            const QString stamp = stamp_line.section(QStringLiteral(": "), 1);
            const QDateTime parsed = QDateTime::fromString(stamp, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            check(!stamp.isEmpty() && !stamp.startsWith(QStringLiteral("1970")) && parsed.isValid() &&
                      qAbs(parsed.secsTo(QDateTime::currentDateTime())) < 3600,
                  QStringLiteral("⑤ CSV 生成时间 = 真实时间且可解析（实得「%1」）").arg(stamp));
            check(body.contains(QStringLiteral("一致: 2 | 不一致: 1 | 仅 a: 1 | 仅 b: 0")),
                  QStringLiteral("⑤ CSV 统计行用文件夹名（含「仅 a: 1 | 仅 b: 0」）"));
        }
        //   ④ ①（取消）：compare_dirs 在每个文件之前轮询 ⇒ 停在文件边界、行数＝已比较数
        {
            std::atomic<int> seen{0};
            const int stopped =
                page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                                  QStringLiteral("auto"), QStringLiteral("auto"),
                                  [&seen]() { return seen.fetch_add(1) >= 2; });
            check(page.last_compare_cancelled() && stopped == 2 && page.last_compare_total() == 4,
                  QStringLiteral("① XML 取消：停在文件边界（已比较 %1 / 共 %2，cancelled=%3）")
                      .arg(stopped)
                      .arg(page.last_compare_total())
                      .arg(page.last_compare_cancelled() ? 1 : 0));
            // 恢复：重跑一次不取消，后面的判据不受影响
            page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                              QStringLiteral("auto"), QStringLiteral("auto"));
        }
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

    // ★★ 2026-10-06（① 用户拍板「**插件的架构Bug，那必须修啊。**」）：两类槽位的**消费端**
    //   修前（本块就是它的锁）：
    //     · `context`（右键菜单）槽**根本没渲染** —— 全 2.x 源码搜 `"context"` 只命中自检断言，
    //       插件注册的右键命令哪儿都不出现；
    //     · `option_entry` 的下拉虽然建出来了（`hub.cpp` 把它塞进操作栏），但指针只在
    //       `render_plugin_slots` 的**局部 QHash** 里 ⇒ 插件加的下拉条目"只显示、不生效"
    //       （页面读不到它的值）。
    //   锁的形态（两处都能判红）：
    //     ① 页面侧 `plugin_option_value(area)` 必须返回插件条目的当前值 —— 拆掉 Hub 的
    //        `set_plugin_option_combo` 登记就红；
    //     ② 包列表**真右键路径**（`customContextMenuRequested`）弹出的那个菜单里必须有插件命令、
    //        且**点它真的调回插件**（走既有 `CommandReg`/`Registry::invoke`，没有新 ABI）；
    //        `when={"loaded":"1"}` 在没加载任何包时**不许**出现（＝`when` 按当前状态**重算**，
    //        不是构造时冻结的死条件）。
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots_ctx"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        // 静态：`stk_command_fn` 是**裸函数指针**（不能捕获局部变量）⇒ 次数只能记在静态上
        static int ctx_clicks = 0;
        ctx_clicks = 0;
        plug::PluginApiAdapter adapter(hub.registry(), "slots_ctx.dll", nullptr);
        const auto on_ctx = +[](void*) { ++ctx_clicks; };
        // when={"loaded": "1"}（C ABI 的 when 值是字符串；1.x 写的是 True，见 abi.h 的 stk_when）
        const char* when_keys[] = {"loaded"};
        const char* when_values[] = {"1"};
        stk_when when;
        when.keys = when_keys;
        when.values = when_values;
        when.count = 1;
        when.fn = nullptr;
        when.user = nullptr;
        const char* cid = adapter.api().register_command(adapter.api().host, "ctx_action",
                                                         "插件右键动作", on_ctx, nullptr, &when);
        check(cid != nullptr, QStringLiteral("① context：插件命令注册失败"));
        check(adapter.api().register_menu_item(adapter.api().host, "fs", "context", cid, 0) == 1,
              QStringLiteral("① context：fs 的 context 挂载点注册失败"));
        check(adapter.api().register_option_entry(adapter.api().host, "fs", "top_bar", "插件下拉值",
                                                  nullptr) == 1,
              QStringLiteral("① option_entry：下拉条目注册失败"));
        hub.render_plugin_slots();

        FsPage* fs = nullptr;
        for (int i = 0; i < hub.page_count(); ++i) {
            if (auto* candidate = dynamic_cast<FsPage*>(hub.page_at(i))) fs = candidate;
        }
        check(fs != nullptr, QStringLiteral("① 找不到 fs 页（Hub 没造出来？）"));
        if (fs != nullptr) {
            // ①-a **页面侧的取值入口**：插件下拉的"当前值"页面读得到（修前恒为空串）
            const QString value = fs->plugin_option_value(QStringLiteral("top_bar"));
            check(value == QStringLiteral("插件下拉值"),
                  QStringLiteral("① option_entry：页面读不到插件下拉的值（实得「%1」）").arg(value));
            // 反向：没登记过的 area 必须读成空 —— 证明这不是"恒定返回某个值"的假入口
            check(fs->plugin_option_value(QStringLiteral("nope")).isEmpty(),
                  QStringLiteral("① option_entry：不存在的 area 必须读成空"));

            // ①-b context：**没加载任何包**（loaded=0）⇒ when 不满足 ⇒ 菜单里一条插件项都没有
            QMenu denied(fs);
            const int added_denied = fs->append_plugin_context_items(&denied, fs->plugin_context());
            check(added_denied == 0 && denied.actions().isEmpty(),
                  QStringLiteral("① context：loaded=0 时不许出现插件项（实得 %1 条 / 菜单 %2 项）")
                      .arg(added_denied)
                      .arg(denied.actions().size()));

            // 加载一个源 ⇒ loaded=1 ⇒ when 满足 ⇒ 出现，且**点它真的调回插件**
            fs->add_source(QStringLiteral("xrkit_smoke_ctx.db"), {});
            check(fs->loaded_source_count() == 1,
                  QStringLiteral("① context：夹具没把包加载起来（loaded=%1）")
                      .arg(fs->loaded_source_count()));
            QMenu allowed(fs);
            const int added_allowed = fs->append_plugin_context_items(&allowed, fs->plugin_context());
            QAction* plugin_action = nullptr;
            for (QAction* act : allowed.actions()) {
                if (act->text() == QStringLiteral("插件右键动作")) plugin_action = act;
            }
            check(added_allowed == 1 && plugin_action != nullptr,
                  QStringLiteral("① context：loaded=1 时插件命令必须出现在菜单里（实得 %1 条）")
                      .arg(added_allowed));
            if (plugin_action != nullptr) plugin_action->trigger();
            check(ctx_clicks == 1,
                  QStringLiteral("① context：点菜单项必须把命令**回传插件**（既有 CommandReg/invoke；"
                                 "实测调用 %1 次）")
                      .arg(ctx_clicks));

            // ①-c **真右键路径**：`customContextMenuRequested` 弹出的那个菜单里也得有它
            //   （证明页面的右键处理器**真的调了** `append_plugin_context_items`；
            //    只测基类方法的话，"页面根本没接线"这种漏法测不出来）。
            QStringList seen;
            QTimer::singleShot(0, [&seen, fs]() {
                QMenu* shown = fs->last_package_context_menu();
                if (shown == nullptr) return;
                for (QAction* act : shown->actions()) seen << act->text();
                shown->close();   // 让 `menu.exec()` 的嵌套事件循环退出来（自检不许卡住）
            });
            emit fs->package_list()->customContextMenuRequested(QPoint(4, 4));
            check(seen.contains(QStringLiteral("插件右键动作")),
                  QStringLiteral("① context：真右键路径的菜单里没有插件命令（实得 [%1]）")
                      .arg(seen.join(QStringLiteral("/"))));

            // ①-d 1.x 口径的**默认槽位**：`location` 留空（C ABI 里 NULL）＝ `"context"`
            //   （`toolkit_plugins.py:361` 的 `location="context"`）。修前 NULL 被当非法槽位拒掉。
            const char* cid2 = adapter.api().register_command(adapter.api().host, "ctx_default",
                                                              "插件右键动作2", on_ctx, nullptr,
                                                              nullptr);
            check(cid2 != nullptr &&
                      adapter.api().register_menu_item(adapter.api().host, "fs", nullptr, cid2, 1) ==
                          1,
                  QStringLiteral("① context：`location` 留空必须落到 1.x 的默认值 context"));
            QMenu defaults(fs);
            const int added_defaults =
                fs->append_plugin_context_items(&defaults, fs->plugin_context());
            bool second_ok = false;
            for (QAction* act : defaults.actions()) {
                if (act->text() == QStringLiteral("插件右键动作2")) second_ok = true;
            }
            check(added_defaults == 2 && second_ok,
                  QStringLiteral("① context：默认槽位的命令也要在菜单里（实得 %1 条 / 第 2 条 %2）")
                      .arg(added_defaults)
                      .arg(second_ok ? 1 : 0));
        }
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
            // 条目列表：★**2026-10-01 与用户两张截图对齐后的口径**（他说"一个正常一个屎"：
            //   封包页＝两行控件 + 一个列表填满；解包页当时是四条横带 + 两个空框）——
            //   **没条目时把整个条目面板（工具行 + 树）收掉**，高度全给包列表 ⇒ 两页同形；
            //   有货时面板才出现、竖向分隔条才生效（下面那条断言接着验它出现）。
            check(empty_page.entry_pane() != nullptr && empty_page.entry_pane()->isHidden(),
                  QStringLiteral("空状态：条目面板整块收起，包列表填满工作区（与封包页同形）"));

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

        // ★ 2026-10-04：`set_registry` 现在**同时**把这份注册表交给引擎（活动钩子）。
        //   本用例里的注册表是**局部对象**，出块就死 ⇒ 必须在这里收回钩子（否则下一个用例
        //   读到的是野指针）。这一条本身也是判据：钩子不许悬空。
        plug::set_active_registry(nullptr);
        check(plug::active_registry() == nullptr,
              QStringLiteral("⑫12.1 自检用完活动注册表钩子必须复位（不许悬空）"));
    }

    // ★★ 2026-10-04 **引擎层解密闸门**（用户口径原话：「GUI 的功能，CLI 肯定要全部拥有啊。
    //   哪有 GUI 的细节功能比 CLI 还多的。」）——
    //   修前的违例：闸门只在外壳（`FsPage::gate_source`）里，CLI 直接调引擎、动作路径**从不建
    //   注册表** ⇒ 加密包被 CLI 当普通包读（同一引擎、两个前端两套行为）。
    //   这一组锁的是"闸门真的搬进了**引擎读入口**"（`package_entries` / `list_db_seek`）：
    //     ① 有活动注册表且命中 ⇒ 拒绝，理由**逐字等于** `gate_db_path` 给的那一条
    //        （GUI 显示的就是它）—— 把引擎里那句 `gate_active_read` 去掉立刻红；
    //     ② 命中且解得开 ⇒ 真的读**解密产物**（条目名换成明文里的那个）；
    //     ③ 显式传入的 registry 优先于活动钩子；
    //     ④ 钩子复位（`nullptr`）后条目表与"从来没有过钩子"**逐值一致**（默认行为不许被改）；
    //     ⑤ 闸门不二次解密自己的产物（外壳先过一遍、引擎再过一遍时必须认出来）。
    {
        namespace plug = xrkit::plugin;
        namespace efs = xrkit::engine::fs;
        std::error_code ec;
        const std::filesystem::path work =
            std::filesystem::temp_directory_path(ec) / "xrkit_smoke_engine_gate";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work, ec);
        // ① 夹具：两枚**真的** X-Ray DB 包（引擎写侧产物）——一枚"加密包"、一枚"明文"
        const QString pkg = QString::fromStdString((work / "cipher.stkenc.db").string());
        const QString plain_db = QString::fromStdString((work / "plain.db").string());
        const auto write_db = [](const QString& path, const std::string& name) {
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem item;
            item.path = name;
            item.data = {static_cast<uint8_t>('X')};
            items.push_back(item);
            const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
            std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
            return raw.size() > 0;
        };
        const bool built = write_db(pkg, "a.txt") && write_db(plain_db, "plain.txt");
        check(built, QStringLiteral("引擎闸门夹具：造不出测试 DB 包"));

        // 条目表的**指纹**（path/is_dir/size 三样都算）——"逐值一致"要拿它比，不靠条数
        const auto fingerprint = [](const std::vector<xrkit::engine::fs::EntryInfo>& entries) {
            std::string text;
            for (const xrkit::engine::fs::EntryInfo& e : entries) {
                text += e.path + "\t" + (e.is_dir ? "d" : "f") + "\t" +
                        std::to_string(e.size) + "\n";
            }
            return text;
        };
        const std::string pkg_utf8 = pkg.toStdString();
        std::string error;

        // ④ 基线：**从来没有过钩子**时读出来的条目表（后面复位后必须与它逐值一致）
        const std::vector<xrkit::engine::fs::EntryInfo> baseline =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        const std::string baseline_print = fingerprint(baseline);
        check(error.empty() && baseline.size() == 1 && baseline[0].path == "a.txt",
              QStringLiteral("引擎闸门基线：裸读应得 a.txt 一条（err=%1，实得 %2 条）")
                  .arg(QString::fromStdString(error))
                  .arg(baseline.size()));

        // ② 一枚"命中却解不开"的解密器
        plug::Registry reject_reg;
        reject_reg.register_decryptor("smoke_gate.dll", [](const std::string&) { return true; },
                                      [](const std::string&, std::string&) { return false; });
        const plug::DecryptGateResult expected_gate =
            plug::gate_db_path(reject_reg, pkg_utf8, plug::decrypt_output_dir());
        check(!expected_gate.ok && !expected_gate.error.empty(),
              QStringLiteral("引擎闸门夹具：gate_db_path 没给出拒绝理由"));
        plug::set_active_registry(&reject_reg);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> gated =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(gated.empty() && error == expected_gate.error,
              QStringLiteral("引擎闸门没生效：`package_entries` 应拒绝并给**同一条**理由"
                             "（实得 %1 条；理由 %2）")
                  .arg(gated.size())
                  .arg(QString::fromStdString(error)));
        // 同一条闸门也在 `--dump`/`--extract` 走的那个入口上（CLI 黑盒判据的引擎侧一半）
        const xrkit::engine::fs::DbListing also_gated =
            xrkit::engine::fs::list_db_seek(pkg_utf8, std::nullopt);
        check(!also_gated.ok && also_gated.error == expected_gate.error,
              QStringLiteral("引擎闸门没覆盖 `list_db_seek`（--dump 走的入口）：%1")
                  .arg(QString::fromStdString(also_gated.error)));

        // ③ 显式传入的 registry 优先：活动钩子里那枚会拒，但显式传的这枚没有解密器 ⇒ 放行
        //   （★ 这条自检第一次跑就抓出一个真缺陷：`package_entries` 把显式 registry 丢了，
        //     内层 `list_db_seek` 又去问全局钩子 ⇒ 上面那次"放行"被内层推翻。修法是把同一份
        //     registry **逐层传下去**。）
        plug::Registry empty_reg;
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> explicit_ok =
            efs::package_entries(pkg_utf8, std::nullopt, error, &empty_reg);
        check(error.empty() && fingerprint(explicit_ok) == baseline_print,
              QStringLiteral("显式传入的 registry 没有优先于活动钩子（实得 %1 条 / err=%2）")
                  .arg(explicit_ok.size())
                  .arg(QString::fromStdString(error)));
        const xrkit::engine::fs::DbListing explicit_seek =
            xrkit::engine::fs::list_db_seek(pkg_utf8, std::nullopt, &empty_reg);
        check(explicit_seek.ok && explicit_seek.entries.size() == 1,
              QStringLiteral("显式传入的 registry 没传进 `list_db_seek`（ok=%1 err=%2）")
                  .arg(explicit_seek.ok ? 1 : 0)
                  .arg(QString::fromStdString(explicit_seek.error)));

        // ⑤ 命中且解得开 ⇒ 读的必须是**解密产物**（明文里那枚条目），且**不二次解密**自己的产物
        int decrypt_calls = 0;
        plug::Registry ok_reg;
        {
            std::ifstream in(plain_db.toStdString(), std::ios::binary);
            const std::string plain((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            ok_reg.register_decryptor(
                "smoke_gate_ok.dll", [](const std::string&) { return true; },
                [&decrypt_calls, plain](const std::string&, std::string& out) {
                    ++decrypt_calls;
                    out = plain;
                    return true;
                });
        }
        plug::set_active_registry(&ok_reg);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> decrypted =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(error.empty() && decrypted.size() == 1 && decrypted[0].path == "plain.txt",
              QStringLiteral("闸门命中且解得开时应读**明文**（期望 plain.txt，实得 %1 条 first=%2）")
                  .arg(decrypted.size())
                  .arg(decrypted.empty() ? QStringLiteral("<空>")
                                         : QString::fromStdString(decrypted[0].path)));
        check(decrypt_calls == 1, QStringLiteral("解密回调应恰好被调 1 次（实测 %1）")
                                      .arg(decrypt_calls));
        // 产物落点：必须落在**引擎那一份**目录里（外壳从同一处取值，才认得出"这是产物"）
        const QString decrypted_path =
            QDir(QString::fromStdString(plug::decrypt_output_dir()))
                .filePath(QStringLiteral("cipher.stkenc.db.decrypted"));
        check(QFileInfo::exists(decrypted_path),
              QStringLiteral("解密产物没落在引擎口径的目录里：%1").arg(decrypted_path));
        // 外壳先过 `gate_source`（产物入目录）再交给引擎 ⇒ 引擎那道闸门必须**认出来**、不二次解密
        if (QFileInfo::exists(decrypted_path)) {
            const int before = decrypt_calls;
            error.clear();
            const std::vector<xrkit::engine::fs::EntryInfo> again =
                efs::package_entries(decrypted_path.toStdString(), std::nullopt, error);
            check(error.empty() && fingerprint(again) == fingerprint(decrypted) &&
                      decrypt_calls == before,
                  QStringLiteral("闸门对自己的产物二次解密了（回调 %1→%2 次 / err=%3）")
                      .arg(before)
                      .arg(decrypt_calls)
                      .arg(QString::fromStdString(error)));
        }

        // ④ 复位 ⇒ 与基线**逐值一致**（默认行为不许被这段新代码顺手改掉）
        plug::set_active_registry(nullptr);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> after_reset =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(error.empty() && fingerprint(after_reset) == baseline_print,
              QStringLiteral("钩子复位后条目表与今天不一致（实得 %1 条 / err=%2）")
                  .arg(after_reset.size())
                  .arg(QString::fromStdString(error)));
        check(plug::active_registry() == nullptr,
              QStringLiteral("引擎闸门用例：钩子用完必须复位"));

        std::filesystem::remove_all(work, ec);
    }

    // ★★ 2026-10-04 **CLI 过闸门**（黑盒：真的起 `xrkit_cli.exe`）——
    //   锁的是"CLI 的动作路径也建注册表、并把引擎的拒绝理由如实打出来 + 非零退出"。
    //   判据用的插件是本构建产出的 `xrkit_test_plugin.dll`（纯 C；它注册了一枚判据为
    //   "文件名含 `.stkenc`" 且**解密必失败**的解密器）。
    //   把 CLI 里"动作分派前建注册表"那段删掉 ⇒ 第②条立刻红。
    {
        namespace plug = xrkit::plugin;
        namespace efs = xrkit::engine::fs;
        // 找 `xrkit_cli.exe` 与插件夹具。**两种布局**都要能活：
        //   · 构建树：`<build>/src/shell/xrkit_app.exe` ⇒ `../../tools/cli/`（自检/门禁跑的就是它）；
        //   · 部署包：`scripts/package.ps1` 的舞台**只放 xrkit_app.exe + Qt 运行时**
        //     （`dist\<版本>\<标记>\<序号>\`）⇒ 那里根本没有 CLI。
        //   找不到 ⇒ **如实记 note 跳过**（不是"通过"）；找到了 ⇒ 三条判据全跑，删掉 CLI 里
        //   "动作分派前建注册表"那段就当场红。★ 第一版把"找不到"写成硬红，结果**打包自检**
        //   在部署布局里红掉（打包直接失败）—— 布局不适用 ≠ 判据失败。
        const QString app_dir = QCoreApplication::applicationDirPath();
        QString cli;
        for (const QString& candidate :
             {app_dir + QStringLiteral("/xrkit_cli.exe"),
              QDir::cleanPath(app_dir + QStringLiteral("/../../tools/cli/xrkit_cli.exe"))}) {
            if (QFileInfo::exists(candidate)) {
                cli = candidate;
                break;
            }
        }
        QString plugin_dll;
        if (!cli.isEmpty()) {
            const QString cli_dir = QFileInfo(cli).absolutePath();
            for (const QString& candidate :
                 {cli_dir + QStringLiteral("/xrkit_test_plugin.dll"),
                  app_dir + QStringLiteral("/xrkit_test_plugin.dll")}) {
                if (QFileInfo::exists(candidate)) {
                    plugin_dll = candidate;
                    break;
                }
            }
        }
        if (cli.isEmpty() || plugin_dll.isEmpty()) {
            result.notes.append(
                QStringLiteral("CLI 闸门黑盒判据：这个布局里找不到 xrkit_cli.exe / "
                               "xrkit_test_plugin.dll（app_dir=%1）⇒ 本用例跳过（构建树/门禁里会真跑）")
                    .arg(app_dir));
        } else {
            const QString plugin_dir = QFileInfo(plugin_dll).absolutePath();
            std::error_code ec;
            const std::filesystem::path work =
                std::filesystem::temp_directory_path(ec) / "xrkit_smoke_cli_gate";
            std::filesystem::remove_all(work, ec);
            std::filesystem::create_directories(work, ec);
            // 一枚**真的** DB 包，文件名带 `.stkenc`（插件那条解密器的判据）
            const QString enc_db = QString::fromStdString((work / "cli.stkenc.db").string());
            {
                std::vector<efs::DbPackItem> items;
                efs::DbPackItem item;
                item.path = "cli.txt";
                item.data = {static_cast<uint8_t>('C')};
                items.push_back(item);
                const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
                std::ofstream out(enc_db.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            const auto run_cli = [](const QString& exe, const QStringList& argv) {
                QProcess process;
                process.setProcessChannelMode(QProcess::MergedChannels);
                process.start(exe, argv);
                const bool finished = process.waitForFinished(120000);
                return std::make_pair(static_cast<int>(finished ? process.exitCode() : -1),
                                      QString::fromUtf8(process.readAllStandardOutput()));
            };

            // ① **默认（不带 --plugins）照旧放行** —— 这就是"NLC 那一类"在本机的现状：
            //    CLI 的 plugins/ 常为空 ⇒ 有注册表但无解密器 ⇒ 与 GUI 现状一致（不许被这次改动打破）
            {
                const auto [rc, out] = run_cli(cli, {QStringLiteral("--dump"), enc_db});
                check(rc == 0 && out.contains(QStringLiteral("DUMP ok")) &&
                          out.contains(QStringLiteral("entries=1")),
                      QStringLiteral("CLI 默认（无插件）必须照旧放行（exit=%1 out=%2）")
                          .arg(rc)
                          .arg(out.trimmed().right(120)));
            }
            // ② `--plugins <目录>` ⇒ 闸门拒绝 + **同一条理由** + 非零退出
            {
                const auto [rc, out] = run_cli(
                    cli, {QStringLiteral("--plugins"), plugin_dir, QStringLiteral("--dump"), enc_db});
                // 期望文案：本进程用**同一个**插件 DLL 装出的注册表问一遍 `gate_db_path`
                //（GUI 显示的就是这条；"同一条文案"要逐字比，不许各写一份）
                plug::Registry mirror;
                plug::PluginApiAdapter adapter(mirror, "xrkit_test_plugin.dll", nullptr);
                const plug::LoadResult loaded = plug::load_plugin(plugin_dll.toStdString(),
                                                                  adapter.api());
                new plug::PluginApiAdapter(std::move(adapter));   // 回调活着要一直有效
                const plug::DecryptGateResult want =
                    plug::gate_db_path(mirror, enc_db.toStdString(), plug::decrypt_output_dir());
                const QString want_text = QString::fromStdString(want.error);
                check(rc != 0 && !out.contains(QStringLiteral("DUMP ok")) && want_text.size() > 0 &&
                          out.contains(want_text),
                      QStringLiteral("CLI 装了插件后必须被闸门拦下并给**同一条**理由"
                                     "（exit=%1；期望含 %2；实得 %3）")
                          .arg(rc)
                          .arg(want_text)
                          .arg(out.trimmed().right(160)));
                check(loaded.ok, QStringLiteral("CLI 闸门判据：本进程装测试插件失败：%1")
                                     .arg(QString::fromStdString(loaded.error)));
            }
            // ③ 插件目录不存在 ⇒ 只记错、**不阻塞**（与 GUI 的启动口径一致）
            {
                const QString ghost =
                    QString::fromStdString((work / "no_such_plugins").string());
                const auto [rc, out] =
                    run_cli(cli, {QStringLiteral("--plugins"), ghost, QStringLiteral("--dump"),
                                  enc_db});
                check(rc == 0 && out.contains(QStringLiteral("entries=1")) &&
                          out.contains(QStringLiteral("PLUGINS dir=")),
                      QStringLiteral("插件目录不存在不许阻塞动作（exit=%1 out=%2）")
                          .arg(rc)
                          .arg(out.trimmed().right(160)));
            }
            std::filesystem::remove_all(work, ec);
        }
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
        // 8.4：自动安装缺件时如实报（不联网、不静默自救）；用户口径"依赖自带"之后，程序目录里
        //      可能已经有随包自带的 ffmpeg.exe ⇒ 那也是一条**如实结论**
        const QString install = page.auto_install();
        check(install.contains(QStringLiteral("download_ffmpeg.py")) ||
                  install.contains(QStringLiteral("自带")) ||
                  install.contains(QStringLiteral("不可用")) ||
                  install.contains(QStringLiteral("ffmpeg 8.x")),
              QStringLiteral("⑧8.4 自动安装按钮给出如实结论（%1）").arg(install.left(48)));
        // 8.7：手动指定不存在的文件必须被拒（且不写盘）
        QString error;
        const QString ghost = QDir::temp().filePath(QStringLiteral("xrkit_nope_ffmpeg.exe"));
        check(!page.set_manual(ghost, QString(), &error) && !error.isEmpty(),
              QStringLiteral("⑧8.7 手动指定不存在的文件被拒"));
    }

// ═══════════════════════════════════════════════════════════════════════════════════════
// 视频页**补全**（本轮）的自检：引擎早就绪，缺的全是界面接线 ⇒ 每条锁都判"接线真的接上了"。
//   · 两条**单文件**输入行（源 + 参考 OGM）+ 输出行，三行都是整行拖放区；
//   · 「转换为 OGM」主按钮：未选源 ⇒ 灰；参考没落地 ⇒ 灰；两条都落地 ⇒ 亮（1.x `:574-577`/`:708`）；
//   · 输出＝**文件**：默认名按 1.x `_auto_output` 三条规则推导，选择走**保存文件**对话框；
//   · 进度条 + 文案接引擎 `progress_from_line`；
//   · **取消**＝终止进程 + 删半成品，且"删没删掉"要**可见**（用户 2026-10-04：「不能静默报错」）；
//   · 「视频参数」面板＝1.x `_refresh_display` 的字段，值取自引擎 `TranscodePlan`（不是页面自己判阈值）。
//
// ★ 两类锁**分开**（这是打包口径逼出来的）：
//   · "不需要夹具"的一类（结构/置灰/面板/默认名/对话框语义/缺失报路径/自动安装/真 ffprobe 链路）
//     在**任何**环境都要跑 —— 发行包里没有夹具，但[包内自检]也得跑得绿；
//   · "夹具驱动"的一类（进度/取消删半成品/失败带 stderr/跳过计数）要**真起进程**，用的是
//     构建产物 `xrkit_fake_ffmpeg.exe`；它在发行包里**故意不带** ⇒ 缺了要**如实出 note 跳过**
//     （不静默、也不误判成失败）。
// ═══════════════════════════════════════════════════════════════════════════════════════
{
    namespace evid = xrkit::engine::video;
    const QString fake =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("xrkit_fake_ffmpeg.exe"));
    const bool have_fake = QFileInfo::exists(fake);
    if (!have_fake) {
        result.notes.append(QStringLiteral(
            "视频页夹具锁：exe 旁边没有 xrkit_fake_ffmpeg.exe（发行包不带夹具）⇒ 进度/取消/失败/"
            "跳过那几条**夹具驱动的**锁跳过；结构/置灰/面板/默认名/对话框/缺失报路径/真 ffprobe 照跑"));
    }

    // 自检用页面：把"ffmpeg 从哪来"与"两个文件对话框"换成可控实现（三点都是**虚拟缝**，
    // 生产路径与自检走同一份接线 ⇒ 判的是接线，不是桩）。
    class SelfCheckVideoPage : public VideoPage {
    public:
        SelfCheckVideoPage() : VideoPage(nullptr) {}
        evid::FfmpegPaths paths;
        QString open_title, open_filter, open_start, open_pick;
        QString save_start, save_initial, save_pick;
        int open_calls = 0, save_calls = 0;

    protected:
        evid::FfmpegPaths resolve_ffmpeg() override { return paths; }

    public:
        QString ask_open_file(const QString& title, const QString& filter,
                              const QString& start_path) override {
            open_title = title;
            open_filter = filter;
            open_start = start_path;
            ++open_calls;
            return open_pick;
        }
        QString ask_save_output(const QString& start_dir, const QString& initial_name) override {
            save_start = start_dir;
            save_initial = initial_name;
            ++save_calls;
            return save_pick;
        }
    };

    QTemporaryDir vt;
    QDir().mkpath(vt.path() + QStringLiteral("/vidsrc"));
    QDir().mkpath(vt.path() + QStringLiteral("/refdir"));
    const QString src_dir2 = vt.path() + QStringLiteral("/vidsrc");
    const QString ref_dir2 = vt.path() + QStringLiteral("/refdir");
    const QString src_file = src_dir2 + QStringLiteral("/intro.avi");
    // 参考**故意不同主名**：1.x `_auto_output` 在"源与参考同主名同目录"时会退成 `.new.ogm`，
    // 那条专测放在下面（否则这条"落参考目录同主名"会一直被防自毁分支吃掉）。
    const QString ref_file = ref_dir2 + QStringLiteral("/ref.ogm");
    { std::ofstream out(src_file.toStdString(), std::ios::binary); out << "SRC"; }
    { std::ofstream out(ref_file.toStdString(), std::ios::binary); out << "REF"; }

    const auto pump = [](int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    };
    const auto make_info = [](bool video, int w, int h, double fps, long long vbr, bool audio,
                              int ch, int ar, long long abr, double duration) {
        evid::MediaInfo info;
        info.parsed = true;
        info.duration = duration;
        info.size_mb = 1.0;
        info.has_video = video;
        if (video) {
            info.video.codec = "theora";
            info.video.width = w;
            info.video.height = h;
            info.video.fps = fps;
            info.video.bitrate = vbr;
        }
        info.has_audio = audio;
        if (audio) {
            info.audio.codec = "vorbis";
            info.audio.channels = ch;
            info.audio.sample_rate = ar;
            info.audio.bitrate = abr;
        }
        return info;
    };
    // 源：320x240@30，有音轨；参考：640x480@25 + 800kbps + 44.1k/2ch ⇒ 两条都该"将…"
    const evid::MediaInfo src_info = make_info(true, 320, 240, 30.0, 0, true, 2, 48000, 160000, 10.0);
    const evid::MediaInfo ref_info = make_info(true, 640, 480, 25.0, 800000, true, 2, 44100, 128000, 12.0);

    SelfCheckVideoPage page;
    page.resize(820, page_min_h());
    page.show();
    QApplication::processEvents();
    page.paths = evid::find_ffmpeg_default();  // 页面自己"按口径"定位到的那份（可能没有）

    // ① 三条**单文件**输入行 + 三条整行拖放区
    check(page.source_edit() != nullptr && page.reference_edit() != nullptr &&
              page.output_edit() != nullptr,
          QStringLiteral("视频①：源 / 参考 OGM / 输出 三条输入行都在"));
    check(page.drop_zones().zone_count() == 3,
          QStringLiteral("视频①：三行各有拖放区（实测 %1）").arg(page.drop_zones().zone_count()));
    check(page.reference_edit()->objectName() == QStringLiteral("video_reference") &&
              page.output_edit()->placeholderText().contains(QStringLiteral("OGM")) &&
              !page.output_edit()->placeholderText().contains(QStringLiteral("目录")),
          QStringLiteral("视频①：输出是**文件**（占位 %1）").arg(page.output_edit()->placeholderText()));

    // ② 主按钮的置灰口径（1.x：未选源灰；参考没落地也灰）
    check(page.convert_button() != nullptr &&
              page.convert_button()->text() == QStringLiteral("转换为 OGM") &&
              !page.convert_button()->isEnabled(),
          QStringLiteral("视频②：未选源时「转换为 OGM」置灰"));
    page.source_edit()->setText(src_file);
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
    check(page.convert_button()->isEnabled() == false,
          QStringLiteral("视频②：源落地、参考没落地 ⇒ 仍灰（1.x：参考必需）"));
    page.reference_edit()->setText(ref_file);
    page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
    check(page.convert_button()->isEnabled(),
          QStringLiteral("视频②：源 + 参考都落地 ⇒ 主按钮亮"));

    // ③「视频参数」面板：三段 + 1.x 的标记字段（值来自引擎的 plan）
    const QString panel = page.info_panel_text();
    check(panel.contains(QStringLiteral("══ 参考 OGM ══")) &&
              panel.contains(QStringLiteral("══ 源文件 ══")) &&
              panel.contains(QStringLiteral("══ 转换参数（匹配参考 OGM） ══")),
          QStringLiteral("视频③：参数面板三段齐（1.x `_refresh_display`）"));
    check(panel.contains(QStringLiteral("30.00 -> 25.00")) &&
              panel.contains(QStringLiteral("将调整")) &&
              panel.contains(QStringLiteral("320x240 -> 640x480")) &&
              panel.contains(QStringLiteral("将缩放")) &&
              panel.contains(QStringLiteral("800 kbps")) &&
              (panel.contains(QStringLiteral("theora -> libtheora")) ||
               panel.contains(QStringLiteral("theora -> mpeg4"))),
          QStringLiteral("视频③：对比行要给出引擎的结论（含将缩放/将调整/编码器）"));
    check(panel.contains(QStringLiteral("  文件: intro.avi")) &&
              panel.count(QStringLiteral("  文件: ")) == 2,
          QStringLiteral("视频③：源/参考各只出一行「文件:」（1.x 的面板形态）"));
    // 同尺寸同帧率 ⇒ 两条都该报 OK（阈值判据在引擎里，页面不许再判一遍）
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source,
                            make_info(true, 640, 480, 25.0, 0, true, 2, 48000, 160000, 10.0),
                            QString());
    const QString same = page.info_panel_text();
    check(same.contains(QStringLiteral("640x480 -> 640x480  [OK]")) &&
              same.contains(QStringLiteral("25.00 -> 25.00  [OK]")) &&
              !same.contains(QStringLiteral("将缩放")) && !same.contains(QStringLiteral("将调整")),
          QStringLiteral("视频③：同尺寸/同帧率 ⇒ [OK]（与 argv 同一判据）"));
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());

    // ④ 输出默认名（1.x `_auto_output` 三条规则）
    const QString want_default =
        QDir::toNativeSeparators(QDir::cleanPath(ref_dir2 + QStringLiteral("/intro.ogm")));
    check(QDir::cleanPath(page.default_output_path()) == QDir::cleanPath(want_default),
          QStringLiteral("视频④：有参考 ⇒ 输出落**参考所在目录**同主名（实得 %1）")
              .arg(page.default_output_path()));
    // 源与参考同目录同主名 ⇒ 默认值会砸到参考文件本身 ⇒ 必须改成 .new.ogm（1.x 的防自毁）
    const QString clash_ref = src_dir2 + QStringLiteral("/intro.ogm");
    { std::ofstream out(clash_ref.toStdString(), std::ios::binary); out << "REF2"; }
    page.reference_edit()->setText(clash_ref);
    page.apply_probe_result(clash_ref, VideoPage::ProbeTarget::Reference, ref_info, QString());
    check(QDir::cleanPath(page.default_output_path()) ==
              QDir::cleanPath(QDir::toNativeSeparators(src_dir2 + QStringLiteral("/intro.new.ogm"))),
          QStringLiteral("视频④：默认输出不许指向参考文件本身（实得 %1）")
              .arg(page.default_output_path()));
    page.reference_edit()->setText(ref_file);
    page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
    page.output_edit()->clear();
    check(QDir::cleanPath(page.default_output_path()) == QDir::cleanPath(want_default),
          QStringLiteral("视频④：默认名只由源+参考决定"));

    // ⑤「选择…」：源/参考是**打开文件**、输出是**保存文件**，起点走 resolve_dialog_start（唯一源目录）
    const std::vector<QWidget*>& zones = page.drop_zones().containers();
    const auto browse_of = [&zones](size_t index) -> QPushButton* {
        return index < zones.size() ? zones[index]->findChild<QPushButton*>() : nullptr;
    };
    check(browse_of(0) != nullptr && browse_of(1) != nullptr && browse_of(2) != nullptr,
          QStringLiteral("视频⑤：三行都有「选择…」按钮"));
    page.open_pick = src_file;
    if (browse_of(0) != nullptr) browse_of(0)->click();
    check(page.open_calls == 1 && page.open_title.contains(QStringLiteral("源视频")) &&
              page.open_filter.contains(QStringLiteral("*.mp4")) &&
              page.source_edit()->text() == QDir::toNativeSeparators(src_file),
          QStringLiteral("视频⑤：源行走**打开文件**对话框（标题 %1）").arg(page.open_title));
    page.open_pick = ref_file;
    if (browse_of(1) != nullptr) browse_of(1)->click();
    check(page.open_title.contains(QStringLiteral("参考 OGM")) &&
              page.open_filter.contains(QStringLiteral("*.ogm")) &&
              page.reference_edit()->text() == QDir::toNativeSeparators(ref_file),
          QStringLiteral("视频⑤：参考行走**打开文件**对话框（过滤器 %1）").arg(page.open_filter));
    reset_last_dialog_start();
    page.output_edit()->clear();
    pump(200);
    page.save_pick = QDir::toNativeSeparators(vt.path() + QStringLiteral("/picked.ogm"));
    if (browse_of(2) != nullptr) browse_of(2)->click();
    check(page.save_calls == 1 && page.save_initial.endsWith(QStringLiteral(".ogm")) &&
              page.output_edit()->text() == page.save_pick,
          QStringLiteral("视频⑤：输出走**保存文件**对话框（默认名 %1）").arg(page.save_initial));
    check(QDir::cleanPath(page.save_start) == QDir::cleanPath(src_dir2) &&
              last_dialog_start_calls() == 1,
          QStringLiteral("视频⑤：保存起点 = **唯一输入所在目录**（%1，走共用函数 %2 次）")
              .arg(page.save_start)
              .arg(last_dialog_start_calls()));

    // ⑥ 真 ffprobe 链路（按 ⑧8.1 能定位到 ffmpeg/ffprobe 时才跑）：
    //    造 1 秒样本 → 页面的"选文件 → 探测 → 面板"这条链 → 面板要出**真**编码/尺寸。
    //    发行包自带 ffmpeg/ffprobe ⇒ 这条在**包内自检**里也成立（＝"不填路径也能用"的判据）。
    {
        const evid::FfmpegPaths real = evid::find_ffmpeg_default();
        if (!real.found || !real.has_probe) {
            result.notes.append(QStringLiteral(
                "视频⑥：本机按口径定位不到 ffmpeg/ffprobe（只认 user.ltx 与程序目录）⇒ "
                "真 ffprobe 链路跳过，属人工验收项"));
        } else {
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_video_page"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString in_path = dir + QStringLiteral("/in.avi");
            QProcess gen;
            gen.start(QString::fromStdString(real.ffmpeg),
                      QStringList{QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                                  QStringLiteral("-i"), QStringLiteral("testsrc=d=1"),
                                  QStringLiteral("-an"), in_path});
            gen.waitForFinished(120000);
            const bool made = gen.exitCode() == 0 && QFile::exists(in_path);
            check(made, QStringLiteral("视频⑥：造 1 秒真样本失败（退出码 %1）").arg(gen.exitCode()));
            if (made) {
                SelfCheckVideoPage real_page;
                real_page.paths = real;
                real_page.source_edit()->setText(in_path);  // 真接线：填框 → textChanged → 真 ffprobe
                QElapsedTimer clock;
                clock.start();
                while (!real_page.source_ready() && clock.elapsed() < 30000) {
                    QCoreApplication::processEvents();
                    QThread::msleep(10);
                }
                check(real_page.source_ready(),
                      QStringLiteral("视频⑥：真 ffprobe 走「选文件→探测」这条链（面板 %1）")
                          .arg(real_page.info_panel_text().left(60)));
                const QString real_panel = real_page.info_panel_text();
                check(real_panel.contains(QStringLiteral("320x240")) &&
                          (real_panel.contains(QStringLiteral("mpeg4")) ||
                           real_panel.contains(QStringLiteral("h264"))),
                      QStringLiteral("视频⑥：面板要显示真 ffprobe 给的尺寸/编码（%1）")
                          .arg(real_panel.left(80)));
            }
            QDir(dir).removeRecursively();
        }
    }

    // ── 以下几条要**真起进程**（夹具当 ffmpeg/ffprobe）；发行包不带夹具 ⇒ 缺了就 note 跳过 ──
    if (have_fake) {
        // ⑦ 按钮 → argv → 进程 → 进度 → 跑完收干净
        page.paths.found = true;
        page.paths.has_probe = true;
        page.paths.ffmpeg = fake.toStdString();
        page.paths.ffprobe = fake.toStdString();
        page.apply_file(src_file, false);  // 起编码器扫描（夹具的 `-encoders`）
        {
            QElapsedTimer clock;
            clock.start();
            while (!page.encoders_scanned() && clock.elapsed() < 15000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
        }
        check(page.encoders_scanned(),
              QStringLiteral("视频⑦：编码器表要扫到结论（含「扫不到」也要有结论）"));
        // 编码器表有结论之后，面板里的编码器必须是**引擎挑的那个**
        page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
        page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
        pump(200);
        check(page.info_panel_text().contains(QStringLiteral("theora -> libtheora")),
              QStringLiteral("视频⑦：夹具编码器表在 ⇒ 面板报 libtheora（%1）")
                  .arg(page.info_panel_text().left(80)));
        const QString out_file = vt.path() + QStringLiteral("/out.ogm");
        QFile::remove(out_file);
        page.output_edit()->setText(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "finish");
        QElapsedTimer vclock;
        vclock.start();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (!page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.transcode_running(),
              QStringLiteral("视频⑦：点「转换为 OGM」要真的起进程（路径＝夹具）"));
        while (page.progress_percent() == 0 && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.progress_percent() > 0 && page.progress_bar() != nullptr &&
                  page.progress_bar()->value() == page.progress_percent() &&
                  page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("%")),
              QStringLiteral("视频⑦：进度条/文案跟着 ffmpeg 的 time= 走（实得 %1）")
                  .arg(page.progress_percent()));
        while (page.transcode_running() && vclock.elapsed() < 60000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(!page.transcode_running() && page.progress_percent() == 100 &&
                  page.progress_bar() != nullptr && page.progress_bar()->value() == 100 &&
                  page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("完成")),
              QStringLiteral("视频⑦：跑完进度到 100、状态报「完成」"));
        check(QFile::exists(out_file) && QFileInfo(out_file).size() > 0,
              QStringLiteral("视频⑦：产物存在（夹具写的）"));
        check(page.convert_button() != nullptr && page.convert_button()->isEnabled() &&
                  page.cancel_button() != nullptr && !page.cancel_button()->isVisible(),
              QStringLiteral("视频⑦：跑完主按钮恢复、取消收起"));
        {
            QElapsedTimer wait;
            wait.start();
            while (!page.info_panel_text().contains(QStringLiteral("══ 输出文件（本次转换结果） ══")) &&
                   wait.elapsed() < 10000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
            check(page.info_panel_text().contains(QStringLiteral("══ 输出文件（本次转换结果） ══")) &&
                      page.info_panel_text().contains(QStringLiteral("vorbis")),
                  QStringLiteral("视频⑦：转换完成后**再解析一次产物**并贴到面板（1.x）"));
        }

        // ⑧ 取消：真停 + **删半成品** + 理由可见（用户红线：不能静默）
        QFile::remove(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "loop");
        vclock.restart();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (!page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        while ((page.progress_percent() == 0 || !QFile::exists(out_file)) &&
               vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.transcode_running() && page.progress_percent() > 0 && QFile::exists(out_file),
              QStringLiteral("视频⑧：取消前——进程在跑、进度在走、半成品已产生"));
        check(page.cancel_button() != nullptr && page.cancel_button()->isVisible(),
              QStringLiteral("视频⑧：跑起来时取消按钮出现在主按钮左边"));
        if (page.cancel_button() != nullptr) page.cancel_button()->click();
        vclock.restart();
        while (page.transcode_running() && vclock.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(!page.transcode_running(), QStringLiteral("视频⑧：取消要**真停**（进程没了）"));
        check(!QFile::exists(out_file),
              QStringLiteral("视频⑧：取消要**删掉半成品**（1.x：不留「看着像成功」的输出）"));
        const QString cancel_msg = page.last_error();
        check(cancel_msg.contains(QStringLiteral("已取消")) &&
                  cancel_msg.contains(QStringLiteral("已删除半成品")) &&
                  cancel_msg.contains(QFileInfo(out_file).fileName()),
              QStringLiteral("视频⑧：取消理由要可见（含文件名）——实得 %1").arg(cancel_msg));
        check(page.progress_percent() == 0 && page.progress_bar() != nullptr &&
                  page.progress_bar()->value() == 0 && page.progress_label() != nullptr &&
                  page.progress_label()->text() == cancel_msg &&
                  page.convert_button() != nullptr && page.convert_button()->isEnabled() &&
                  page.cancel_button() != nullptr && !page.cancel_button()->isVisible(),
              QStringLiteral("视频⑧：取消后进度归零、文案＝取消理由、按钮恢复"));

        // ⑨ 失败：**ffmpeg 的 stderr 原文**要带出来（不能只给"失败"）
        QFile::remove(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "fail");
        vclock.restart();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.last_error().contains(QStringLiteral("退出码")) &&
                  page.last_error().contains(QStringLiteral("boom-invalid-input")),
              QStringLiteral("视频⑨：失败理由要带 stderr 原文（实得 %1）")
                  .arg(page.last_error().left(90)));
        check(page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("失败")) &&
                  page.info_panel_text().contains(QStringLiteral("错误原文")),
              QStringLiteral("视频⑨：失败在状态栏与面板上都可见"));
        qunsetenv("XRKIT_FAKE_FFMPEG_MODE");

        // ⑫ 跳过要**计数 + 逐条原因**（用户红线）
        {
            SelfCheckVideoPage busy;
            busy.resize(820, page_min_h());
            busy.paths = page.paths;
            busy.apply_file(src_file, false);   // 起进程（异步）
            busy.apply_file(ref_file, true);    // 忙 ⇒ 跳过并留痕
            check(busy.skipped_count() >= 1 && !busy.skip_reasons().isEmpty(),
                  QStringLiteral("视频⑫：跳过要计数（%1 项）").arg(busy.skipped_count()));
            check(busy.info_panel_text().contains(QStringLiteral("跳过")),
                  QStringLiteral("视频⑫：跳过要逐条可见（面板）"));
            pump(500);
        }
    }

    // ⑩ 找不到 ffmpeg：要说明**试过哪些路径**（不能只灰着、不能只给一行泛泛的话）
    {
        SelfCheckVideoPage bare;  // paths 全空 ⇒ resolve_ffmpeg 报"没找到"
        bare.resize(820, page_min_h());
        bare.source_edit()->setText(src_file);
        bare.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
        bare.reference_edit()->setText(ref_file);
        bare.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
        check(bare.convert_button() != nullptr && bare.convert_button()->isEnabled(),
              QStringLiteral("视频⑩：ffmpeg 缺失**不是**主按钮置灰的理由（灰＝说不清）"));
        if (bare.convert_button() != nullptr) bare.convert_button()->click();
        const QString miss = bare.last_error();
        check(miss.contains(QStringLiteral("找不到 ffmpeg")) &&
                  miss.contains(QStringLiteral("user.ltx")) &&
                  miss.contains(QStringLiteral("程序目录")) &&
                  miss.contains(QStringLiteral("不搜 PATH")),
              QStringLiteral("视频⑩：缺失理由要含**试过哪些路径**（实得 %1）").arg(miss.left(110)));
        check(bare.progress_label() != nullptr &&
                  bare.progress_label()->text().contains(QStringLiteral("失败")),
              QStringLiteral("视频⑩：缺失在进度文案上也可见"));
        // 状态栏三态：探测之前是"未检测"（**不是 ✗**）；探测之后按**真实结论**报
        //（有的机器/包目录里恰好有实物 ffmpeg ⇒ 两种结论都要如实、都要带路径信息）
        bare.detect();
        const QString state_text = bare.status_text();
        if (bare.probe_state() == evid::ProbeState::Missing) {
            check(state_text.contains(QStringLiteral("✗")) &&
                      state_text.contains(QStringLiteral("试过")),
                  QStringLiteral("视频⑩：找不到时状态栏要报出试过的路径（%1）")
                      .arg(state_text.left(90)));
        } else {
            check(state_text.contains(QStringLiteral("✓")) &&
                      state_text.contains(QStringLiteral("ffmpeg")),
                  QStringLiteral("视频⑩：找得到时状态栏要报**哪一份** ffmpeg（%1）")
                      .arg(state_text.left(90)));
        }

        // ⑪ 自动安装按钮**不再是空壳**：点一下要真做事、结论可见
        QWidget* setup = bare.request_setup();
        QPushButton* install_button = nullptr;
        if (setup != nullptr) {
            for (QPushButton* candidate : setup->findChildren<QPushButton*>()) {
                if (candidate->text() == QStringLiteral("自动安装")) install_button = candidate;
            }
        }
        check(install_button != nullptr, QStringLiteral("视频⑪：设置小窗里有「自动安装」"));
        if (install_button != nullptr) install_button->click();
        const QString after = bare.status_label() != nullptr ? bare.status_label()->text() : QString();
        check(!after.trimmed().isEmpty() &&
                  (after.contains(QStringLiteral("自动安装")) ||
                   after.contains(QStringLiteral("自带")) ||
                   after.contains(QStringLiteral("download_ffmpeg.py"))),
              QStringLiteral("视频⑪：点「自动安装」要给出**结论**（%1）").arg(after.left(70)));
    }
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

    // ⑬ ★★ Windows 版本资源（VERSIONINFO）——用户 2026-10-04 口径：exe "完全没有元数据、也没有签名"
    //   是杀软误报的一半根因。判据落在**真实资源字节**上（读本进程 exe 的资源，不是看源码里写没写 .rc）：
    //     · 三字段非空：ProductName / FileDescription / FileVersion —— 资源没挂上/挂错目标就红；
    //     · FileVersion == core::version_string() 的版本号部分 —— **同一个进程里现取现比**：
    //       在 .rc 里手写一个旧版本串 ⇒ 这里立刻红（.rc 与 version.h 的漂移另由门禁
    //       `gen_winres.py --check` 拦；兄弟产物 CLI / 构建器由门禁 [30] 一起查）。
    //       用户原话："版本只有一个事实来源，别处不许复制版本串"。
#if defined(_WIN32)
    {
        const QString product = exe_version_field(QStringLiteral("ProductName"));
        const QString description = exe_version_field(QStringLiteral("FileDescription"));
        const QString file_version = exe_version_field(QStringLiteral("FileVersion"));
        const QString original = exe_version_field(QStringLiteral("OriginalFilename"));
        check(!product.isEmpty() && !description.isEmpty() && !file_version.isEmpty(),
              QStringLiteral("⑬ 版本资源三字段非空（ProductName=%1 / FileDescription=%2 / "
                             "FileVersion=%3）")
                  .arg(product.isEmpty() ? QStringLiteral("(空)") : product,
                       description.isEmpty() ? QStringLiteral("(空)") : description,
                       file_version.isEmpty() ? QStringLiteral("(空)") : file_version));
        const QString compiled = QString::fromStdString(xrkit::core::version_string());
        const QString semver = compiled.section(QLatin1Char(' '), 0, 0);
        check(!semver.isEmpty() && file_version == semver,
              QStringLiteral("⑬ FileVersion 与 core::version_string() 的版本号部分一致"
                             "（资源=%1 / 编译进去的版本串=%2 ⇒ 版本号部分=%3）")
                  .arg(file_version, compiled, semver));
        // 挂错目标的资源（把 CLI 的那份挂到本 exe 上）⇒ OriginalFilename 对不上，当场红
        const QString self_name = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        check(original.compare(self_name, Qt::CaseInsensitive) == 0,
              QStringLiteral("⑬ OriginalFilename 是本 exe 的名字（资源=%1 / 实际=%2）")
                  .arg(original, self_name));
        result.notes.append(QStringLiteral("⑬ 版本资源：%1 | %2 | FileVersion=%3 | 原名=%4")
                                .arg(product, description, file_version, original));
    }
#endif

    // ⑭ ★★ 2026-10-05 **像素级锁**：用户对着裁剪图说「那个露出深灰色底板的设计需要重做或者
    //   改掉，很丑」「很多控件会露出深灰色的底板」「很多下拉栏设计太丑了」「勾就勾」。
    //   机制（本轮实测）：泛化规则 `QWidget { background: @{bg}; }` 命中"没写 QSS 的裸容器"
    //   （`path_row()` 的行容器、`TaskShell::panel()`、页视口…），容器自己涂了一层比卡片更深的
    //   灰，又只露出**标签背后那一块** ⇒ 就是那块"缺了一块"的深灰圆角底板。
    //   判据不靠肉眼：**把真实控件渲染成图，逐像素比**。
    //     ① 行容器"标签区"的底色 == 卡片底色（`surface2`）—— 改回旧样式立刻红；
    //     ② 同类容器（TaskShell 面板）里不许存在"整块 == `bg`"的填充；
    //     ③ 下拉框 / 勾选框：控件矩形内**不许有与所在面板底色不同的整块填充**。
    {
        const auto color_of = [](const char* role) {
            return QColor(QString::fromUtf8(
                xrkit::core::theme_color(xrkit::core::ThemeMode::Dark, role)));
        };
        const QColor bg = color_of("bg");
        const QColor surface = color_of("surface");
        const QColor surface2 = color_of("surface2");
        const QColor entry = color_of("entry_bg");
        // 采样一块区域里"最常见的颜色"（跳过边框/文字的抗锯齿过渡像素）
        const auto dominant = [](const QImage& image, const QRect& rect) {
            QHash<QRgb, int> histogram;
            const QRect clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const QRgb pixel = image.pixel(x, y);
                    histogram[pixel] = histogram.value(pixel, 0) + 1;
                }
            }
            QRgb best = 0;
            int best_count = 0;
            for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
                if (it.value() > best_count) {
                    best_count = it.value();
                    best = it.key();
                }
            }
            // ★ 必须走 `fromRgba`：`QColor(QRgb)` 会把 **alpha 丢掉**（全透明区域会变成
            //   "不透明的黑"）⇒ 把"容器没涂底"误判成"容器涂了 #000000"（本轮实测踩过一次）。
            return QColor::fromRgba(best);
        };
        // 一块区域里有几个"整块填充色"（同一颜色出现 >= 8 次算一块）—— 用来断言
        // "控件矩形内不存在与面板底色不同的整块填充"：文字/边框/抗锯齿都是零散像素。
        const auto block_colors = [](const QImage& image, const QRect& rect, QColor panel,
                                     const QList<QColor>& allowed) {
            QHash<QRgb, int> histogram;
            const QRect clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    histogram[image.pixel(x, y)] = histogram.value(image.pixel(x, y), 0) + 1;
                }
            }
            QStringList offenders;
            for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
                if (it.value() < 8) continue;
                const QColor color(it.key());
                if (color == panel) continue;
                bool ok = false;
                for (const QColor& fine : allowed) {
                    if (color == fine) ok = true;
                }
                if (!ok) offenders << color.name();
            }
            return offenders;
        };

        Hub style_hub(nullptr, QString());
        // 字体系统：勾选框的"勾"由 CheckMarkStyle 画，不受字体影响；这里先量字号避免误判
        style_hub.resize(1600, 1000);
        style_hub.show();
        style_hub.set_current_page(5);   // 汉化包生成页（用户截图那一页：三条路径行 + 下拉 + 勾选框）
        for (int pass = 0; pass < 3; ++pass) {
            style_hub.layout()->activate();
            for (QWidget* child : style_hub.findChildren<QWidget*>()) {
                if (child->layout() != nullptr) child->layout()->activate();
            }
            QApplication::processEvents();
        }

        // ① 标签背后的底板：**字段行容器**（`DropZones::attach_row` 统一挂 `field_row` 名字，
        //    六个页面都走这一条）自己不许涂底 —— 涂了就是用户截图里标签背后那块深灰圆角。
        int plate_hits = 0;
        QString plate_detail;
        for (QWidget* container : style_hub.findChildren<QWidget*>()) {
            if (container->objectName() != QStringLiteral("field_row")) continue;
            // 只看**当前页上真的显示出来**的行：没显示的页还没走过布局，几何是退化值
            // （实测：隐藏页的行容器宽 640、输入框挪到 x=452）⇒ 那种残缺几何上的像素
            // 判据不是用户看到的界面，会假红。
            if (!container->isVisible()) continue;
            const auto* edit = container->findChild<QLineEdit*>();
            const auto* label = container->findChild<QLabel*>();
            if (edit == nullptr || label == nullptr) continue;   // 只认"标签 + 输入框"这种字段行
            const QSize size = container->size();
            if (size.width() < 8 || size.height() < 8) continue;
            // 判据直接盯"谁涂了底"：把**行容器自己**渲染成图（DrawWindowBackground）
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(0, 0, 0, 0));
            container->render(&image);
            // 标签带 = 左边 2..(输入框左缘-2)，纵向取中间 3/5（避开上下边框与输入框）
            const int right = std::max(3, edit->x() - 2);
            const QRect strip(2, size.height() / 5, std::max(1, right - 2),
                              std::max(1, size.height() * 3 / 5));
            const QColor painted = dominant(image, strip);
            if (painted.alpha() == 0) continue;   // 透明 = 容器不涂底（这就是要的）
            if (painted != surface2) {
                ++plate_hits;
                if (plate_detail.isEmpty()) {
                    plate_detail = QStringLiteral("字段行(%1,%2 %3x%4) 标签带涂了 %5（应为卡片底 %6）")
                                       .arg(container->x())
                                       .arg(container->y())
                                       .arg(size.width())
                                       .arg(size.height())
                                       .arg(painted.name(), surface2.name());
                }
            }
        }
        check(plate_hits == 0,
              QStringLiteral("⑭ 标签背后不许有深灰底板（%1 处；%2）")
                  .arg(plate_hits)
                  .arg(plate_detail.isEmpty() ? QStringLiteral("无") : plate_detail));

        // ② 任务面板（同一个机制的另一处现场，用户截图图二"缺了一块"）：它是裸容器且带进度条，
        //    渲染出来的底必须是**透明的**（底交给卡片），不许自己涂一层 `bg`。
        {
            int panel_hits = 0;
            QString detail;
            for (QWidget* panel : style_hub.findChildren<QWidget*>()) {
                if (panel->objectName() != QStringLiteral("task_panel")) continue;
                const QSize size = panel->size();
                if (size.width() < 8 || size.height() < 8) continue;
                QImage image(size, QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(0, 0, 0, 0));
                panel->render(&image);
                const QColor painted =
                    dominant(image, QRect(2, 2, std::max(1, size.width() - 4),
                                          std::max(1, size.height() / 3)));
                if (painted.alpha() != 0 && painted == bg) {
                    ++panel_hits;
                    if (detail.isEmpty()) {
                        detail = QStringLiteral("任务面板涂了 %1").arg(painted.name());
                    }
                }
            }
            check(panel_hits == 0,
                  QStringLiteral("⑭ 任务面板不许自己涂 %1 底（%2 处；%3）")
                      .arg(bg.name())
                      .arg(panel_hits)
                      .arg(detail.isEmpty() ? QStringLiteral("无") : detail));
        }

        // ③ 下拉框：控件矩形内不许有"与面板底色不同的整块填充"
        {
            int offenders = 0;
            QString detail;
            for (QComboBox* combo : style_hub.findChildren<QComboBox*>()) {
                if (!combo->isVisible()) continue;
                QImage image(combo->size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(0, 0, 0, 0));
                combo->render(&image);
                // 判据：把控件渲染出来，看**它自己的底**是不是干净的输入框底 ——
                // 取"文字下方那条横带"（避开文字/箭头；离屏上文字会退化成方块，
                // 落在文字上的判据会假红），并要求那一带**只有一个颜色且 == entry_bg**。
                // 旧样式（容器给控件涂了一层深灰 / 弹层底色撞色）在这里立刻红。
                const QRect foot(px(4), std::max(1, combo->height() - px(8)),
                                 std::max(1, combo->width() - px(8)), px(6));
                const QColor painted = dominant(image, foot);
                if (painted != entry) {
                    ++offenders;
                    if (detail.isEmpty()) {
                        detail = QStringLiteral("%1 自己的底是 %2（应为 %3）")
                                     .arg(combo->objectName(), painted.name(), entry.name());
                    }
                }
            }
            check(offenders == 0,
                  QStringLiteral("⑭ 下拉框自己的底要与输入框同一套令牌（%1 处；%2）")
                      .arg(offenders)
                      .arg(detail.isEmpty() ? QStringLiteral("无") : detail));
        }

        // ④ 勾选框：**"勾就勾"** —— 选中态必须画出一个勾（勾的颜色 = 主题 text 令牌），
        //    未选中态**不许**有勾。判据：把方框渲染到哨兵底上，数"勾色像素"。
        {
            const QColor mark = color_of("text");
            // ★ 判据用**亮度**而不是"精确等于令牌色"：勾是**抗锯齿**画的，一笔里只有少数像素
            //   恰好等于纯色（实测 7/14px 框）。亮笔画 vs 深底（`entry_bg`/`selected` 都在
            //   #23262a~#31363b）在本主题下分得很开，所以"亮像素数"既稳又能判红。
            const auto mark_pixels = [indicator = QSize(px(14), px(14))](bool on) {
                QStyleOptionButton option;
                option.rect = QRect(0, 0, indicator.width(), indicator.height());
                option.state = QStyle::State_Enabled;
                if (on) option.state |= QStyle::State_On;
                option.palette = qApp->palette();
                QImage image(indicator, QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(255, 0, 255));   // 哨兵底（亮品红）——**不算**亮笔画
                QPainter painter(&image);
                qApp->style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                painter.end();
                int hits = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor color = QColor::fromRgba(image.pixel(x, y));
                        if (color == QColor(255, 0, 255)) continue;   // 哨兵
                        if (qGray(color.rgb()) >= 150) ++hits;
                    }
                }
                return hits;
            };
            const int on_hits = mark_pixels(true);
            const int off_hits = mark_pixels(false);
            check(on_hits >= 12,
                  QStringLiteral("⑭ 勾选框选中态要真的画出勾（亮笔画像素 %1 < 12）").arg(on_hits));
            check(off_hits <= 2,
                  QStringLiteral("⑭ 勾选框未选中态不许有勾（实测 %1 个亮笔画像素）").arg(off_hits));
            // 尺寸令牌也一起钉住：勾随界面缩放（px(14)@1.5 时是 21）
            {
                const double saved = xrkit::core::ui_scale();
                xrkit::core::set_ui_scale(1.5);
                const bool scaled = px(14) == 21;
                xrkit::core::set_ui_scale(saved);
                check(scaled, QStringLiteral("⑭ 勾选框尺寸走 px() 令牌（px(14)@1.5 应为 21）"));
            }
        }

        // ⑤ 拖放高亮仍然是中性色（强调色只许三处）：这条既有口径在本轮改动后重验一次
        {
            const QString sheet = stylesheet_for(xrkit::core::ThemeMode::Dark);
            check(sheet.contains(QStringLiteral("*[drop_hover=\"true\"]")),
                  QStringLiteral("⑭ 拖放高亮规则还在（改样式别把它弄丢）"));
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ★★ 2026-10-06（D2 批）：编码转换补齐 / XML 详情栏与多列 / 系统字体选择器
    //
    // 这一批的判据都按"能不能判红"来写：把新东西拆掉、把旧行为改回去，下面任意一条
    // 都会立刻变红（每条的红/绿原始输出见交付报告）。三组的共同口径：
    //   · **零静默**：跳过/排除/失败/降级都要能读出计数或理由；
    //   · **同类东西一套组件**：批处理在 `engine::text::batch`、表格装配在 `pair_tree.h`、
    //     字体判据在 `engine::font`、起点判据在 `dialog_start.h` —— 页面里**不许**再抄一份。
    // ═══════════════════════════════════════════════════════════════════════

    // ── （D2-①）编码转换页：1.x 那 9 处"不必要阉割"逐条回接 ─────────────────
    {
        namespace etxt = xrkit::engine::text;
        ConvertPage page;
        page.resize(900, page_min_h());
        page.show();
        QApplication::processEvents();

        // ① 新控件在位且**默认值照 1.x**：扩展名 `.xml`、递归**开**、备份**开**、排除空
        check(page.ext_edit() != nullptr && page.ext_edit()->text() == QStringLiteral(".xml"),
              QStringLiteral("D2① 扩展名默认 `.xml`（实得「%1」）")
                  .arg(page.ext_edit() == nullptr ? QString() : page.ext_edit()->text()));
        check(page.recursive_box() != nullptr && page.recursive_box()->isChecked(),
              QStringLiteral("D2② 递归子目录**默认开**（1.x `:70` value=True）"));
        check(page.backup_box() != nullptr && page.backup_box()->isChecked(),
              QStringLiteral("D2③ 备份**默认开**（1.x `:71` value=True）"));
        check(page.exclude_edit() != nullptr && page.exclude_edit()->text().isEmpty(),
              QStringLiteral("D2④ 排除模式默认为空"));

        // 附：★ `ascii` 只在**目标**编码里（1.x 目标下拉有它、源下拉没有）；
        //     引擎侧的有损编解码对拍见 `tests/golden_lossy_codec.py`（补对拍时抓到真缺陷）。
        const auto has_item = [](QComboBox* combo, const QString& text) {
            if (combo == nullptr) return false;
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemData(i).toString() == text) return true;
            }
            return false;
        };
        check(has_item(page.target_combo(), QStringLiteral("ascii")),
              QStringLiteral("D2附 目标编码下拉里有 ascii（1.x 目标表里有它）"));
        check(!has_item(page.source_combo(), QStringLiteral("ascii")),
              QStringLiteral("D2附 源编码下拉里**没有** ascii（源是猜的，1.x 源表里也没有）"));
        check(page.target_combo() != nullptr &&
                  page.target_combo()->currentData().toString() == QStringLiteral("utf-8"),
              QStringLiteral("D2附 目标编码默认仍是 utf-8"));

        // ── （D3）三张东亚表上架：下拉里出现三项，且**与引擎编码名一一对应** ──
        //   ★ 判据写"一一对应"不是"包含"：下拉的 itemData 必须是**引擎那三个常量本身**
        //     （`etxt::kBig5/kShiftJis/kEucKr` 的字面值），否则界面选了 `big5`、
        //     引擎收到的是别的名字，就会静默落到"未知编码"。
        //     拆掉源表里的对应项 ⇒ 下面任意一条立刻红。
        {
            const char* kNew[] = {etxt::kBig5, etxt::kShiftJis, etxt::kEucKr};
            for (const char* name : kNew) {
                const QString want = QString::fromUtf8(name);
                check(has_item(page.source_combo(), want),
                      QStringLiteral("D3① 源编码下拉里有 %1（1.x ENCODING_CHOICES 源侧有它）")
                          .arg(want));
                check(has_item(page.target_combo(), want),
                      QStringLiteral("D3① 目标编码下拉里也有 %1（引擎两侧都实现了）").arg(want));
                // 一一对应：下拉项 data → 页面取到的编码名
                int idx = -1;
                for (int i = 0; i < page.source_combo()->count(); ++i) {
                    if (page.source_combo()->itemData(i).toString() == want) {
                        idx = i;
                        break;
                    }
                }
                check(idx >= 0, QStringLiteral("D3② 源下拉里能定位到 %1").arg(want));
                if (idx >= 0) {
                    const int keep = page.source_combo()->currentIndex();
                    page.source_combo()->setCurrentIndex(idx);
                    check(QString::fromStdString(page.current_source_encoding()) == want,
                          QStringLiteral("D3② 源下拉选中 %1 ⇒ 交给引擎的编码名就是 %1（实得 %2）")
                              .arg(want)
                              .arg(QString::fromStdString(page.current_source_encoding())));
                    page.source_combo()->setCurrentIndex(keep);
                }
            }
            // 三项**顺序**与 1.x 的 ENCODING_CHOICES 一致（gb2312 之后、windows-1251 之前）
            const QStringList want_order = {QStringLiteral("big5"),
                                            QStringLiteral("shift_jis"),
                                            QStringLiteral("euc-kr")};
            QStringList got_order;
            for (int i = 0; i < page.source_combo()->count(); ++i) {
                const QString d = page.source_combo()->itemData(i).toString();
                if (d == QStringLiteral("big5") || d == QStringLiteral("shift_jis") ||
                    d == QStringLiteral("euc-kr")) {
                    got_order << d;
                }
            }
            check(got_order == want_order,
                  QStringLiteral("D3③ 三项在源下拉里的相对顺序照 1.x 的 ENCODING_CHOICES"
                                 "（big5 → shift_jis → euc-kr，实得 %1）")
                      .arg(got_order.join(QStringLiteral(" → "))));
            check(!has_item(page.source_combo(), QStringLiteral("ascii")),
                  QStringLiteral("D2附 加了三个新项之后，源下拉里**仍然没有** ascii"));

            // ★ 全库"编码名列表"同类搜索的落点：XML 校对页也有一张编码下拉，但它的
            //   模型是 **1.x `xml_compare_app.py:302` 的 `ENCODINGS`**（六项，**不含**
            //   big5/shift_jis/euc-kr）⇒ 那一页**故意不动**，这里把它钉住，免得日后
            //   有人"顺手统一"成转换页那张表（那会偏离 1.x 的界面口径）。
            {
                XmlPage xml;
                xml.show();
                QApplication::processEvents();
                const auto enc_has = [&xml](const QString& text) {
                    if (xml.enc_a_combo() == nullptr) return false;
                    for (int i = 0; i < xml.enc_a_combo()->count(); ++i) {
                        if (xml.enc_a_combo()->itemData(i).toString() == text) return true;
                    }
                    return false;
                };
                for (const char* name : kNew) {
                    const QString want = QString::fromUtf8(name);
                    check(!enc_has(want),
                          QStringLiteral("D3④ XML 校对页的编码下拉**不含** %1"
                                         "（1.x 那一页的 ENCODINGS 只有六项）")
                              .arg(want));
                }
                check(enc_has(QStringLiteral("windows-1251")),
                      QStringLiteral("D3④ XML 校对页的编码下拉仍是 1.x 那六项之一"));
                xml.close();
            }
        }

        // ③ 输出目录非空 ⇒ 备份勾选框**禁用且取消勾选**（1.x `on_output_dir_change` `:575-580`）
        page.output_edit()->setText(QStringLiteral("E:/xrkit_d2_out"));
        check(!page.backup_box()->isEnabled() && !page.backup_box()->isChecked(),
              QStringLiteral("D2③ 输出目录非空 ⇒ 备份禁用且取消勾选（实得 enabled=%1 checked=%2）")
                  .arg(page.backup_box()->isEnabled() ? 1 : 0)
                  .arg(page.backup_box()->isChecked() ? 1 : 0));
        page.output_edit()->clear();
        check(page.backup_box()->isEnabled(),
              QStringLiteral("D2③ 输出目录清空 ⇒ 备份勾选框恢复可点"));

        // ②⑥⑦⑧⑨ 五颗"1.x 有、2.x 没有"的按钮都在
        check(page.restore_button() != nullptr && page.clear_backup_button() != nullptr &&
                  page.open_output_button() != nullptr && page.export_button() != nullptr,
              QStringLiteral("D2⑥⑦⑧⑨ 恢复备份/清空备份/打开输出/导出统计四颗按钮都在"));

        // ★ 「取消」要**先确认**（1.x `:629-637` `askyesno`）—— 确认钩子挂在**共用**任务壳上
        check(page.task() != nullptr && page.task()->has_cancel_confirm(),
              QStringLiteral("D2⑤ 取消确认钩子挂上了（1.x 取消前 askyesno）"));
        {
            bool asked = false;
            page.task()->set_cancel_confirm([&asked] {
                asked = true;
                return false;   // 用户点"否"
            });
            page.task()->run(QStringLiteral("探针"), [](const ProgressSink&, const MessageSink&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            });
            page.task()->cancel_button()->click();
            check(asked && !page.task()->cancel_requested(),
                  QStringLiteral("D2⑤ 点取消时先确认；答「否」⇒ 不置位、不留痕"));
            for (int i = 0; i < 200 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            page.task()->set_cancel_confirm([] { return true; });
        }

        // ②③④⑤ 端到端：真的递归进子目录 + 备份到 源目录\backup + 行数/计数如实
        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_d2_convert"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/sub"));
        const auto write_file = [](const QString& path, const QByteArray& body) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return;
            file.write(body);
        };
        // 源是 cp1251 的俄文 XML 声明；子目录里再放一个 —— **旧实现只读顶层，会静默漏掉它**
        const QByteArray cp1251_decl = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\n<r>\n"
                                       "<s id=\"a\">\xCF\xF0\xE8\xE2\xE5\xF2</s>\n</r>\n";
        write_file(root + QStringLiteral("/top.xml"), cp1251_decl);
        write_file(root + QStringLiteral("/sub/deep.xml"), cp1251_decl);
        write_file(root + QStringLiteral("/skip.txt"), "not xml\n");
        write_file(root + QStringLiteral("/temp_skip.xml"), cp1251_decl);

        page.source_edit()->setText(root);
        page.exclude_edit()->setText(QStringLiteral("*temp*"));
        // 1.x 的备份勾选框被"输出目录非空"取消过之后**不会自己勾回来**（它只恢复可点）
        // ⇒ 原地转换这一轮要显式勾上（这正是 1.x 的行为，不是绕过）
        page.backup_box()->setChecked(true);
        page.start_button()->click();
        for (int i = 0; i < 600 && page.task()->running(); ++i) {
            QThread::msleep(5);
            QApplication::processEvents();
        }
        QApplication::processEvents();
        const etxt::BatchOutcome outcome = page.last_outcome();
        check(page.last_scan_text().contains(QStringLiteral("递归 2 层")),
              QStringLiteral("D2② 台账报出递归层数（%1）").arg(page.last_scan_text()));
        check(outcome.collect.skipped_excluded == 1 && outcome.collect.skipped_ext == 1,
              QStringLiteral("D2④ 排除/后缀不符各自记账（排除 %1 / 后缀 %2）")
                  .arg(outcome.collect.skipped_excluded)
                  .arg(outcome.collect.skipped_ext));
        // ★★ 这一条就是"子目录被静默漏掉"那个缺陷的锁：候选必须是 2 个（顶层 + 子目录）
        check(outcome.candidates == 2 && outcome.success == 2,
              QStringLiteral("D2② **递归进子目录**：候选 %1（应为 2）、成功 %2")
                  .arg(outcome.candidates)
                  .arg(outcome.success));
        const bool saw_deep = outcome.processed == 2 && QFileInfo::exists(root + QStringLiteral("/sub/deep.xml"));
        check(saw_deep && outcome.collect.depth == 2,
              QStringLiteral("D2② 台账里的层数 = 2（实得 %1）").arg(outcome.collect.depth));
        // ③ 备份落在 `源目录\backup\<相对路径>.bak`
        check(QFileInfo::exists(root + QStringLiteral("/backup/top.xml.bak")) &&
                  QFileInfo::exists(root + QStringLiteral("/backup/sub/deep.xml.bak")),
              QStringLiteral("D2③ 备份落在 源目录\\backup\\<相对路径>.bak（新增 %1 个）")
                  .arg(outcome.backed_up));
        // ★ 输出目录模式要**保留相对结构**（否则不同子目录的同名文件会互相覆盖）
        page.output_edit()->setText(root + QStringLiteral("/out"));
        page.source_edit()->setText(root);
        page.start_button()->click();
        for (int i = 0; i < 600 && page.task()->running(); ++i) {
            QThread::msleep(5);
            QApplication::processEvents();
        }
        QApplication::processEvents();
        check(QFileInfo::exists(root + QStringLiteral("/out/sub/deep.xml")),
              QStringLiteral("D2② 输出目录保留相对结构（out/sub/deep.xml 在）"));

        // ⑨ 导出统计：utf-8-sig + 字段与 1.x 同序
        check(page.stats_ready(), QStringLiteral("D2⑨ 跑完一批后有统计快照"));
        const QString csv = root + QStringLiteral("/stats.csv");
        QString problem;
        check(page.export_stats(csv, &problem), QStringLiteral("D2⑨ 导出统计成功（%1）").arg(problem));
        {
            QFile file(csv);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray body = file.readAll();
                const QString text = QString::fromUtf8(body);
                check(body.startsWith("\xEF\xBB\xBF"), QStringLiteral("D2⑨ 统计 CSV 是 utf-8-sig"));
                check(text.contains(QStringLiteral("编码转换统计报告")),
                      QStringLiteral("D2⑨ 统计 CSV 有标题行"));
                check(text.contains(QStringLiteral("输出目录,")) &&
                          text.contains(QStringLiteral("耗时,")) &&
                          text.contains(QStringLiteral("候选文件数,")),
                      QStringLiteral("D2⑨ 统计 CSV 有 1.x 的那几行"));
            } else {
                check(false, QStringLiteral("D2⑨ 统计 CSV 读不回来"));
            }
        }

        // ⑥ 恢复备份（页面按钮 ⇒ 引擎三件）：把文件改坏再点按钮 ⇒ 内容回到原样
        {
            const QByteArray good = QByteArray(cp1251_decl);
            write_file(root + QStringLiteral("/top.xml"), "WRECKED");
            page.output_edit()->clear();
            page.source_edit()->setText(root);
            page.restore_button()->click();
            for (int i = 0; i < 600 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            QApplication::processEvents();
            QFile back(root + QStringLiteral("/top.xml"));
            QByteArray got;
            if (back.open(QIODevice::ReadOnly)) got = back.readAll();
            check(got == good,
                  QStringLiteral("D2⑥ 恢复备份把内容还原（实得 %1 字节，应为 %2）")
                      .arg(got.size())
                      .arg(good.size()));
        }
        // ⑦ 清空备份：点了之后 backup 里的 .bak 真没了，且状态栏报数
        {
            page.clear_backup_button()->click();
            for (int i = 0; i < 600 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            QApplication::processEvents();
            check(!QFileInfo::exists(root + QStringLiteral("/backup/top.xml.bak")) &&
                      page.status() != nullptr &&
                      page.status()->text().contains(QStringLiteral("清空完成")),
                  QStringLiteral("D2⑦ 清空备份真的删了且报数（状态栏「%1」）")
                      .arg(page.status() == nullptr ? QString() : page.status()->text()));
        }
        // ⑧ 打开输出：目录不存在要**明确说**（不许点了没反应）
        page.output_edit()->setText(root + QStringLiteral("/no_such_dir"));
        page.open_output();
        check(page.status() != nullptr &&
                  page.status()->text().contains(QStringLiteral("目录不存在")),
              QStringLiteral("D2⑧ 打开输出：目录不存在时说清楚（「%1」）")
                  .arg(page.status() == nullptr ? QString() : page.status()->text()));
        QDir(root).removeRecursively();
        page.hide();
    }

    // ── （D2-②）XML 校对页：结果表多列 + 详情栏 + 逐条复制id + 双击弹窗 ──────
    {
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        page.resize(980, page_min_h());
        page.show();
        QApplication::processEvents();
        // 1.x `:965-971` `_hide_detail`：没选中行时详情栏**不占位**
        check(page.detail_panel() != nullptr && !page.detail_panel()->isVisible(),
              QStringLiteral("D2-XML 未选中时详情栏不占位（1.x `_hide_detail`）"));

        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_d2_xml"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/a"));
        QDir().mkpath(root + QStringLiteral("/b"));
        const auto write = [](const QString& path, const QString& body) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return;
            file.write(body.toUtf8());
        };
        write(root + QStringLiteral("/a/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/diff.xml"),
              QStringLiteral("<r>\n<s id=\"y\"><text>V</text></s>\n"
                             "<s id=\"z\"><text>W</text></s>\n</r>\n"));
        page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                          QStringLiteral("auto"), QStringLiteral("auto"));
        page.select_visible_row(0);
        QApplication::processEvents();

        // 10.1/★ 多列：列数/列头/列策略**来自模式声明**（1.x `_apply_columns`）
        const exml::XmlModeSpec* spec = exml::find_mode(page.current_mode_key());
        check(page.result_tree() != nullptr && spec != nullptr &&
                  page.result_tree()->columnCount() == static_cast<int>(spec->columns.size()),
              QStringLiteral("D2-XML 结果表列数 = 模式声明（实得 %1 / 声明 %2）")
                  .arg(page.result_tree() == nullptr ? -1 : page.result_tree()->columnCount())
                  .arg(spec == nullptr ? -1 : static_cast<int>(spec->columns.size())));
        if (page.result_tree() != nullptr && spec != nullptr) {
            bool headers_ok = true;
            for (int col = 0; col < static_cast<int>(spec->columns.size()); ++col) {
                if (page.result_tree()->headerItem()->text(col) !=
                    QString::fromStdString(spec->columns[static_cast<size_t>(col)].heading)) {
                    headers_ok = false;
                }
            }
            check(headers_ok, QStringLiteral("D2-XML 表头文本逐列等于模式声明"));
            // 列策略：一律 Interactive ＋ **末列吃剩余宽度**（用户 09-29 口径；
            // 把某列设成 Stretch ⇒ 丢掉"列可拖"，这条立刻红）
            int stretched = 0;
            bool all_interactive = true;
            for (int col = 0; col < page.result_tree()->columnCount(); ++col) {
                const auto mode = page.result_tree()->header()->sectionResizeMode(col);
                if (mode == QHeaderView::Interactive) continue;
                if (mode == QHeaderView::Stretch) {
                    ++stretched;
                } else {
                    all_interactive = false;
                }
            }
            if (page.result_tree()->header()->stretchLastSection()) ++stretched;
            check(all_interactive && stretched == 1,
                  QStringLiteral("D2-XML 列策略＝一律 Interactive ＋ 末列拉伸（stretch=%1 all_interactive=%2）")
                      .arg(stretched)
                      .arg(all_interactive ? 1 : 0));
            // 行数与筛后行数一致；第 0 行第 0 列就是 rel（表格与 row_text 同源）
            check(page.result_tree()->topLevelItemCount() == page.visible_count(),
                  QStringLiteral("D2-XML 表格行数 = 筛后行数（%1 / %2）")
                      .arg(page.result_tree()->topLevelItemCount())
                      .arg(page.visible_count()));
        }

        // ② 选中 ⇒ 详情栏出现：头行 `[ rel ] ✓/✗ …`、正文非空、逐条 id 列表行数对得上
        check(page.detail_panel() != nullptr && page.detail_panel()->isVisible(),
              QStringLiteral("D2-XML 选中一行 ⇒ 详情栏出现（1.x `_show_detail`）"));
        const QString head = page.detail_head() == nullptr ? QString() : page.detail_head()->text();
        check(head.startsWith(QStringLiteral("[ ")) &&
                  (head.contains(QStringLiteral("✗")) || head.contains(QStringLiteral("✓"))) &&
                  head.contains(QStringLiteral("ID 数: A=")),
              QStringLiteral("D2-XML 头行形如 `[ rel ] ✗ 状态 ID 数: A= B=`（实得「%1」）").arg(head));
        check(page.detail_view() != nullptr && !page.detail_view()->toPlainText().isEmpty(),
              QStringLiteral("D2-XML 详情正文非空（%1 字节）")
                  .arg(page.detail_view() == nullptr ? 0
                                                     : page.detail_view()->toPlainText().size()));
        const std::vector<XmlDetailId> ids = page.detail_ids_for(page.selected_visible_row());
        check(page.id_list() != nullptr &&
                  page.id_list()->topLevelItemCount() == static_cast<int>(ids.size()) &&
                  !ids.empty(),
              QStringLiteral("D2-XML 逐条 id 列表行数 = 该行差异 id 数（%1 / %2）")
                  .arg(page.id_list() == nullptr ? -1 : page.id_list()->topLevelItemCount())
                  .arg(ids.size()));
        // ★ 每行一个「复制id」按钮 ⇒ 点它真的进剪贴板（1.x `:1183`/`_copy_one_id`）
        if (page.id_list() != nullptr && page.id_list()->topLevelItemCount() > 0) {
            QTreeWidgetItem* item = page.id_list()->topLevelItem(0);
            auto* button = qobject_cast<QPushButton*>(page.id_list()->itemWidget(item, 0));
            check(button != nullptr && button->text() == QStringLiteral("复制id"),
                  QStringLiteral("D2-XML 每条 id 后面有「复制id」按钮"));
            if (button != nullptr) {
                QGuiApplication::clipboard()->clear();
                button->click();
                check(QGuiApplication::clipboard()->text() == ids.front().id &&
                          page.status() != nullptr &&
                          page.status()->text().contains(QStringLiteral("已复制 id")),
                      QStringLiteral("D2-XML 点「复制id」进剪贴板（实得「%1」，应为「%2」）")
                          .arg(QGuiApplication::clipboard()->text(), ids.front().id));
            }
        }

        // ③ 双击 ⇒ 明细弹窗（1.x `:907-914`）：不 exec，只看内容
        {
            QDialog* dialog = page.make_detail_dialog(0, &page);
            check(dialog != nullptr, QStringLiteral("D2-XML 双击能造出明细弹窗"));
            if (dialog != nullptr) {
                dialog->show();
                QApplication::processEvents();
                auto* text = dialog->findChild<QPlainTextEdit*>(QStringLiteral("xml_dialog_text"));
                auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("xml_dialog_id_list"));
                auto* head_label = dialog->findChild<QLabel*>(QStringLiteral("xml_dialog_head"));
                check(text != nullptr && !text->toPlainText().isEmpty(),
                      QStringLiteral("D2-XML 弹窗里有完整明细正文"));
                check(list != nullptr && list->topLevelItemCount() == static_cast<int>(ids.size()),
                      QStringLiteral("D2-XML 弹窗里也有逐条 id（%1）")
                          .arg(list == nullptr ? -1 : list->topLevelItemCount()));
                check(head_label != nullptr && head_label->text() == head,
                      QStringLiteral("D2-XML 弹窗头行与详情栏**同一份**判据"));
                check(page.make_detail_dialog(9999, &page) == nullptr,
                      QStringLiteral("D2-XML 越界下标不造弹窗（不许空指针解引用）"));
                dialog->close();
                dialog->deleteLater();
            }
        }

        // ★ 表格与 CSV **同源**（`cell_text` 唯一一份）：CSV 第一个数据格 == 表格第 0 行第 0 列
        {
            const QString csv = root + QStringLiteral("/same_source.csv");
            check(page.export_csv(csv, QStringLiteral("2026-10-06 00:00:00")),
                  QStringLiteral("D2-XML 导出仍成功"));
            QFile file(csv);
            QString body;
            if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            const QStringList lines = body.split(QStringLiteral("\r\n"));
            const QString first_cell =
                lines.size() > 11 ? lines[11].section(QLatin1Char(','), 0, 0) : QString();
            const QString tree_cell =
                page.result_tree() != nullptr && page.result_tree()->topLevelItemCount() > 0
                    ? page.result_tree()->topLevelItem(0)->text(0)
                    : QString();
            check(!tree_cell.isEmpty() && first_cell.contains(tree_cell),
                  QStringLiteral("D2-XML 表格与 CSV 同源（CSV「%1」/ 表格「%2」）")
                      .arg(first_cell, tree_cell));
        }
        QDir(root).removeRecursively();
        page.hide();
    }

    // ── （D2-③）汉化包生成页：系统字体选择器 / 尺寸档位 / 打开输出 / 语言后缀 ────
    {
        namespace fnt = xrkit::engine::font;
        FontPackPage page;
        page.resize(980, page_min_h());
        page.show();
        QApplication::processEvents();

        // ★ 1.x `toolkit_platform.system_font_dirs` `:222-234`：**不写死 C:\Windows**
        const QStringList dirs = page.system_font_dirs();
        const QString win_root = qEnvironmentVariable("SystemRoot");
        check(!dirs.isEmpty(), QStringLiteral("D2-字体 系统字体目录非空（%1 个）").arg(dirs.size()));
        check(win_root.isEmpty() ||
                  QDir::cleanPath(dirs.first()) ==
                      QDir::cleanPath(QDir(win_root).filePath(QStringLiteral("Fonts"))),
              QStringLiteral("D2-字体 第一个目录来自 %%SystemRoot%%（实得「%1」，SystemRoot=「%2」）")
                  .arg(dirs.isEmpty() ? QString() : dirs.first(), win_root));

        // ★ 过滤：白名单后缀 + 文件名子串 + **完整路径**去重（1.x `:494-511`）
        const QStringList all_fonts = page.font_candidates(QString());
        const QStringList msyh_fonts = page.font_candidates(QStringLiteral("MSYH"));  // 大小写不敏感
        int bad_ext = 0;
        int bad_keyword = 0;
        for (const QString& path : msyh_fonts) {
            const QString name = QFileInfo(path).fileName().toLower();
            if (!name.endsWith(QStringLiteral(".ttf")) && !name.endsWith(QStringLiteral(".ttc")) &&
                !name.endsWith(QStringLiteral(".otf"))) {
                ++bad_ext;
            }
            if (!name.contains(QStringLiteral("msyh"))) ++bad_keyword;
        }
        check(bad_ext == 0 && bad_keyword == 0,
              QStringLiteral("D2-字体 候选项都在白名单后缀内且命中子串（坏后缀 %1 / 未命中 %2）")
                  .arg(bad_ext)
                  .arg(bad_keyword));
        check(QSet<QString>(all_fonts.begin(), all_fonts.end()).size() == all_fonts.size(),
              QStringLiteral("D2-字体 候选项按**完整路径**去重（%1 项）").arg(all_fonts.size()));
        check(all_fonts.size() >= msyh_fonts.size(),
              QStringLiteral("D2-字体 子串过滤真的在过滤（全部 %1 ≥ 命中 %2）")
                  .arg(all_fonts.size())
                  .arg(msyh_fonts.size()));

        // ★ 默认字体三级优先级（1.x `:129-149`）：上次用过的（存在）→ 系统雅黑 → 仓库根
        if (!msyh_fonts.isEmpty()) {
            check(page.default_font_path(msyh_fonts.first()) ==
                      QDir::toNativeSeparators(msyh_fonts.first()),
                  QStringLiteral("D2-字体 默认字体：user.ltx 里那个仍存在 ⇒ 原样继承"));
        }
        const QString fallback = page.default_font_path(QStringLiteral("Z:/nope/none.ttf"));
        check(fallback.isEmpty() || QFileInfo::exists(fallback),
              QStringLiteral("D2-字体 默认字体兜底必须是个**真存在**的文件（实得「%1」）")
                  .arg(fallback));

        // 11.8 的"同一判据"：列表标记缓存 == 引擎的四态标记（不许页面自己判一遍）
        if (!msyh_fonts.isEmpty()) {
            check(page.cached_mark(msyh_fonts.first()) ==
                      QString::fromUtf8(fnt::cjk_state_mark(
                          page.check_font(msyh_fonts.first()).state)),
                  QStringLiteral("D2-字体 列表标记与预览标题**同一判据**（引擎 cjk_state_mark）"));
        }

        // ★ 预览缩放：1.x `set_zoom` —— 步进 0.5、夹在 [0.5, 4.0]、字号 = max(12, int(40*v))
        page.zoom_preview(10.0);
        check(page.preview_zoom() == 4.0 && page.preview_pixel_size() == 160,
              QStringLiteral("D2-字体 缩放上限 4.0 ⇒ 字号 160（实得 %1 / %2）")
                  .arg(page.preview_zoom())
                  .arg(page.preview_pixel_size()));
        page.zoom_preview(-10.0);
        check(page.preview_zoom() == 0.5 && page.preview_pixel_size() == 20,
              QStringLiteral("D2-字体 缩放下限 0.5 ⇒ 字号 20（实得 %1 / %2）")
                  .arg(page.preview_zoom())
                  .arg(page.preview_pixel_size()));
        // 回到 1.0（下面要判"对话框打开时缩放标签 = 100%"）
        page.zoom_preview(0.5);
        check(page.preview_zoom() == 1.0,
              QStringLiteral("D2-字体 缩放可回到 1.0（实得 %1）").arg(page.preview_zoom()));

        // ★ 自建字体选择器（**不是**下拉、**不是**单纯文件框）：结构逐项对上 1.x
        {
            QDialog* dialog = page.make_font_picker(&page);
            check(dialog != nullptr, QStringLiteral("D2-字体 「选择…」造得出自建字体选择器"));
            if (dialog != nullptr) {
                dialog->show();
                QApplication::processEvents();
                auto* list = dialog->findChild<QListWidget*>(QStringLiteral("font_picker_list"));
                auto* filter = dialog->findChild<QLineEdit*>(QStringLiteral("font_picker_filter"));
                auto* preview = dialog->findChild<QLabel*>(QStringLiteral("font_picker_preview"));
                auto* zoom = dialog->findChild<QLabel*>(QStringLiteral("font_picker_zoom"));
                auto* caption = dialog->findChild<QLabel*>(QStringLiteral("font_picker_caption"));
                check(list != nullptr && filter != nullptr && preview != nullptr &&
                          zoom != nullptr && caption != nullptr,
                      QStringLiteral("D2-字体 左过滤+列表 / 右预览+缩放+标题 都在"));
                check(list != nullptr && list->count() == all_fonts.size(),
                      QStringLiteral("D2-字体 列表项数 = 候选项数（%1 / %2）")
                          .arg(list == nullptr ? -1 : list->count())
                          .arg(all_fonts.size()));
                check(zoom != nullptr && zoom->text() == QStringLiteral("100%"),
                      QStringLiteral("D2-字体 缩放标签初始 100%%（实得「%1」）")
                          .arg(zoom == nullptr ? QString() : zoom->text()));
                check(dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_in")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_out")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_pick")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_browse")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_cancel")) !=
                              nullptr,
                      QStringLiteral("D2-字体 ±缩放 / 选择 / 浏览其他目录 / 取消 都在"));
                // ★ **双击＝选择**（1.x `:641`）：直接发那个信号，看字体框是否真的被填上
                //   ⚠ 这条会**真的写 user.ltx**（"任何选择路径都记住"就是它的语义）⇒
                //   先备份、后复原：自检不许把"上次选择的字体"留在构建机（发行包更是
                //   在**包目录里**跑 `--smoke` —— 留着就等于往包里塞一个 user.ltx）。
                const QString ltx_path = QString::fromStdString(xrkit::core::user_ltx_path());
                const bool ltx_existed = QFileInfo::exists(ltx_path);
                QByteArray ltx_before;
                if (ltx_existed) {
                    QFile before(ltx_path);
                    if (before.open(QIODevice::ReadOnly)) ltx_before = before.readAll();
                }
                if (list != nullptr && list->count() > 0) {
                    QListWidgetItem* item = list->item(0);
                    list->setCurrentItem(item);
                    QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                              Q_ARG(QListWidgetItem*, item));
                    check(page.font_edit()->text() ==
                              QDir::toNativeSeparators(item->data(Qt::UserRole).toString()),
                          QStringLiteral("D2-字体 **双击＝选择**（字体框「%1」/ 候选项「%2」）")
                              .arg(page.font_edit()->text(),
                                   item->data(Qt::UserRole).toString()));
                }
                // user.ltx 复原（判据已经在上面取过，这里只是把机器状态还回去）
                if (ltx_existed) {
                    QFile restore(ltx_path);
                    if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        restore.write(ltx_before);
                    }
                } else {
                    QFile::remove(ltx_path);
                }
                check(QFileInfo::exists(ltx_path) == ltx_existed &&
                          (!ltx_existed || QFile(ltx_path).size() == ltx_before.size()),
                      QStringLiteral("D2-字体 自检把 user.ltx 还原（存在=%1）")
                          .arg(ltx_existed ? 1 : 0));
                // ± 缩放按钮真的改状态与标签
                if (auto* zoom_in =
                        dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_in"))) {
                    const double before_zoom = page.preview_zoom();
                    zoom_in->click();
                    QApplication::processEvents();
                    check(page.preview_zoom() > before_zoom && zoom != nullptr &&
                              zoom->text() != QStringLiteral("100%"),
                          QStringLiteral("D2-字体 「+」真的放大并刷新标签（%1%% → %2）")
                              .arg(static_cast<int>(before_zoom * 100))
                              .arg(zoom == nullptr ? QString() : zoom->text()));
                }
                dialog->close();
                dialog->deleteLater();
            }
        }

        // 尺寸档位（1.x `:84-89` ⇒ `PackageOptions::offset`）
        check(page.size_combo() != nullptr && page.size_combo()->count() == 5 &&
                  page.size_combo()->currentData().toInt() == 0,
              QStringLiteral("D2-字体 尺寸档位 5 项且默认「标准」= 0"));
        if (page.size_combo() != nullptr) {
            page.size_combo()->setCurrentIndex(1);
            check(page.size_offset() == 3,
                  QStringLiteral("D2-字体 +3 ⇒ offset=3（实得 %1）").arg(page.size_offset()));
            page.size_combo()->setCurrentIndex(0);
        }

        // 语言"自定义 + 输入框"（1.x `_lang_changed` `:202-209`）
        {
            const int custom = page.lang_combo()->findText(QStringLiteral("自定义"));
            check(custom >= 0, QStringLiteral("D2-字体 加载语言有「自定义」"));
            page.lang_combo()->setCurrentIndex(custom);
            QApplication::processEvents();
            check(page.lang_edit() != nullptr && page.lang_edit()->isVisible(),
                  QStringLiteral("D2-字体 选「自定义」⇒ 语言输入框出现"));
            page.lang_edit()->setText(QStringLiteral("jpn"));
            engine::font::PackageOptions options =
                page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.lang == "jpn",
                  QStringLiteral("D2-字体 自定义语言进 options（实得「%1」）")
                      .arg(QString::fromStdString(options.lang)));
            page.lang_combo()->setCurrentIndex(page.lang_combo()->findText(QStringLiteral("chs")));
            QApplication::processEvents();
            check(page.lang_edit() != nullptr && !page.lang_edit()->isVisible(),
                  QStringLiteral("D2-字体 切回 chs ⇒ 输入框收起"));
        }
        // 后缀预设 无/_chs/自定义（1.x `_suf_changed` `:211-220`）
        {
            page.suffix_combo()->setCurrentIndex(0);
            engine::font::PackageOptions options =
                page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix.empty(), QStringLiteral("D2-字体 后缀「无」⇒ 空串"));
            page.suffix_combo()->setCurrentIndex(1);
            options = page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix == "_chs", QStringLiteral("D2-字体 后缀「_chs」⇒ \"_chs\""));
            const int custom = page.suffix_combo()->findText(QStringLiteral("自定义"));
            page.suffix_combo()->setCurrentIndex(custom);
            QApplication::processEvents();
            page.suffix_edit()->setText(QStringLiteral("_rus"));
            options = page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix == "_rus" &&
                      page.suffix_edit() != nullptr && page.suffix_edit()->isVisible(),
                  QStringLiteral("D2-字体 自定义后缀进 options（实得「%1」）")
                      .arg(QString::fromStdString(options.suffix)));
            page.suffix_combo()->setCurrentIndex(0);
        }

        // 打开输出目录（1.x `:649-659`：目录无效 / 打开失败都要有反馈）
        {
            page.out_edit()->setText(QStringLiteral("Z:/definitely_missing_dir"));
            QString problem;
            check(!page.open_output(&problem) && !problem.isEmpty(),
                  QStringLiteral("D2-字体 打开输出：无效目录给理由（「%1」）").arg(problem));
            page.out_edit()->clear();
            problem.clear();
            check(!page.open_output(&problem) && problem.contains(QStringLiteral("无效")),
                  QStringLiteral("D2-字体 打开输出：空目录也明说（「%1」）").arg(problem));
        }
        page.hide();
    }

    return result;
}

}  // namespace xrkit::shell
