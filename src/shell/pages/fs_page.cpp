#include "fs_page.h"

#include "xrkit/core/theme.h"

#include "../virtual_list.h"

#include <QComboBox>
#include <QDir>
#include <QMetaObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace xrkit::shell {
namespace {

namespace efs = xrkit::engine::fs;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

bool FsPage::gate_source(QString& path, QString* error_out) {
    last_gate_error_.clear();
    if (registry_ == nullptr) return true;  // 没有注册表 ⇒ 一切照旧（不引入新分支）
    const std::string in = path.toUtf8().toStdString();
    const plugin::DecryptGateResult result =
        plugin::gate_db_path(*registry_, in, decrypt_dir_.toUtf8().toStdString());
    if (!result.ok) {
        last_gate_error_ = QString::fromStdString(result.error);
        if (error_out != nullptr) *error_out = last_gate_error_;
        return false;
    }
    if (result.used_plugin) {
        // 解开了：换成解密产物，并在日志里说清"这一份是插件解出来的"
        path = QString::fromStdString(result.path);
        log_message(QStringLiteral("已用插件解密器解开：%1").arg(path));
    }
    return true;
}

bool FsPage::add_loose_dir(const QString& dir, QString* error) {
    std::string err;
    const auto entries = engine::fs::loose_entries(dir.toUtf8().toStdString(), err);
    if (!err.empty()) {
        // 不存在的目录/读不动 ⇒ **可见失败**（不静默加一个空源）
        if (error != nullptr) *error = QString::fromStdString(err);
        log_message(QStringLiteral("散装层拒绝加载：%1").arg(QString::fromStdString(err)));
        return false;
    }
    QString gated = dir;
    QString gate_error;
    if (!gate_source(gated, &gate_error)) {
        if (error != nullptr) *error = gate_error;
        log_message(QStringLiteral("散装层被解密闸门拒绝：%1").arg(gate_error));
        return false;
    }
    add_source(gated, entries);
    log_message(QStringLiteral("散装层已并入：%1（%2 个条目）")
                    .arg(gated)
                    .arg(static_cast<int>(entries.size())));
    return true;
}

void FsPage::log_message(const QString& text) {
    // 页面只转发给宿主（Hub 决定往哪写）
    if (auto* hub = window()) {
        QMetaObject::invokeMethod(hub, "log_line", Qt::DirectConnection, Q_ARG(QString, text));
    }
}

FsPage::FsPage(QWidget* parent) : PageBase(QStringLiteral("fs"), QStringLiteral("文件系统"), parent) {
    decrypt_dir_ = QDir::temp().filePath(QStringLiteral("xrkit_decrypted"));
    QDir().mkpath(decrypt_dir_);
    // ── 左：包列表（6.4：拖放**直接挂在这个既有控件上**，不另造 drop_zone）
    package_list_ = new QListWidget;
    package_list_->setObjectName(QStringLiteral("fs_package_list"));
    package_list_->setAcceptDrops(true);
    package_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    package_list_->setMinimumHeight(xrkit::core::px(120));

    // ── 中：格式下拉（6.13：取值 = 内部键）+ 条目列表（虚拟化）
    format_combo_ = new QComboBox;
    format_combo_->setObjectName(QStringLiteral("fs_format_combo"));
    fill_format_combo();

    entry_list_ = new VirtualList;
    entry_list_->setObjectName(QStringLiteral("fs_entry_list"));
    entry_list_->set_row_height(xrkit::core::px(20));
    // ★ 2026-09-27（用户实测"排版有很大问题"）：条目区给自己一个**最小高度**，
    //   免得没有条目时被压成一条细线（看着像坏了）。
    entry_list_->setMinimumHeight(xrkit::core::px(160));
    entry_list_->set_row_text_provider([this](int row) -> QString {
        if (row < 0 || row >= static_cast<int>(row_texts_.size())) return QString();
        return row_texts_[static_cast<size_t>(row)];
    });

    hint_ = new QLabel(QStringLiteral("把包（.db / .sqfs）拖到左侧列表；可叠加多个包，"
                                      "同名条目**以后加载的为准**（引擎实际读到的那一份）"));
    hint_->setObjectName(QStringLiteral("fs_hint"));
    hint_->setWordWrap(true);

    body_layout()->addWidget(hint_, 0);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("打包/解包格式：")), 0);
    row->addWidget(format_combo_, 0);
    row->addStretch(1);
    body_layout()->addLayout(row);
    // 空状态提示：没有条目时显示（有货就藏）—— 与"空列表只有一条细线"配套
    entry_hint_ = new QLabel(QStringLiteral(
        "把包（.db / .sqfs）拖到窗口里就能加载；可叠加多个包，同名条目以后加载的为准。\n"
        "加载后这里显示包内文件树，勾选条目后点下方「解包」。"));
    entry_hint_->setObjectName(QStringLiteral("fs_entry_hint"));
    entry_hint_->setWordWrap(true);

    body_layout()->addWidget(package_list_, 1);
    body_layout()->addWidget(entry_list_, 1);
    // ★ 真机反馈「太空又太挤」第 2 步（2026-09-27）：空状态做成**占位块**——居中、吃满卡片
    //   剩余空间，而不是原来贴在卡片底部的一行小字（中间那片空白因此变成可读的引导）。
    empty_block_ = new QWidget;
    empty_block_->setObjectName(QStringLiteral("fs_empty_block"));
    auto* empty_layout = new QVBoxLayout(empty_block_);
    empty_layout->setContentsMargins(0, 0, 0, 0);
    empty_layout->setSpacing(xrkit::core::px(8));
    empty_layout->addStretch(1);
    empty_layout->addWidget(entry_hint_, 0, Qt::AlignHCenter);
    empty_layout->addStretch(1);
    body_layout()->addWidget(empty_block_, 1);

    // ── 操作栏（规则 2：贴底、不进视口）
    // 6.3：**两个不同的"全选"** —— 左边那组改包列表选中态，右边那个改包内勾选态。
    select_all_button_ = add_action(QStringLiteral("全选包"));
    select_all_button_->setObjectName(QStringLiteral("fs_select_all_packages"));
    add_action(QStringLiteral("加载选中"));
    add_action(QStringLiteral("取消加载"));
    add_action(QStringLiteral("移除"));
    check_all_button_ = add_action(QStringLiteral("包内全选"));
    check_all_button_->setObjectName(QStringLiteral("fs_check_all_files"));
    unpack_button_ = add_action(QStringLiteral("解包"));
    unpack_button_->setObjectName(QStringLiteral("fs_unpack"));

    QObject::connect(select_all_button_, &QPushButton::clicked, this, [this] {
        std::vector<std::string> visible;
        for (const Source& source : sources_) visible.push_back(source.key);
        selection_.select_all(visible);
        // 只动包列表：包内勾选**一个都不碰**（6.3 的两套状态）
        for (int i = 0; i < package_list_->count(); ++i) package_list_->item(i)->setSelected(true);
    });
    QObject::connect(check_all_button_, &QPushButton::clicked, this, [this] {
        check_all(true);
    });
    QObject::connect(unpack_button_, &QPushButton::clicked, this, [this] {
        // 6.11：解包走的是**同一个** `extract_to`（选中集 = 当前勾选的文件）
        std::vector<std::string> only;
        for (const efs::MergeNode* node : tree_.flatten()) {
            if (!node->is_dir && node->checked) only.push_back(node->path);
        }
        // 真程序里"从哪个包解"= 包列表里选中的那一份；这里取第一个源（自检也用它喂）
        const QString image = sources_.empty() ? QString() : sources_.front().path;
        const efs::ExtractTreeResult result =
            extract_to(image, image + QStringLiteral(".out"), efs::Selection::Listed, only);
        if (hint_ != nullptr) {
            hint_->setText(result.ok
                               ? QStringLiteral("解包完成：%1 个文件、%2 字节（失败 %3 项）%4")
                                     .arg(result.files_written)
                                     .arg(result.bytes_written)
                                     .arg(result.failures.size())
                                     .arg(utf8(result.failure_note()))
                               : QStringLiteral("解包失败：%1").arg(utf8(result.error)));
        }
    });

    refresh_rows();
}

