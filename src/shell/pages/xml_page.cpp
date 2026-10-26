#include "xml_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/xlsx.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/sniff.h"
#include "xrkit/engine/xml/dirdiff.h"

#include "../pair_tree.h"
#include "../task_shell.h"

#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>

#include <cstdlib>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <map>
#include <set>

namespace xrkit::shell {
namespace {

namespace exml = xrkit::engine::xml;
namespace etxt = xrkit::engine::text;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

const char* kEncodings[] = {etxt::kUtf8,    etxt::kWindows1251, etxt::kWindows1252,
                            etxt::kGbk,     etxt::kGb2312,      etxt::kLatin1};

// 相对路径 → 绝对路径（'/' 分隔，便于两侧对齐）
}  // namespace

XmlPage::XmlPage(QWidget* parent)
    : PageBase(QStringLiteral("xml"), QStringLiteral("XML校对"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    dir_a_edit_ = new QLineEdit;
    dir_a_edit_->setObjectName(QStringLiteral("xml_dir_a"));
    dir_a_edit_->setPlaceholderText(QStringLiteral("文件夹 A"));
    dir_b_edit_ = new QLineEdit;
    dir_b_edit_->setObjectName(QStringLiteral("xml_dir_b"));
    dir_b_edit_->setPlaceholderText(QStringLiteral("文件夹 B"));

    // ★ 2026-10-04 用户第 2 条（"目录定向"）：判据只有成员函数那一份（自检断言同一份）
    const auto dir_a_judge = [this]() { return dir_a_dialog_start(); };
    const auto dir_b_judge = [this]() { return dir_b_dialog_start(); };

    const auto dir_row = [this](const QString& label, QLineEdit* edit, QComboBox*& enc,
                                std::function<DialogStart()> judge) {
        // ★ 2026-10-04 用户第 1 条：整行成容器 ⇒ 两侧文件夹都是"拖到相应区域"
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        enc = new QComboBox;
        enc->addItem(QStringLiteral("auto"), QStringLiteral("auto"));
        for (const char* name : kEncodings) enc->addItem(utf8(name), utf8(name));
        layout->addWidget(enc, 0);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, judge] {
            // ★ 2026-10-04 用户第 2 条：①上次的选择 → ②唯一输入目录 → ③工具自身所在文件夹
            const QString picked = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择目录"),
                resolve_dialog_start(judge ? judge() : DialogStart{}));
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        drop_zones_.attach_row(container, this);
        return container;
    };
    body_layout()->addWidget(dir_row(QStringLiteral("文件夹 A："), dir_a_edit_, enc_a_combo_,
                                     dir_a_judge));
    // 文件夹 B ← 文件夹 A（唯一才用；A 空/多元 ⇒ 工具目录）
    body_layout()->addWidget(dir_row(QStringLiteral("文件夹 B："), dir_b_edit_, enc_b_combo_,
                                     dir_b_judge));

    mode_combo_ = new QComboBox;
    mode_combo_->setObjectName(QStringLiteral("xml_mode"));
    fill_mode_combo();
    filter_combo_ = new QComboBox;
    filter_combo_->setObjectName(QStringLiteral("xml_filter"));
    auto* opt_row = new QHBoxLayout;
    opt_row->addWidget(new QLabel(QStringLiteral("比较模式：")), 0);
    opt_row->addWidget(mode_combo_, 0);
    opt_row->addSpacing(xrkit::core::px(12));
    opt_row->addWidget(new QLabel(QStringLiteral("筛选：")), 0);
    opt_row->addWidget(filter_combo_, 0);
    opt_row->addStretch(1);
    body_layout()->addLayout(opt_row);
    rebuild_filter_combo();

    // ★★ D2 批：结果表＝**按模式声明的多列**（原来是单列 VirtualList、用 `\t` 拼文本）。
    //   列策略与 fs 的两棵树**同一份装配件**（末列吃剩余宽度、其余列 Interactive）。
    result_tree_ = new QTreeWidget;
    result_tree_->setObjectName(QStringLiteral("xml_rows"));
    result_tree_->setRootIsDecorated(false);   // 平表：没有层级，不该画展开箭头
    // ★★ 2026-10-25 用户：「详情/差异 id 以及整个明细栏也需要能够拖拽改变高度」
    //   ⇒ 结果表与明细栏放进竖向 QSplitter（总览行在分栏**下面**，始终可见）。
    split_ = new QSplitter(Qt::Vertical);
    split_->setObjectName(QStringLiteral("xml_split"));
    split_->setChildrenCollapsible(false);
    split_->addWidget(result_tree_);
    body_layout()->addWidget(split_, 1);
    apply_result_columns();

    // ★★ D2 批：详情栏（1.x `:436-458`）—— 上半＝明细正文，下半＝逐条 id + 复制id。
    detail_panel_ = new QWidget;
    detail_panel_->setObjectName(QStringLiteral("xml_detail_panel"));
    auto* detail_layout = new QVBoxLayout(detail_panel_);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    detail_head_ = new QLabel;
    detail_head_->setObjectName(QStringLiteral("xml_detail_head"));
    detail_head_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_view_ = new QPlainTextEdit;
    detail_view_->setObjectName(QStringLiteral("xml_detail_text"));
    detail_view_->setReadOnly(true);
    // ★ 2026-10-25：不再写死 150px 上限 —— 明细栏在 QSplitter 里，高度由用户拖。
    // ★ 2026-10-22（用户实测反馈）：给明细栏一个**显式收起入口**（不只是"取消选中"）。
    {
        auto* head_row = new QHBoxLayout;
        // ★★ 2026-10-25 用户：「详情和差异 id 都和日志这个地方一样，需要能够点击展开和收起」
        //   ⇒ 两个小标题做成可点的 QToolButton（▾/▸），与日志的折叠同一个交互。
        auto* detail_toggle = new QToolButton;
        detail_toggle_ = detail_toggle;
        detail_toggle->setObjectName(QStringLiteral("xml_detail_toggle"));
        detail_toggle->setAutoRaise(true);
        detail_toggle->setCheckable(true);
        detail_toggle->setChecked(true);
        detail_toggle->setText(QStringLiteral("▾ 详情"));
        QObject::connect(detail_toggle, &QToolButton::toggled, this,
                         [this, detail_toggle](bool on) {
                             detail_toggle->setText(on ? QStringLiteral("▾ 详情")
                                                       : QStringLiteral("▸ 详情"));
                             update_detail_sections();
                         });
        head_row->addWidget(detail_toggle, 0);
        auto* collapse = new QPushButton(QStringLiteral("收起明细"));
        collapse->setObjectName(QStringLiteral("xml_detail_collapse"));
        QObject::connect(collapse, &QPushButton::clicked, this, [this] {
            if (result_tree_ != nullptr) {
                result_tree_->clearSelection();
                result_tree_->setCurrentItem(nullptr);
            }
            show_detail_for_item(nullptr);
        });
        head_row->addWidget(detail_head_, 1);
        // ★ 2026-10-25 用户：「明细，右边的按钮偏上」⇒ 按钮在行内**垂直居中**
        head_row->addWidget(collapse, 0, Qt::AlignVCenter);
        detail_layout->addLayout(head_row);
    }
    detail_layout->addWidget(detail_view_, 1);
    {
        // 「差异 id（逐条复制）」也做成可折叠（与「详情」同一交互）
        auto* id_toggle = new QToolButton;
        id_toggle_ = id_toggle;
        id_toggle->setObjectName(QStringLiteral("xml_id_toggle"));
        id_toggle->setAutoRaise(true);
        id_toggle->setCheckable(true);
        id_toggle->setChecked(true);
        id_toggle->setText(QStringLiteral("▾ 差异 id（逐条复制）"));
        QObject::connect(id_toggle, &QToolButton::toggled, this, [this, id_toggle](bool on) {
            id_toggle->setText(on ? QStringLiteral("▾ 差异 id（逐条复制）")
                                  : QStringLiteral("▸ 差异 id（逐条复制）"));
            update_detail_sections();
        });
        detail_layout->addWidget(id_toggle, 0, Qt::AlignLeft);
    }
    id_list_ = new QTreeWidget;
    id_list_->setObjectName(QStringLiteral("xml_id_list"));
    id_list_->setRootIsDecorated(false);
    // ★ 2026-10-25：上限交给 QSplitter/折叠开关，不再写死 150px。
    // 列表结构（列/列策略）只有一份实现：`fill_id_tree` 第一次调用时装上（这里用空表触发）
    fill_id_tree(id_list_, {});
    detail_layout->addWidget(id_list_, 1);
    // ★ 1.x `:965-971` `_hide_detail`：**没选中行时详情栏不占位**（它一开始是"平时隐藏"，
    //   选中一行才加进分栏）。这不只是观感 —— 详情栏常驻会把这一页顶到 836px，
    //   800px 高的窗口里统计行直接被挤到折线以下（实测 GEOM xml_stats y=720 / 视口 583）。
    // ★ 2026-10-26：点明细栏的**空白处**也要能收起（用户：「现在只能点按钮了」）
    detail_panel_->installEventFilter(this);
    detail_panel_->setVisible(false);
    split_->addWidget(detail_panel_);   // ★ 2026-10-25：明细栏进分栏（高度可拖）
    split_->setStretchFactor(0, 1);
    split_->setStretchFactor(1, 0);
    split_->setSizes({xrkit::core::px(360), xrkit::core::px(240)});

    // ★★ 2026-10-06（⑤）：统计标签 —— 1.x `xml_compare_app.py:406-409` 建 label、
    //   `:675-676` 写值：`一致 {cons}  |  不一致 {incon}  |  仅 {name_a} {oa}  |  仅 {name_b} {ob}`。
    //   2.x 原来只在完成时往表里塞一句消息（`:147-150`），界面上**没有**这个统计行。
    stats_label_ = new QLabel;
    stats_label_->setObjectName(QStringLiteral("xml_stats"));
    stats_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // ★ 2026-10-25 用户：「总览稍微偏上了一些」⇒ 加一点上边距（不挤到分栏手柄上）
    stats_label_->setContentsMargins(0, xrkit::core::px(6), 0, 0);
    body_layout()->addWidget(stats_label_, 0);

    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    body_layout()->addWidget(task_->panel(), 0);
    // ★★ 2026-10-07（审计补充 F3「孤儿顶层控件」）：修前这一行是
    //   `status_ = new QLabel(task_->status());` —— `TaskShell::status()` 返回的是
    //   **QString**（控件访问器叫 `status_label()`），于是 `QLabel(const QString&, QWidget* = nullptr)`
    //   造出一个**无父**的标签：它是个**顶层窗口**、从不进布局（全仓 `addWidget(status_)`
    //   零命中）⇒ 本页所有页面级消息（"请先选两个文件夹" / 导出成功失败 / 已复制 id）
    //   用户**一个字都看不见**，而且没人拥有它（泄漏）。
    //   口径：页面级消息标签必须**有父、进布局**，自检按 objectName 找得到且可见。
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("xml_status"));
    status_->setWordWrap(true);
    body_layout()->addWidget(status_, 0);

