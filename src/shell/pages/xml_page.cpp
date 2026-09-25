#include "xml_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/sniff.h"

#include "../task_shell.h"
#include "../virtual_list.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

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
    auto* hint = new QLabel(QStringLiteral(
        "选两个目录比同名的 .xml：**行数/ID 统计**看条数与出现次数，"
        "**ID 文本对比**看同 id 的文本是否一致。解析失败的行会标成「结果不可信」。"));;
    hint->setWordWrap(true);

    dir_a_edit_ = new QLineEdit;
    dir_a_edit_->setObjectName(QStringLiteral("xml_dir_a"));
    dir_a_edit_->setAcceptDrops(true);
    dir_a_edit_->setPlaceholderText(QStringLiteral("文件夹 A（可拖进来）"));
    dir_b_edit_ = new QLineEdit;
    dir_b_edit_->setObjectName(QStringLiteral("xml_dir_b"));
    dir_b_edit_->setAcceptDrops(true);
    dir_b_edit_->setPlaceholderText(QStringLiteral("文件夹 B（可拖进来）"));

    const auto dir_row = [this](const QString& label, QLineEdit* edit, QComboBox*& enc) {
        auto* layout = new QHBoxLayout;
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        enc = new QComboBox;
        enc->addItem(QStringLiteral("auto"), QStringLiteral("auto"));
        for (const char* name : kEncodings) enc->addItem(utf8(name), utf8(name));
        layout->addWidget(enc, 0);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit] {
            const QString picked = QFileDialog::getExistingDirectory(this, QStringLiteral("选择目录"),
                                                                    edit->text());
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        return layout;
    };
    body_layout()->addWidget(hint, 0);
    body_layout()->addLayout(dir_row(QStringLiteral("文件夹 A："), dir_a_edit_, enc_a_combo_));
    body_layout()->addLayout(dir_row(QStringLiteral("文件夹 B："), dir_b_edit_, enc_b_combo_));

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

    rows_ = new VirtualList;
    rows_->setObjectName(QStringLiteral("xml_rows"));
    rows_->set_row_height(xrkit::core::px(20));
    rows_->set_row_text_provider([this](int index) -> QString {
        if (index < 0 || index >= static_cast<int>(row_texts_.size())) return QString();
        return row_texts_[static_cast<size_t>(index)];
    });
    body_layout()->addWidget(rows_, 1);

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
            const int count = compare_dirs(a, b, enc_a, enc_b);
            progress(100);
            message(QStringLiteral("比较完成：%1 个文件（当前筛选后 %2 行）")
                        .arg(count)
                        .arg(visible_count())
                        .toUtf8().toStdString());
        });
    });
    auto* export_button = add_action(QStringLiteral("导出 CSV"));
    export_button->setObjectName(QStringLiteral("xml_export"));
    QObject::connect(export_button, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出 CSV"), QStringLiteral("xml_compare.csv"),
            QStringLiteral("CSV 文件 (*.csv)"));
        if (path.isEmpty()) return;
        QString problem;
        if (!export_csv(path, QStringLiteral("1970-01-01 00:00:00"))) {
            problem = QStringLiteral("导出失败");
        }
        status_->setText(problem.isEmpty()
                             ? QStringLiteral("已导出 %1 行").arg(visible_count())
                             : problem);
    });
    refresh_rows();
}

void XmlPage::fill_mode_combo() {
    for (const exml::XmlModeSpec& spec : exml::builtin_modes()) {
        // 显示名给人看，**取值是内部键**（⑫12.8/⑥6.13 同一条纪律）
        mode_combo_->addItem(utf8(spec.name), utf8(spec.key));
    }
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
}

std::string XmlPage::name_a() const {
    const QString text = dir_a_edit_ != nullptr ? dir_a_edit_->text().trimmed() : QString();
    if (text.isEmpty()) return "A";
    return QDir(text).dirName().toUtf8().toStdString();
}