void FsPage::fill_format_combo() {
    format_combo_->clear();
    for (const efs::FsFormatItem& item : efs::fs_formats()) {
        // 显示名给人看，**取值给机器用**（内部键）—— 1.x 把显示名当取值存，
        // 于是 `ALL_FMTS.get(fmt)` 必然取不到，SquashFS 被静默按 xdb 打包。
        format_combo_->addItem(utf8(item.label), utf8(item.key));
    }
}

std::string FsPage::current_format_key() const {
    return format_combo_->currentData().toString().toUtf8().toStdString();
}

void FsPage::add_source(const QString& source_in,
                        const std::vector<std::pair<std::string, bool>>& entries) {
    // ⑫12.1：加载前先过插件解密器（拒绝时**不加载**，理由给用户看）
    QString gated = source_in;
    QString gate_error;
    if (!gate_source(gated, &gate_error)) {
        log_message(QStringLiteral("拒绝加载（需要解密插件）：%1").arg(gate_error));
        return;
    }
    const QString source = gated;  // 之后正文照旧用 source（形参已改名 source_in）

    efs::MergeInput input;
    input.source = source.toUtf8().toStdString();
    input.entries = entries;
    efs::MergeTree incoming = efs::build_tree(input);
    if (sources_.empty()) {
        tree_ = incoming;
    } else {
        tree_.merge(incoming, static_cast<int>(sources_.size()));
    }
    Source record;
    record.path = source;
    record.key = source.toUtf8().toStdString();
    sources_.push_back(record);
    package_list_->addItem(source);
    refresh_rows();
}