    // 10.4：切模式只重建筛选项 + 清空结果（**不重跑 I/O**）
    QObject::connect(mode_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { set_mode(QString()); });
    QObject::connect(filter_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { refresh_rows(); });
    // ★★ D2 批：选中回调（1.x `<<TreeviewSelect>>` → `_on_row_select`）＋双击弹窗
    QObject::connect(result_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        // ★★ 2026-10-22（用户实测反馈）：**点空白处收不回去** —— 修前用的是
        //   `currentItem()`，而"清空选择"之后 `currentItem()` 往往**仍指向上一行**
        //   （current 与 selected 是两件事）⇒ 详情栏一直停在那儿。
        //   现在按 **selectedItems()** 判断：没有选中项 ⇒ 收起明细。
        const QList<QTreeWidgetItem*> selected = result_tree_->selectedItems();
        show_detail_for_item(selected.isEmpty() ? nullptr : selected.first());
    });
    QObject::connect(result_tree_, &QTreeWidget::itemDoubleClicked, this,
                     [this](QTreeWidgetItem* item, int) {
                         if (item == nullptr) return;
                         const int index = item->data(0, Qt::UserRole).toInt();
                         if (QDialog* dialog = make_detail_dialog(index, this)) dialog->show();
                     });

    auto* start = add_action(QStringLiteral("开始比较"));
    start->setObjectName(QStringLiteral("xml_start"));
    QObject::connect(start, &QPushButton::clicked, this, [this] {
        const QString a = dir_a_edit_->text().trimmed();
        const QString b = dir_b_edit_->text().trimmed();
        if (a.isEmpty() || b.isEmpty()) {
            status_->setText(QStringLiteral("请先选两个文件夹"));
            return;
        }
        const QString enc_a = enc_a_combo_->currentData().toString();
        const QString enc_b = enc_b_combo_->currentData().toString();
        // ★★ 2026-10-06（审计 3-5）：**worker 不许读控件** —— 模式 key 在起线程**之前**
        //   快照下来（worker 里 `current_mode_key()` 会读这个快照，不再碰 `mode_combo_`）。
        //   同时任务跑完（`apply_finish`）再清空，免得下一轮主线程路径读到旧值。
        mode_key_at_task_start_ = mode_combo_->currentData().toString();
        // ★★ 2026-10-06（审计 3-5）：**起 worker 之前**在主线程同步禁用模式/筛选控件 ——
        //   「比较期间不许切模式/换筛选」这条只在这里生效（worker 那边另有阻塞式排队，
        //   管的是自检直接调 `compare_dirs` 的情形）；收尾在 `apply_finish` 恢复。
        set_compare_controls_enabled(false);
        // 任务壳拒绝（已有任务在跑）⇒ 把刚禁用的控件马上放回去（否则页面会僵住）
        const bool started = task_->run(QStringLiteral("XML 校对"), [this, a, b, enc_a, enc_b](
                                                  const ProgressSink& progress,
                                                  const MessageSink& message) {
            progress(0);
            // ①：取消谓词 = 本页任务壳的取消位（compare_dirs 在**每个文件之前**轮询它）
            const auto should_stop = [this]() {
                return task_ != nullptr && task_->cancel_requested();
            };
            const int count = compare_dirs(a, b, enc_a, enc_b, should_stop);
            progress(100);
            if (last_compare_cancelled_) {
                // ①零静默：说清"停在哪、已经比了多少"（绝不报成"比较完成"）
                message(QStringLiteral("已取消：在文件边界停下（已比较 %1 / 共 %2 个文件）")
                            .arg(count)
                            .arg(last_compare_total_)
                            .toUtf8().toStdString());
                return;
            }
            message(QStringLiteral("比较完成：%1 个文件（当前筛选后 %2 行）")
                        .arg(count)
                        .arg(visible_count())
                        .toUtf8().toStdString());
        });
        if (!started) set_compare_controls_enabled(true);
    });
    // ★ 2026-10-06（⑤）：1.x `xml_compare_app.py:391-392` 的「清除」——
    //   2.x 原来全文件 `清除` 零命中（这条用户可达功能整体消失）。
    clear_button_ = add_action(QStringLiteral("清除"));
    clear_button_->setObjectName(QStringLiteral("xml_clear"));
    QObject::connect(clear_button_, &QPushButton::clicked, this, [this] { clear_dirs(); });
    auto* export_button = add_action(QStringLiteral("导出 Excel"));
    export_button_ = export_button;   // ★ P17-6：成员化 —— 比较期间要跟模式/筛选/清除一起禁用
    export_button->setObjectName(QStringLiteral("xml_export"));
    QObject::connect(export_button, &QPushButton::clicked, this, [this] {
        // ★★ 2026-10-25 用户：「导出 csv……完全没有任何用」⇒ 默认导出 **XLSX 报告**
        //   （明细/汇总/统计三张表；范围＝全部比较结果）。CSV 那条仍留在
        //   `export_csv()`（CLI/对拍用），只是不再是按钮的落点。
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出 Excel"), QStringLiteral("xml_compare.xlsx"),
            QStringLiteral("Excel 工作簿 (*.xlsx)"));
        if (path.isEmpty()) return;
        const QString done = export_xlsx(
                                   path, QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                                 ? QString()
                                 : status_->text();
        if (!done.isEmpty()) status_->setText(done);
    });
    refresh_rows();
    refresh_stats_label();
}