std::string XmlPage::name_b() const {
    const QString text = dir_b_edit_ != nullptr ? dir_b_edit_->text().trimmed() : QString();
    if (text.isEmpty()) return "B";
    return QDir(text).dirName().toUtf8().toStdString();
}

int XmlPage::compare_dirs(const QString& dir_a, const QString& dir_b, const QString& enc_a,
                          const QString& enc_b) {
    ++scan_count_;  // 10.4：这个计数只在真扫目录时涨（切模式不许涨）
    const std::map<std::string, QString> a = list_xml(dir_a);
    const std::map<std::string, QString> b = list_xml(dir_b);
    std::set<std::string> all;
    for (const auto& [rel, path] : a) all.insert(rel);
    for (const auto& [rel, path] : b) all.insert(rel);

    results_.clear();
    const bool text_mode = current_mode_key() == "text";
    for (const std::string& rel : all) {
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

void XmlPage::refresh_rows() {
    row_texts_.clear();
    visible_.clear();
    for (size_t i = 0; i < results_.size(); ++i) {
        const Row& row = results_[i];
        if (!row_matches(row)) continue;
        visible_.push_back(static_cast<int>(i));
        QString text;
        if (row.kind == "same") text += QStringLiteral("一致");
        else if (row.kind == "diff") text += QStringLiteral("不一致");
        else if (row.kind == "only_a") text += QStringLiteral("仅 A");
        else text += QStringLiteral("仅 B");
        if (row.result.untrusted_a || row.result.untrusted_b) text += QStringLiteral("(不可信)");
        text += QStringLiteral("\t") + utf8(row.result.rel) + QStringLiteral("\t") +
                utf8(exml::inline_detail(row.result, name_a(), name_b()));
        row_texts_.push_back(text);
    }
    rows_->set_row_count(static_cast<int>(row_texts_.size()));
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

bool XmlPage::export_csv(const QString& path, const QString& generated_at) {
    const exml::XmlModeSpec* spec = mode();
    exml::XmlCsvReport report;
    report.mode_name = spec != nullptr ? spec->name : "";
    report.folder_a = dir_a_edit_->text().trimmed().toUtf8().toStdString();
    report.folder_b = dir_b_edit_->text().trimmed().toUtf8().toStdString();
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
        for (const exml::XmlColumn& column : spec->columns) {
            if (column.key == "rel") {
                cells.push_back(row.result.rel);
            } else if (column.key == "status") {
                cells.push_back(row.kind == "same"      ? "一致"
                                : row.kind == "diff"    ? "不一致"
                                : row.kind == "only_a"  ? "仅 A 有"
                                                        : "仅 B 有");
            } else if (column.key == "lines_a") {
                cells.push_back(std::to_string(row.result.lines_a));
            } else if (column.key == "lines_b") {
                cells.push_back(std::to_string(row.result.lines_b));
            } else if (column.key == "ids_a") {
                cells.push_back(std::to_string(row.result.ids_a));
            } else if (column.key == "ids_b") {
                cells.push_back(std::to_string(row.result.ids_b));
            } else if (column.key == "diff_count") {
                cells.push_back(std::to_string(row.result.only_a.size() +
                                               row.result.only_b.size() +
                                               row.result.text_diff.size()));
            } else if (column.key == "detail") {
                cells.push_back(exml::inline_detail(row.result, name_a(), name_b()));
            } else {
                cells.push_back("");
            }
        }
        report.rows.push_back(std::move(cells));
    }
    for (const Row& row : results_) {
        if (row.result.consistent) {
            ++report.consistent;
        } else if (row.kind == "only_a") {
            ++report.only_a;
        } else if (row.kind == "only_b") {
            ++report.only_b;
        } else {
            ++report.inconsistent;
        }
    }
    // "实际使用编码"那行：从结果里取两侧第一个非空的实际编码
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

}  // namespace xrkit::shell