QString FsPage::source_at(int index) const {
    if (index < 0 || index >= static_cast<int>(sources_.size())) return QString();
    return sources_[static_cast<size_t>(index)].path;
}

void FsPage::check_all(bool checked) {
    tree_.toggle_all(checked);
    refresh_rows();
}

bool FsPage::entry_hint_visible() const {
    return entry_hint_ != nullptr && entry_hint_->isVisibleTo(const_cast<FsPage*>(this));
}

void FsPage::refresh_rows() {
    row_texts_.clear();
    for (const efs::MergeNode* node : tree_.flatten()) {
        QString text = utf8(node->path);
        if (node->is_dir) {
            text += QStringLiteral("/");
        } else if (node->source_index >= 0 &&
                   node->source_index < static_cast<int>(sources_.size())) {
            // "这一份来自哪个包" = 引擎实际会读到的那一份（6.1）
            text += QStringLiteral("　←　") + sources_[static_cast<size_t>(node->source_index)].path;
        }
        if (node->checked) text += QStringLiteral("　[勾选]");
        row_texts_.push_back(text);
    }
    entry_list_->set_row_count(static_cast<int>(row_texts_.size()));
    if (empty_block_ != nullptr) empty_block_->setVisible(row_texts_.empty());
}

efs::ExtractTreeResult FsPage::extract_to(const QString& image_path, const QString& out_root,
                                          efs::Selection selection_kind,
                                          const std::vector<std::string>& only) {
    // ★ 这里是本页**唯一**调用引擎导出的地方（门禁静态锁：整个页面文件里
    //   `extract_tree(` 只许出现一次）—— 6.11 的"两条路合一"就靠它。
    efs::ExtractTreeOptions options;
    options.selection = selection_kind;
    options.only = only;
    return efs::extract_tree(image_path.toUtf8().toStdString(), out_root.toUtf8().toStdString(), options);
}

}  // namespace xrkit::shell