// ★ 2026-10-06（⑤）：1.x `_clear_entries`（`:514-518`）——**只清两个目录栏**。
void XmlPage::clear_dirs() {
    if (dir_a_edit_ != nullptr) dir_a_edit_->clear();
    if (dir_b_edit_ != nullptr) dir_b_edit_->clear();
}

QString XmlPage::stats_text() const {
    uint64_t consistent = 0;
    uint64_t inconsistent = 0;
    uint64_t only_a = 0;
    uint64_t only_b = 0;
    count_kinds(&consistent, &inconsistent, &only_a, &only_b);
    // 1.x 的空格口径照抄：`一致 N  |  不一致 N  |  仅 {名} N  |  仅 {名} N`
    return QStringLiteral("一致 %1  |  不一致 %2  |  仅 %3 %4  |  仅 %5 %6")
        .arg(static_cast<qulonglong>(consistent))
        .arg(static_cast<qulonglong>(inconsistent))
        .arg(utf8(name_a()))
        .arg(static_cast<qulonglong>(only_a))
        .arg(utf8(name_b()))
        .arg(static_cast<qulonglong>(only_b));
}

void XmlPage::refresh_stats_label() {
    if (stats_label_ == nullptr) return;
    QString text = stats_text();
    // ★★ 2026-10-10（P17-10，审计 02 F-05 Medium）：**目录遍历出错必须在页面上可见**。
    //   修前：某侧目录遍历中途出错被引擎 `break` 吞掉 ⇒ 结果表可能给出"虚假的一致/缺失"，
    //   页面上一个字都没有。现在把引擎带回来的 `report.errors` 贴在统计行下面
    //   （零静默：说清是哪一侧、什么错，并明确"结论不可信"）。
    if (!last_scan_errors_.isEmpty()) {
        text += QStringLiteral("\n⚠ 目录遍历出错 %1 处 ⇒ **本次比较结论不可信**：%2")
                    .arg(last_scan_errors_.size())
                    .arg(last_scan_errors_.first());
    }
    stats_label_->setText(text);
}

// 四个计数只有这一份实现（标签与 CSV 读同一份 ⇒ 不可能分叉）
void XmlPage::count_kinds(uint64_t* consistent, uint64_t* inconsistent, uint64_t* only_a,
                          uint64_t* only_b) const {
    for (const Row& row : results_) {
        if (row.result.consistent) {
            ++*consistent;
        } else if (row.kind == "only_a") {
            ++*only_a;
        } else if (row.kind == "only_b") {
            ++*only_b;
        } else {
            ++*inconsistent;
        }
    }
}

void XmlPage::fill_mode_combo() {
    const QString keep = mode_combo_->currentData().toString();
    mode_combo_->blockSignals(true);
    mode_combo_->clear();
    if (registry_ != nullptr) {
        // ⑫12.1/12.5：内置与插件**同一张表**，按 order 升序 + 注册顺序（内置同名优先）
        for (const plugin::CompareModeReg* mode : registry_->compare_modes_for()) {
            mode_combo_->addItem(utf8(mode->spec.name), utf8(mode->spec.key));
        }
    }
    if (mode_combo_->count() == 0) {
        // 没有注册表（或表是空的）⇒ 退回内置声明表：页面照样能用
        for (const exml::XmlModeSpec& spec : exml::builtin_modes()) {
            mode_combo_->addItem(utf8(spec.name), utf8(spec.key));
        }
    }
    const int index = mode_combo_->findData(keep);
    if (index >= 0) mode_combo_->setCurrentIndex(index);
    mode_combo_->blockSignals(false);
}

void XmlPage::set_registry(const plugin::Registry* registry) {
    registry_ = registry;
    fill_mode_combo();   // 插件模式与内置模式出现在**同一个**下拉里
    rebuild_filter_combo();
}

// ★★ 插件改装 P1（Hub 机制层）：宿主机制的入口（见 page_base.h）。本页只要注册表。
void XmlPage::attach_services(const PageServices& services) {
    PageBase::attach_services(services);
    set_registry(services.registry);
}

std::string XmlPage::current_mode_key() const {
    // ★★ 2026-10-06（审计 3-5）：**工作线程里不许读控件** —— 「开始比较」在起 worker 之前
    //   把模式 key 快照进 `mode_key_at_task_start_`（见那边的注释与「开始比较」按钮），
    //   worker 读快照；主线程路径（自检直调 / 切模式）照旧读下拉。
    if (!mode_key_at_task_start_.isEmpty() &&
        QThread::currentThread() != QCoreApplication::instance()->thread()) {
        return mode_key_at_task_start_.toUtf8().toStdString();
    }
    return mode_combo_->currentData().toString().toUtf8().toStdString();
}

const exml::XmlModeSpec* XmlPage::mode() const {
    // ── ★★ 2026-10-06（审计 3-7）：**插件模式必须走注册表解析** ──────────────────
    // 审计原文：「插件比较模式"注册成功后**无人消费**"（`xml_page.cpp:318` +
    // `compare.cpp:510-515` 只遍历 `builtin_modes()`）」。
    // 修前这里是 `exml::find_mode(current_mode_key())`，而 `find_mode` **只遍历内置表**
    // ⇒ 插件模式的 key 永远解析不出来 ⇒ 下面那句 `spec = &builtin_modes().front()` 把
    // **内置统计模式**当成它的声明：表头/筛选/行取值全用内置那份，插件注册的列与筛选
    // 一个都不生效（下拉里看得见它、选中它却什么都不是）—— 正是工程 M3 出口判据要禁的
    // "注册成功但无人消费"。
    // 现在的口径：**注册表优先**（内置模式也在注册表里，owner 是 `kBuiltinPlugin`）——
    // 页面与引擎读的是**同一张表**（⑫12.1），不存在"下拉来自注册表、声明来自内置"的分叉。
    const std::string key = current_mode_key();
    if (registry_ != nullptr) {
        if (const plugin::CompareModeReg* reg_mode = registry_->compare_mode(key)) {
            return &reg_mode->spec;
        }
    }
    const exml::XmlModeSpec* spec = exml::find_mode(key);
    if (spec == nullptr && !exml::builtin_modes().empty()) spec = &exml::builtin_modes().front();
    return spec;
}
// ★★ 2026-10-06（审计 3-7）：当前模式的**注册项**（插件模式才非空；内置模式返回 nullptr）。
// 为什么要这一份：`mode()` 只回声明（列/筛选），而"这一行怎么算/怎么拼明细"要看
// 注册项上的 `compare` / `detail_builder` —— 插件的回调就在那里，不取就等于没人消费。
const plugin::CompareModeReg* XmlPage::mode_reg() const {
    if (registry_ == nullptr) return nullptr;
    return registry_->compare_mode(current_mode_key());
}

