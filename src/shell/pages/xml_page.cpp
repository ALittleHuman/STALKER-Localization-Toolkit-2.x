#include "xml_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/sniff.h"

#include "../pair_tree.h"
#include "../task_shell.h"

#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
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
std::map<std::string, QString> list_xml(const QString& root) {
    std::map<std::string, QString> out;
    QDirIterator it(root, QStringList() << QStringLiteral("*.xml"), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        out[QDir(root).relativeFilePath(path).toUtf8().toStdString()] = path;
    }
    return out;
}

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
    body_layout()->addWidget(result_tree_, 1);
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
    detail_view_->setMaximumHeight(xrkit::core::px(150));
    detail_layout->addWidget(detail_head_, 0);
    detail_layout->addWidget(detail_view_, 0);
    detail_layout->addWidget(new QLabel(QStringLiteral("差异 id（逐条复制）")), 0);
    id_list_ = new QTreeWidget;
    id_list_->setObjectName(QStringLiteral("xml_id_list"));
    id_list_->setRootIsDecorated(false);
    id_list_->setMaximumHeight(xrkit::core::px(150));
    // 列表结构（列/列策略）只有一份实现：`fill_id_tree` 第一次调用时装上（这里用空表触发）
    fill_id_tree(id_list_, {});
    detail_layout->addWidget(id_list_, 0);
    // ★ 1.x `:965-971` `_hide_detail`：**没选中行时详情栏不占位**（它一开始是"平时隐藏"，
    //   选中一行才加进分栏）。这不只是观感 —— 详情栏常驻会把这一页顶到 836px，
    //   800px 高的窗口里统计行直接被挤到折线以下（实测 GEOM xml_stats y=720 / 视口 583）。
    detail_panel_->setVisible(false);
    body_layout()->addWidget(detail_panel_, 0);

    // ★★ 2026-10-06（⑤）：统计标签 —— 1.x `xml_compare_app.py:406-409` 建 label、
    //   `:675-676` 写值：`一致 {cons}  |  不一致 {incon}  |  仅 {name_a} {oa}  |  仅 {name_b} {ob}`。
    //   2.x 原来只在完成时往表里塞一句消息（`:147-150`），界面上**没有**这个统计行。
    stats_label_ = new QLabel;
    stats_label_->setObjectName(QStringLiteral("xml_stats"));
    stats_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body_layout()->addWidget(stats_label_, 0);

    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    body_layout()->addWidget(task_->panel(), 0);
    status_ = new QLabel(task_->status());
    status_->setObjectName(QStringLiteral("xml_status"));

    // 10.4：切模式只重建筛选项 + 清空结果（**不重跑 I/O**）
    QObject::connect(mode_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { set_mode(QString()); });
    QObject::connect(filter_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { refresh_rows(); });
    // ★★ D2 批：选中回调（1.x `<<TreeviewSelect>>` → `_on_row_select`）＋双击弹窗
    QObject::connect(result_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        show_detail_for_item(result_tree_->currentItem());
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
        task_->run(QStringLiteral("XML 校对"), [this, a, b, enc_a, enc_b](
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
    });
    // ★ 2026-10-06（⑤）：1.x `xml_compare_app.py:391-392` 的「清除」——
    //   2.x 原来全文件 `清除` 零命中（这条用户可达功能整体消失）。
    clear_button_ = add_action(QStringLiteral("清除"));
    clear_button_->setObjectName(QStringLiteral("xml_clear"));
    QObject::connect(clear_button_, &QPushButton::clicked, this, [this] { clear_dirs(); });
    auto* export_button = add_action(QStringLiteral("导出 CSV"));
    export_button->setObjectName(QStringLiteral("xml_export"));
    QObject::connect(export_button, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出 CSV"), QStringLiteral("xml_compare.csv"),
            QStringLiteral("CSV 文件 (*.csv)"));
        if (path.isEmpty()) return;
        QString problem;
        // ⑤：生成时间 = **真实当前时间**（原来是写死的 `1970-01-01 00:00:00`）
        if (!export_csv_auto(path)) {
            problem = QStringLiteral("导出失败");
        }
        status_->setText(problem.isEmpty()
                             ? QStringLiteral("已导出 %1 行").arg(visible_count())
                             : problem);
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
    if (stats_label_ != nullptr) stats_label_->setText(stats_text());
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

std::string XmlPage::current_mode_key() const {
    return mode_combo_->currentData().toString().toUtf8().toStdString();
}

const exml::XmlModeSpec* XmlPage::mode() const {
    const exml::XmlModeSpec* spec = exml::find_mode(current_mode_key());
    if (spec == nullptr && !exml::builtin_modes().empty()) spec = &exml::builtin_modes().front();
    return spec;
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
    // ⑤：这一轮比较的对象（名字快照，统计标签/CSV 都用它）
    compared_a_ = dir_a;
    compared_b_ = dir_b;
    last_compare_cancelled_ = false;
    const std::map<std::string, QString> a = list_xml(dir_a);
    const std::map<std::string, QString> b = list_xml(dir_b);
    std::set<std::string> all;
    for (const auto& [rel, path] : a) all.insert(rel);
    for (const auto& [rel, path] : b) all.insert(rel);
    last_compare_total_ = static_cast<int>(all.size());

    results_.clear();
    const bool text_mode = current_mode_key() == "text";
    for (const std::string& rel : all) {
        // ★ 2026-10-06（①）：**每个文件之前**轮询取消请求 —— 停下时 results_ 里都是
        //   已经比完的完整结果（不写半个），行数就是"已比较数"，由前端如实报出。
        if (should_stop && should_stop()) {
            last_compare_cancelled_ = true;
            break;
        }
        const auto ita = a.find(rel);
        const auto itb = b.find(rel);
        Row row;
        if (ita == a.end()) {
            row.result = exml::only_one_side(rel, "b");
            row.kind = "only_b";
            results_.push_back(std::move(row));
            continue;
        }
        if (itb == b.end()) {
            row.result = exml::only_one_side(rel, "a");
            row.kind = "only_a";
            results_.push_back(std::move(row));
            continue;
        }
        const etxt::DecodedBytes text_a =
            etxt::read_text_bytes(ita->second.toUtf8().toStdString(), enc_a.toUtf8().toStdString());
        const etxt::DecodedBytes text_b =
            etxt::read_text_bytes(itb->second.toUtf8().toStdString(), enc_b.toUtf8().toStdString());
        if (!text_a.ok || !text_b.ok) {
            // 读不出来（编码/权限）：如实记一条"不可信"，绝不静默当成一致
            row.result = exml::only_one_side(rel, text_a.ok ? "b" : "a");
            row.result.warning = !text_a.ok ? text_a.error : text_b.error;
            row.result.untrusted_a = !text_a.ok;
            row.result.untrusted_b = !text_b.ok;
            row.kind = text_a.ok ? "only_b" : "only_a";
            results_.push_back(std::move(row));
            continue;
        }
        row.result = text_mode
                         ? exml::compare_text(rel, text_a.text, text_b.text, text_a.used_encoding,
                                              text_b.used_encoding)
                         : exml::compare_stats(rel, text_a.text, text_b.text, text_a.used_encoding,
                                               text_b.used_encoding);
        row.kind = row.result.consistent ? "same"
                                         : (row.result.only_one_side
                                                ? (row.result.only_side == "a" ? "only_a" : "only_b")
                                                : "diff");
        results_.push_back(std::move(row));
    }
    refresh_rows();
    // ⑤ 统计标签：**只在主线程刷**（本函数也被工作线程调用 ⇒ 那种情况排队回主线程；
    //   自检/直接调用在主线程 ⇒ 立刻落地，读标签的断言不用额外 pump）。
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        refresh_stats_label();
    } else {
        QMetaObject::invokeMethod(this, [this] { refresh_stats_label(); }, Qt::QueuedConnection);
    }
    return static_cast<int>(results_.size());
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
    const std::string text = row.result.mode == "text"
                                 ? exml::full_text_detail(row.result, name_a(), name_b())
                                 : exml::full_detail(row.result, name_a(), name_b());
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
        auto* copy = new QPushButton(QStringLiteral("复制id"));
        copy->setObjectName(QStringLiteral("xml_copy_id"));
        const QString id_text = one.id;
        QObject::connect(copy, &QPushButton::clicked, this, [this, id_text] { copy_id(id_text); });
        tree->setItemWidget(item, 0, copy);
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