void XmlPage::rebuild_filter_combo() {
    const exml::XmlModeSpec* spec = mode();
    const QString keep = filter_combo_->currentText();
    filter_combo_->blockSignals(true);
    filter_combo_->clear();
    if (spec != nullptr) {
        for (const exml::XmlFilter& filter : spec->filters) {
            filter_combo_->addItem(utf8(filter.label), utf8(filter.label));
        }
    }
    // 旧筛选名在新模式下仍然有效就保留，否则回到第 0 项（照 1.x 的做法）
    const int index = filter_combo_->findText(keep);
    filter_combo_->setCurrentIndex(index >= 0 ? index : 0);
    filter_combo_->blockSignals(false);
}

void XmlPage::set_mode(const QString& key) {
    if (!key.isEmpty()) {
        const int index = mode_combo_->findData(key);
        if (index >= 0) mode_combo_->setCurrentIndex(index);
    }
    // 10.4：清空旧结果 + 重建筛选项，**不重跑 I/O**
    results_.clear();
    visible_.clear();
    rebuild_filter_combo();
    refresh_rows();
    refresh_stats_label();   // ⑤ 结果被清空 ⇒ 统计标签跟着归零（不许留着上一轮的数）
}

std::string XmlPage::name_a() const {
    // ★ 2026-10-06（⑤）：1.x `:580` `self.name_a = root_a.name` —— 名字在**比较开始时**
    //   定下（`compared_a_`），之后不跟着输入框的编辑乱变（否则"统计行显示的名字"
    //   会与"这次结果对应的目录"错位）。没比过 ⇒ 用当前输入框（导出前没比也一样）。
    const QString text = !compared_a_.isEmpty()
                             ? compared_a_
                             : (dir_a_edit_ != nullptr ? dir_a_edit_->text().trimmed() : QString());
    if (text.isEmpty()) return "A";
    return QDir(text).dirName().toUtf8().toStdString();
}

std::string XmlPage::name_b() const {
    const QString text = !compared_b_.isEmpty()
                             ? compared_b_
                             : (dir_b_edit_ != nullptr ? dir_b_edit_->text().trimmed() : QString());
    if (text.isEmpty()) return "B";
    return QDir(text).dirName().toUtf8().toStdString();
}

int XmlPage::compare_dirs(const QString& dir_a, const QString& dir_b, const QString& enc_a,
                          const QString& enc_b, const std::function<bool()>& should_stop) {
    ++scan_count_;  // 10.4：这个计数只在真扫目录时涨（切模式不许涨）
    // ★★ 2026-10-08（P11b）：**目录遍历/配对/逐文件比**整套下沉到引擎
    //   （`engine/xml/dirdiff.cpp`）—— 插件页与 CLI 参照都走它，页面只剩界面侧排版。
    compared_a_ = dir_a;
    compared_b_ = dir_b;
    last_compare_cancelled_ = false;

    const exml::XmlModeSpec* spec_this_round = mode();
    const plugin::CompareModeReg* reg_this_round = mode_reg();
    exml::DirDiffOptions options;
    options.dir_a = dir_a.toUtf8().toStdString();
    options.dir_b = dir_b.toUtf8().toStdString();
    options.enc_a = enc_a.toUtf8().toStdString();
    options.enc_b = enc_b.toUtf8().toStdString();
    options.mode_key = current_mode_key();
    options.should_stop = should_stop;
    if (reg_this_round != nullptr && static_cast<bool>(reg_this_round->compare)) {
        // 插件注册的回调**真的被调用**（审计 3-7 那条不许回退）
        options.compare = reg_this_round->compare;
    }
    const exml::DirDiffReport report = exml::compare_dirs(options);
    last_compare_total_ = report.total;
    last_compare_cancelled_ = report.cancelled;
    // ★★ P17-10（02 F-05）：遍历错误带到界面（`refresh_stats_label` 在主线程把它们显示出来）
    last_scan_errors_.clear();
    for (const std::string& e : report.errors) {
        last_scan_errors_ << QString::fromStdString(e);
    }

    results_.clear();
    for (const exml::DirDiffRow& row : report.rows) {
        Row out;
        out.result = row.result;
        out.kind = row.kind;
        results_.push_back(std::move(out));
    }
    (void)spec_this_round;

    // 收尾一律**排队回主线程**（工作线程里不碰控件；见审计 3-5 那段血账）
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    if (on_main) {
        refresh_rows();
        refresh_stats_label();
    } else {
        QMetaObject::invokeMethod(
            this,
            [this] {
                refresh_rows();
                refresh_stats_label();
                set_compare_controls_enabled(true);
                mode_key_at_task_start_.clear();
            },
            Qt::QueuedConnection);
    }
    return static_cast<int>(results_.size());
}

// ★★ 2026-10-06（审计 3-5）：比较期间的"模式/筛选控件按需禁用"。
// 为什么要它（审计原文的后半句）：「叠加"比较期间模式/筛选控件未禁用"（全文件
// `setEnabled` 零命中）⇒ 主线程 `results_.clear()` 与 worker `push_back` **并发**」——
// worker 正在往 `results_` 里 push 的时候，用户在模式下拉上点一下 ⇒ 主线程 `set_mode()`
// 直接 `results_.clear()`，两个线程同时改同一个 `std::vector`（UB / 崩）。
// 口径：**只有真在工作线程跑的时候**才禁用（主线程直调的判据路径不禁用 —— 自检要在
// 比较前后来回切模式，禁用会把它自己挡住）；收尾在主线程恢复。
void XmlPage::set_compare_controls_enabled(bool on) {
    if (mode_combo_ != nullptr) mode_combo_->setEnabled(on);
    if (filter_combo_ != nullptr) filter_combo_->setEnabled(on);
    // ★★ 2026-10-10（P17-6，审计 03 F-04 Medium）：**「导出 CSV」与「清除」也要一起禁**。
    //   事实（修前）：这里只禁了模式/筛选。两条按钮的**实际**风险不一样，如实分开写：
    //     · **导出 CSV ＝ 真竞态**：`export_csv()` 遍历 `visible_`/`results_` 并 `count_kinds()`
    //       （见下面那段），而比较 worker 正在 `results_.clear()` + `push_back()`
    //       ⇒ 同一个 `std::vector` 的主/worker 并发读写（UB / 崩）。这是审计点的那一条。
    //     · **清除 ＝ 口径不一致（不是竞态）**：`clear_dirs()` 只清两个**输入框**（1.x 的
    //       「清除」语义就是如此），不碰 `results_`；但比较收尾的统计/文件夹名是**按输入框**
    //       算的 ⇒ 比较中清掉输入框，收尾会出现"统计对着一组空名字"。
    //   两条一起禁是审计口径；`export_csv` 自己还有一道"比较中拒绝"的闸（挡住绕过按钮直调）。
    if (export_button_ != nullptr) export_button_->setEnabled(on);
    if (clear_button_ != nullptr) clear_button_->setEnabled(on);
}

// ★★ 2026-10-06（审计 3-5）：只读探针（见头文件）。控件不在（构造期）也算"没禁用"。
//   ★ P17-6：判据覆盖四个控件（模式/筛选/导出/清除）—— 少一个就等于那条竞态还开着。
bool XmlPage::compare_controls_enabled() const {
    if (mode_combo_ == nullptr || filter_combo_ == nullptr) return true;
    if (export_button_ != nullptr && !export_button_->isEnabled()) return false;
    if (clear_button_ != nullptr && !clear_button_->isEnabled()) return false;
    return mode_combo_->isEnabled() && filter_combo_->isEnabled();
}

bool XmlPage::row_matches(const Row& row) const {
    const exml::XmlModeSpec* spec = mode();
    const std::string label = filter_combo_->currentText().toUtf8().toStdString();
    if (spec == nullptr) return true;
    for (const exml::XmlFilter& filter : spec->filters) {
        if (filter.label != label) continue;
        if (filter.row_kinds.empty()) return true;
        if (std::find(filter.row_kinds.begin(), filter.row_kinds.end(), row.kind) ==
            filter.row_kinds.end()) {
            return false;
        }
        if (!filter.differ_types.empty()) {
            // 文本模式的子类型：only（仅一侧有 id）/ text（文本不同）
            std::set<std::string> types;
            if (!row.result.only_a.empty() || !row.result.only_b.empty()) types.insert("only");
            if (!row.result.text_diff.empty()) types.insert("text");
            for (const std::string& want : filter.differ_types) {
                if (types.count(want) == 0) return false;
            }
        }
        return true;
    }
    return true;
}

// 一行"状态"列的唯一实现：结果表 / CSV / 详情头都读它（表头与明细不可能对不上）
QString XmlPage::row_status_text(const Row& row) const {
    if (row.kind == "same") return QStringLiteral("一致");
    if (row.kind == "only_a") return QStringLiteral("仅 %1").arg(utf8(name_a()));
    if (row.kind == "only_b") return QStringLiteral("仅 %1").arg(utf8(name_b()));
    return QStringLiteral("不一致");
}

// 一列一格的**唯一实现**（1.x `_cell_text`；结果表与 CSV 共用 ⇒ 不可能分叉）
std::string XmlPage::cell_text(const Row& row, const exml::XmlColumn& column) const {
    const QString status = row_status_text(row);
    if (column.key == "rel") return row.result.rel;
    if (column.key == "status") {
        QString text = status;
        if (row.result.untrusted_a || row.result.untrusted_b) {
            text += QStringLiteral("（不可信）");
        }
        return text.toUtf8().toStdString();
    }
    if (column.key == "lines_a") return std::to_string(row.result.lines_a);
    if (column.key == "lines_b") return std::to_string(row.result.lines_b);
    if (column.key == "ids_a") return std::to_string(row.result.ids_a);
    if (column.key == "ids_b") return std::to_string(row.result.ids_b);
    if (column.key == "diff_count") {
        return std::to_string(row.result.only_a.size() + row.result.only_b.size() +
                              row.result.text_diff.size());
    }
    if (column.key == "detail") return exml::inline_detail(row.result, name_a(), name_b());
    return std::string();
}

void XmlPage::apply_result_columns() {
    if (result_tree_ == nullptr) return;
    const exml::XmlModeSpec* spec = mode();
    PairTreeSpec tree_spec;
    std::string signature;
    if (spec != nullptr) {
        for (const exml::XmlColumn& column : spec->columns) {
            tree_spec.headers << utf8(column.heading);
            tree_spec.widths << column.width;
            signature += column.key + "|";
        }
    }
    tree_spec.min_height = 140;
    if (signature == applied_columns_ && result_tree_->columnCount() == tree_spec.headers.size()) {
        return;  // 列没变 ⇒ 不重装（免得把用户拖过的列宽复位）
    }
    applied_columns_ = signature;
    apply_pair_tree_spec(result_tree_, tree_spec);
}

QTreeWidgetItem* XmlPage::item_for_visible_row(int index) const {
    if (index < 0 || index >= static_cast<int>(row_items_.size())) return nullptr;
    return row_items_[static_cast<size_t>(index)];
}

void XmlPage::select_visible_row(int index) {
    QTreeWidgetItem* item = item_for_visible_row(index);
    if (result_tree_ == nullptr || item == nullptr) return;
    result_tree_->setCurrentItem(item);
    show_detail_for_item(item);   // `setCurrentItem` 会发选择信号，但那一条路要过事件循环
}

void XmlPage::refresh_rows() {
    row_texts_.clear();
    visible_.clear();
    row_items_.clear();
    if (result_tree_ != nullptr) {
        result_tree_->clear();   // 行重填（列/表头由 apply_result_columns 管，不动）
    }
    apply_result_columns();
    const exml::XmlModeSpec* spec = mode();
    for (size_t i = 0; i < results_.size(); ++i) {
        const Row& row = results_[i];
        if (!row_matches(row)) continue;
        const int visible_index = static_cast<int>(visible_.size());
        visible_.push_back(static_cast<int>(i));
        // 行文本（`\t` 拼接）保留下来：它是"这一行会显示成什么"的**单一口径**，
        // 自检与日志都读它；表格则按模式声明的列画（两者同源于 cell_text）。
        QString text = row_status_text(row);
        if (row.result.untrusted_a || row.result.untrusted_b) text += QStringLiteral("(不可信)");
        text += QStringLiteral("\t") + utf8(row.result.rel) + QStringLiteral("\t") +
                utf8(exml::inline_detail(row.result, name_a(), name_b()));
        row_texts_.push_back(text);

        if (result_tree_ != nullptr && spec != nullptr) {
            auto* item = new QTreeWidgetItem;
            for (int col = 0; col < static_cast<int>(spec->columns.size()); ++col) {
                item->setText(col, utf8(cell_text(row, spec->columns[static_cast<size_t>(col)])));
            }
            // 可见行下标存在**第 0 列**（与 fs 的包列表同一约定：数据永远从第 0 列读）
            item->setData(0, Qt::UserRole, visible_index);
            result_tree_->addTopLevelItem(item);
            row_items_.push_back(item);
        }
    }
    if (selected_visible_ >= static_cast<int>(visible_.size())) {
        selected_visible_ = -1;
        if (detail_head_ != nullptr) detail_head_->clear();
        if (detail_view_ != nullptr) detail_view_->clear();
        if (id_list_ != nullptr) id_list_->clear();
    }
}

QString XmlPage::row_text(int index) const {
    if (index < 0 || index >= static_cast<int>(row_texts_.size())) return QString();
    return row_texts_[static_cast<size_t>(index)];
}

QString XmlPage::detail_text(int index) const {
    if (index < 0 || index >= static_cast<int>(visible_.size())) return QString();
    const Row& row = results_[static_cast<size_t>(visible_[static_cast<size_t>(index)])];
    // 只有"看这一行"的时候才调完整明细（10.3：摘要路径绝不调它）
    // ★★ 2026-10-06（审计 3-7）：插件**给了 `detail_builder`** 就用它 —— 修前这一支
    //   只认内置 text/stats 两条路 ⇒ 插件注册的明细回调同样"注册成功但无人消费"。
    const plugin::CompareModeReg* reg = mode_reg();
    const bool plugin_detail = reg != nullptr && static_cast<bool>(reg->detail_builder);
    const std::string text =
        plugin_detail ? reg->detail_builder(row.result, name_a(), name_b())
                      : (row.result.mode == "text"
                             ? exml::full_text_detail(row.result, name_a(), name_b())
                             : exml::full_detail(row.result, name_a(), name_b()));
    if (!row.result.warning.empty()) return utf8(row.result.warning + "\n\n" + text);
    return utf8(text);
}

// 1.x `:937-950`：头行按行类别给不同形状（一致 ✓ / 仅单侧 / 不一致 ✗ + ID 数）
QString XmlPage::detail_head_for(int index) const {
    if (index < 0 || index >= static_cast<int>(visible_.size())) return QString();
    const Row& row = results_[static_cast<size_t>(visible_[static_cast<size_t>(index)])];
    const QString rel = utf8(row.result.rel);
    const QString status = row_status_text(row);
    if (row.kind == "same") return QStringLiteral("[ %1 ] ✓ %2").arg(rel, status);
    if (row.kind == "only_a" || row.kind == "only_b") {
        return QStringLiteral("[ %1 ] %2").arg(rel, status);
    }
    return QStringLiteral("[ %1 ] ✗ %2  ID 数: A=%3 B=%4")
        .arg(rel, status)
        .arg(static_cast<qulonglong>(row.result.ids_a))
        .arg(static_cast<qulonglong>(row.result.ids_b));
}

// 1.x `_detail_ids`（`:1018-1046`）：四类差异来源汇总、**去重保序**。
std::vector<XmlDetailId> XmlPage::detail_ids_for(int index) const {
    std::vector<XmlDetailId> out;
    if (index < 0 || index >= static_cast<int>(visible_.size())) return out;
    const Row& row = results_[static_cast<size_t>(visible_[static_cast<size_t>(index)])];
    std::set<std::string> seen;
    const auto add = [&out, &seen](const QString& category, const std::string& value) {
        if (value.empty() || seen.count(value) != 0) return;
        seen.insert(value);
        out.push_back(XmlDetailId{category, utf8(value)});
    };
    for (const std::string& id : row.result.only_a) {
        add(QStringLiteral("仅 %1").arg(utf8(name_a())), id);
    }
    for (const std::string& id : row.result.only_b) {
        add(QStringLiteral("仅 %1").arg(utf8(name_b())), id);
    }
    for (const auto& item : row.result.text_diff) {
        add(QStringLiteral("文本不同"), item[0]);
    }
    for (const auto& item : row.result.count_diff) {
        add(QStringLiteral("次数不同"), item[0]);
    }
    return out;
}

void XmlPage::fill_id_tree(QTreeWidget* tree, const std::vector<XmlDetailId>& ids) {
    if (tree == nullptr) return;
    tree->clear();
    if (tree->columnCount() != 3) {
        // 列策略与结果表**同一份装配件**（一律 Interactive ＋ 末列吃剩余宽度）。
        // 「复制id」按钮放在**第 0 列**（窄列），中间的 id 文本吃剩余宽度 —— 按钮在行首，
        // 不会像"按钮列在末列"那样被 stretchLastSection 拉到右边、中间留一大块空白。
        apply_pair_tree_spec(tree, PairTreeSpec{QStringList{QStringLiteral("复制"),
                                                            QStringLiteral("来源"),
                                                            QStringLiteral("id")},
                                                QList<int>{96, 110, 240}, 0, false});
    }
    for (const XmlDetailId& one : ids) {
        auto* item = new QTreeWidgetItem;
        item->setText(1, one.category);
        item->setText(2, one.id);
        tree->addTopLevelItem(item);
        // ★★ 2026-10-25 用户实测（截图）：按钮**文字被裁**（只看见「:制」）、**偏下**、**太大**。
        //   三处一起改：① 文案缩成「复制」（完整 id 进 tooltip）；② 高度按 px(20) 固定、
        //   宽度按字宽算（不再靠列宽施舍）；③ 外面包一层容器并**垂直居中** ——
        //   `setItemWidget` 会把控件拉伸到整个单元格矩形，直接放按钮就会贴顶/贴底。
        auto* copy = new QPushButton(QStringLiteral("复制"));
        copy->setObjectName(QStringLiteral("xml_copy_id"));
        copy->setToolTip(QStringLiteral("复制 id：%1").arg(one.id));
        // ★ 2026-10-26 用户复测：**还是被裁** ⇒ 高度 20→26、右边距 16→34（QSS 里还有 8px 内距）
        // ★★ 2026-10-26 第二版：不再"猜"宽高 —— 直接用它自己的 `sizeHint()`
        //   （sizeHint 已经把 QSS 的 padding/border 与字体算进去了；手写的数值总有一边对不上）。
        //   同时把**行高**也抬到按钮的 sizeHint（`setItemWidget` 不会按控件撑行，
        //   行不够高时按钮被裁的正是文字 —— 这就是用户两次看到"还是那样"的机制）。
        copy->setFixedSize(copy->sizeHint());
        item->setSizeHint(0, QSize(copy->sizeHint().width() + xrkit::core::px(10),
                                   copy->sizeHint().height() + xrkit::core::px(6)));
        copy->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        const QString id_text = one.id;
        QObject::connect(copy, &QPushButton::clicked, this, [this, id_text] { copy_id(id_text); });
        auto* copy_holder = new QWidget;
        auto* copy_holder_layout = new QHBoxLayout(copy_holder);
        copy_holder_layout->setContentsMargins(xrkit::core::px(4), 0, xrkit::core::px(4), 0);
        copy_holder_layout->setSpacing(0);
        copy_holder_layout->addWidget(copy, 0, Qt::AlignVCenter | Qt::AlignLeft);
        tree->setItemWidget(item, 0, copy_holder);
    }
}

bool XmlPage::copy_id(const QString& id) {
    QClipboard* clip = QGuiApplication::clipboard();
    if (clip == nullptr) {
        status_->setText(QStringLiteral("复制失败：拿不到剪贴板"));
        return false;
    }
    clip->setText(id);
    const QString done = QStringLiteral("已复制 id: %1").arg(id);
    status_->setText(done);
    log_to_host(done);
    return true;
}

void XmlPage::show_detail_for_item(QTreeWidgetItem* item) {
    if (item == nullptr) {
        selected_visible_ = -1;
        if (detail_head_ != nullptr) detail_head_->clear();
        if (detail_view_ != nullptr) detail_view_->clear();
        if (id_list_ != nullptr) id_list_->clear();
        if (detail_panel_ != nullptr) detail_panel_->setVisible(false);  // 1.x `_hide_detail`
        return;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    selected_visible_ = index;
    if (detail_head_ != nullptr) detail_head_->setText(detail_head_for(index));
    // 完整明细**只在这一刻**生成（10.3：摘要路径绝不调它）
    if (detail_view_ != nullptr) detail_view_->setPlainText(detail_text(index));
    fill_id_tree(id_list_, detail_ids_for(index));
    if (detail_panel_ != nullptr) detail_panel_->setVisible(true);  // 1.x `_show_detail`
}

void XmlPage::update_detail_sections() {
    if (detail_panel_ == nullptr) return;
    const bool show_detail = detail_toggle_ == nullptr || detail_toggle_->isChecked();
    const bool show_ids = id_toggle_ == nullptr || id_toggle_->isChecked();
    if (detail_view_ != nullptr) detail_view_->setVisible(show_detail);
    if (id_list_ != nullptr) id_list_->setVisible(show_ids);
    // ★★ 2026-10-26 用户实测：**两项都收起时高度还在**（截图：一大片空白）
    //   ⇒ 两个都收起就把面板压到"头部那一行 + 两个小标题"的高度；任一展开就恢复可拖。
    if (split_ == nullptr) return;
    if (!show_detail && !show_ids) {
        const int collapsed = xrkit::core::px(24) * 2 + xrkit::core::px(10) + xrkit::core::px(24);
        detail_panel_->setMaximumHeight(collapsed);
        const int total = split_->height();
        split_->setSizes({std::max(0, total - collapsed), collapsed});
    } else {
        detail_panel_->setMaximumHeight(QWIDGETSIZE_MAX);
    }
}

bool XmlPage::eventFilter(QObject* watched, QEvent* event) {
    // 明细栏的**空白区**（不是子控件）被点 ⇒ 等同于"取消选中"（用户要求恢复这个习惯）
    if (watched == detail_panel_ && event->type() == QEvent::MouseButtonPress) {
        if (result_tree_ != nullptr) {
            result_tree_->clearSelection();
            result_tree_->setCurrentItem(nullptr);
        }
        show_detail_for_item(nullptr);
        return true;
    }
    return PageBase::eventFilter(watched, event);
}

QDialog* XmlPage::make_detail_dialog(int index, QWidget* parent) {
    if (index < 0 || index >= static_cast<int>(visible_.size())) return nullptr;
    auto* dialog = new QDialog(parent != nullptr ? parent : this);
    dialog->setObjectName(QStringLiteral("xml_detail_dialog"));
    dialog->setWindowTitle(QStringLiteral("明细：%1").arg(utf8(results_[static_cast<size_t>(
        visible_[static_cast<size_t>(index)])].result.rel)));
    dialog->resize(xrkit::core::px(760), xrkit::core::px(520));
    auto* layout = new QVBoxLayout(dialog);
    auto* head = new QLabel(detail_head_for(index), dialog);
    head->setObjectName(QStringLiteral("xml_dialog_head"));
    head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* view = new QPlainTextEdit(dialog);
    view->setObjectName(QStringLiteral("xml_dialog_text"));
    view->setReadOnly(true);
    view->setPlainText(detail_text(index));
    auto* id_title = new QLabel(QStringLiteral("差异 id（逐条复制）"), dialog);
    auto* ids = new QTreeWidget(dialog);
    ids->setObjectName(QStringLiteral("xml_dialog_id_list"));
    ids->setRootIsDecorated(false);
    fill_id_tree(ids, detail_ids_for(index));
    auto* close = new QPushButton(QStringLiteral("关闭"), dialog);
    close->setObjectName(QStringLiteral("xml_dialog_close"));
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    auto* button_row = new QHBoxLayout;
    button_row->addStretch(1);
    button_row->addWidget(close, 0);
    layout->addWidget(head, 0);
    layout->addWidget(view, 1);
    layout->addWidget(id_title, 0);
    layout->addWidget(ids, 1);
    layout->addLayout(button_row);
    // 双击弹出来的那个：用完自己销毁（自检里 `deleteLater` 也不冲突）
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    return dialog;
}

bool XmlPage::export_csv(const QString& path, const QString& generated_at) {
    // ★★ 2026-10-10（P17-6，审计 03 F-04 Medium）：**比较进行中拒绝导出**。
    //   按钮那一侧已经禁用了（`set_compare_controls_enabled(false)`），这条挡住**绕过按钮
    //   直调**的调用者（自检、将来的内部调用者、插件页）：那时 worker 正在
    //   `results_.clear()+push_back()`，而本函数会遍历 `results_`/`visible_` 并 `count_kinds()`
    //   ⇒ 同一个 `std::vector` 的主/worker 并发读写（UB）。
    //   零静默：拒绝要留痕（调用方与用户都看得见"为什么没导出"）。
    if (task_ != nullptr && task_->running()) {
        status_->setText(QStringLiteral("比较进行中：导出已拒绝（等它结束或先取消）"));
        log_to_host(QStringLiteral("导出 CSV 被拒：比较任务正在跑（worker 正在改结果表）"));
        return false;
    }
    const exml::XmlModeSpec* spec = mode();
    exml::XmlCsvReport report;
    report.mode_name = spec != nullptr ? spec->name : "";
    report.folder_a = dir_a_edit_->text().trimmed().toUtf8().toStdString();
    report.folder_b = dir_b_edit_->text().trimmed().toUtf8().toStdString();
    // ★ 2026-10-06（⑤）：统计行用**文件夹名**（1.x `:1321` 的 `仅 {na}: {oa}`）
    report.name_a = name_a();
    report.name_b = name_b();
    report.enc_a = enc_a_combo_->currentData().toString().toUtf8().toStdString();
    report.enc_b = enc_b_combo_->currentData().toString().toUtf8().toStdString();
    report.filter_label = filter_combo_->currentText().toUtf8().toStdString();
    report.generated_at = generated_at.toUtf8().toStdString();
    if (spec != nullptr) {
        for (const exml::XmlColumn& column : spec->columns) report.columns.push_back(column.heading);
    }
    // 10.5：范围 = **当前筛选后**的行
    for (int index : visible_) {
        const Row& row = results_[static_cast<size_t>(index)];
        std::vector<std::string> cells;
        if (spec != nullptr) {
            for (const exml::XmlColumn& column : spec->columns) {
                // ★ D2 批：**同一份 cell_text** —— 表格里看到的与 CSV 里导出的
                //   不可能再分叉（原来这里是页面里另抄一份 if-else 链）。
                cells.push_back(cell_text(row, column));
            }
        }
        report.rows.push_back(std::move(cells));
    }
    // 计数只有一份实现（`count_kinds`）—— 统计标签与这张报表读同一份
    uint64_t consistent = 0;
    uint64_t inconsistent = 0;
    uint64_t only_a = 0;
    uint64_t only_b = 0;
    count_kinds(&consistent, &inconsistent, &only_a, &only_b);
    report.consistent = consistent;
    report.inconsistent = inconsistent;
    report.only_a = only_a;
    report.only_b = only_b;    // "实际使用编码"那行：从结果里取两侧第一个非空的实际编码
    std::string enc_a_used;
    std::string enc_b_used;
    for (const Row& row : results_) {
        if (enc_a_used.empty() && !row.result.encoding_a.empty()) enc_a_used = row.result.encoding_a;
        if (enc_b_used.empty() && !row.result.encoding_b.empty()) enc_b_used = row.result.encoding_b;
    }
    if (!enc_a_used.empty() || !enc_b_used.empty()) {
        report.encodings_used = "A=" + (enc_a_used.empty() ? "-" : enc_a_used) + " / B=" +
                                (enc_b_used.empty() ? "-" : enc_b_used);
    }
    std::string error;
    return exml::write_csv_file(path.toUtf8().toStdString(), report, error);
}

// ★ 2026-10-06（⑤）：CSV 的生成时间＝**真实当前时间**（1.x `:1312` `datetime.now()` 的
//   `%Y-%m-%d %H:%M:%S` 口径）。2.x 原来在按钮里写死 `"1970-01-01 00:00:00"`。
bool XmlPage::export_xlsx(const QString& path, const QString& generated_at) {
    // ★★ 2026-10-25（用户）：「导出 csv 是导出全部内容……完全没有发挥表格的优势」
    //   ⇒ XLSX 报告，三张表：
    //     · 明细：**每行 = 一个差异 id**（可筛选/排序/透视；不再是"把差异聚成一格文字"）
    //     · 汇总：**每文件一行**（差异数 / ID 数 / 各类计数）
    //     · 统计：元数据 + 总计数（不再是 CSV 顶部那 7 行"占位"）
    //   范围：**全部比较结果**（用户裁定）。
    if (task_ != nullptr && task_->running()) {
        status_->setText(QStringLiteral("比较进行中：导出已拒绝（等它结束或先取消）"));
        log_to_host(QStringLiteral("导出 Excel 被拒：比较任务正在跑"));
        return false;
    }
    // 行状态文本：与界面「状态」列**同一份实现**（`cell_text`；保留键 "status"）
    const auto row_status = [this](const Row& row) -> std::string {
        const exml::XmlModeSpec* spec = mode();
        if (spec != nullptr) {
            for (const exml::XmlColumn& column : spec->columns) {
                if (column.key == "status") return cell_text(row, column);
            }
        }
        return std::string();
    };

    std::vector<xrkit::core::XlsxSheet> sheets;

    // ── 表 1：明细（每行一个差异 id）────────────────────────────────────
    {
        xrkit::core::XlsxSheet sheet;
        sheet.name = "差异明细";
        sheet.header = {"文件（相对）", "状态", "类别", "ID", "A 次数", "B 次数", "说明"};
        sheet.widths = {56, 10, 16, 36, 8, 8, 70};
        for (int index : visible_) { (void)index; }   // 占位：范围=全部，不用 visible_
        for (const Row& row : results_) {
            const QString rel = utf8(row.result.rel);
            const std::string status = row_status(row);
            for (const std::string& id : row.result.only_a) {
                sheet.rows.push_back({xrkit::core::XlsxCell::of_text(rel.toStdString()),
                                      xrkit::core::XlsxCell::of_text(status),
                                      xrkit::core::XlsxCell::of_text("仅 A"),
                                      xrkit::core::XlsxCell::of_text(id),
                                      xrkit::core::XlsxCell::of_number(1),
                                      xrkit::core::XlsxCell::of_number(0),
                                      xrkit::core::XlsxCell::of_text("")});
            }
            for (const std::string& id : row.result.only_b) {
                sheet.rows.push_back({xrkit::core::XlsxCell::of_text(rel.toStdString()),
                                      xrkit::core::XlsxCell::of_text(status),
                                      xrkit::core::XlsxCell::of_text("仅 B"),
                                      xrkit::core::XlsxCell::of_text(id),
                                      xrkit::core::XlsxCell::of_number(0),
                                      xrkit::core::XlsxCell::of_number(1),
                                      xrkit::core::XlsxCell::of_text("")});
            }
            for (const auto& item : row.result.count_diff) {   // (id, A 次数, B 次数)
                sheet.rows.push_back(
                    {xrkit::core::XlsxCell::of_text(rel.toStdString()),
                     xrkit::core::XlsxCell::of_text(status),
                     xrkit::core::XlsxCell::of_text("次数不同"),
                     xrkit::core::XlsxCell::of_text(item[0]),
                     xrkit::core::XlsxCell::of_number(std::atof(item[1].c_str())),
                     xrkit::core::XlsxCell::of_number(std::atof(item[2].c_str())),
                     xrkit::core::XlsxCell::of_text("")});
            }
            for (const auto& item : row.result.text_diff) {   // (id, A 文本, B 文本)
                sheet.rows.push_back(
                    {xrkit::core::XlsxCell::of_text(rel.toStdString()),
                     xrkit::core::XlsxCell::of_text(status),
                     xrkit::core::XlsxCell::of_text("文本不同"),
                     xrkit::core::XlsxCell::of_text(item[0]),
                     xrkit::core::XlsxCell::of_number(1),
                     xrkit::core::XlsxCell::of_number(1),
                     xrkit::core::XlsxCell::of_text("A: " + item[1] + "  →  B: " + item[2])});
            }
        }
        sheets.push_back(std::move(sheet));
    }

    // ── 表 2：汇总（每文件一行）──────────────────────────────────────────
    {
        xrkit::core::XlsxSheet sheet;
        sheet.name = "文件汇总";
        sheet.header = {"文件（相对）",       "状态",     "差异数",   "ID 数 A", "ID 数 B",
                        "仅 A",               "仅 B",     "文本不同", "次数不同"};
        sheet.widths = {56, 10, 8, 8, 8, 8, 8, 10, 10};
        for (const Row& row : results_) {
            const size_t diffs = row.result.only_a.size() + row.result.only_b.size() +
                                 row.result.text_diff.size() + row.result.count_diff.size();
            sheet.rows.push_back(
                {xrkit::core::XlsxCell::of_text(utf8(row.result.rel).toStdString()),
                 xrkit::core::XlsxCell::of_text(row_status(row)),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(diffs)),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(row.result.ids_a)),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(row.result.ids_b)),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(row.result.only_a.size())),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(row.result.only_b.size())),
                 xrkit::core::XlsxCell::of_number(static_cast<double>(row.result.text_diff.size())),
                 xrkit::core::XlsxCell::of_number(
                     static_cast<double>(row.result.count_diff.size()))});
        }
        sheets.push_back(std::move(sheet));
    }

    // ── 表 3：统计（元数据 + 总计数）─────────────────────────────────────
    {
        xrkit::core::XlsxSheet sheet;
        sheet.name = "统计";
        sheet.header = {"项目", "值"};
        sheet.widths = {22, 110};
        uint64_t consistent = 0, inconsistent = 0, only_a = 0, only_b = 0;
        count_kinds(&consistent, &inconsistent, &only_a, &only_b);
        const exml::XmlModeSpec* spec = mode();
        std::string enc_a_used, enc_b_used;
        for (const Row& row : results_) {
            if (enc_a_used.empty() && !row.result.encoding_a.empty()) enc_a_used = row.result.encoding_a;
            if (enc_b_used.empty() && !row.result.encoding_b.empty()) enc_b_used = row.result.encoding_b;
        }
        const auto add = [&sheet](const std::string& key, const std::string& value) {
            sheet.rows.push_back({xrkit::core::XlsxCell::of_text(key),
                                  xrkit::core::XlsxCell::of_text(value)});
        };
        add("生成时间", generated_at.toStdString());
        add("比较模式", spec != nullptr ? spec->name : "");
        add("文件夹 A", dir_a_edit_->text().trimmed().toUtf8().toStdString());
        add("文件夹 B", dir_b_edit_->text().trimmed().toUtf8().toStdString());
        add("编码（A / B）",
            enc_a_combo_->currentData().toString().toUtf8().toStdString() + " / " +
                enc_b_combo_->currentData().toString().toUtf8().toStdString());
        add("实际使用编码（A / B）", enc_a_used + " / " + enc_b_used);
        add("筛选条件（仅影响界面）", filter_combo_->currentText().toUtf8().toStdString());
        add("一致", std::to_string(consistent));
        add("不一致", std::to_string(inconsistent));
        // `name_a()/name_b()` 返回的是 **std::string**（不是 QString）——直接拼
        add("仅 " + name_a(), std::to_string(only_a));
        add("仅 " + name_b(), std::to_string(only_b));
        add("比较文件数", std::to_string(results_.size()));
        add("明细行数", std::to_string(sheets.front().rows.size()));
        add("导出范围", "全部比较结果（不受界面筛选影响）");
        sheets.push_back(std::move(sheet));
    }

    std::string error;
    if (!xrkit::core::write_xlsx_file(path.toUtf8().toStdString(), sheets, &error)) {
        status_->setText(QStringLiteral("导出失败：%1").arg(utf8(error)));
        log_to_host(QStringLiteral("导出 Excel 失败：%1").arg(utf8(error)));
        return false;
    }
    const QString done = QStringLiteral("已导出 Excel：%1 个文件 / %2 条明细")
                             .arg(results_.size())
                             .arg(sheets.front().rows.size());
    status_->setText(done);
    log_to_host(done);
    return true;
}

bool XmlPage::export_csv_auto(const QString& path) {
    return export_csv(path, QDateTime::currentDateTime().toString(QStringLiteral(
                                "yyyy-MM-dd HH:mm:ss")));
}

// ★ 2026-10-04 用户第 2 条：两个"选择"调用点的起点判据（与按钮 lambda 里那一行同源）
DialogStart XmlPage::dir_a_dialog_start() const {
    return dialog_start_for(dir_a_edit_->text(), QStringList(), false);
}

DialogStart XmlPage::dir_b_dialog_start() const {
    return dialog_start_for(dir_b_edit_->text(), QStringList{dir_a_edit_->text()}, false);
}

}  // namespace xrkit::shell
