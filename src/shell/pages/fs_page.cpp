#include "fs_page.h"

#include "xrkit/core/theme.h"

#include "../drop_filter.h"
#include "../virtual_list.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
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

    // ★ 用户 2026-09-27 真机明令：「把图中玩意删掉」——卡片里那坨**空状态文字**连同它的
    //   容器块一起删掉；页内提示只保留**顶部标题行的副标题**那一句（原来卡片里还贴了一遍
    //   几乎同义的话，是重复）。删完卡片里只剩：打包格式行 + 两个列表。
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("打包/解包格式：")), 0);
    row->addWidget(format_combo_, 0);
    row->addStretch(1);
    // ★ 用户 2026-09-27：「1.x 的部分功能或选择没了」——1.x 有"输入行 + 浏览"和"扫描目录"，
    //   2.x 却**只有拖放**（而拖放当时根本没接线，见 drop_filter.h 的说明）⇒ 等于没有入口。
    //   这里补上**唯一**的加包入口按钮：选文件（可多选）→ 走 `add_paths`（与拖放同一条）。
    add_button_ = new QPushButton(QStringLiteral("加包"));
    add_button_->setObjectName(QStringLiteral("fs_add_package"));
    row->addWidget(add_button_, 0);
    body_layout()->addLayout(row);
    body_layout()->addWidget(package_list_, 1);
    body_layout()->addWidget(entry_list_, 1);

    QObject::connect(add_button_, &QPushButton::clicked, this, [this] {
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要加载的包（可多选）"), QString(),
            QStringLiteral("包 (*.db *.db0 *.db1 *.db2 *.db3 *.db4 *.db5 *.db6 *.db7 *.db8 "
                           "*.db9 *.xdb *.sqfs *.sq_textures *.sq_levels *.sq_base *.sq_meshes);;"
                           "所有文件 (*.*)"));
        if (!picked.isEmpty()) add_paths(picked);
    });
    // 6.4：拖放挂在**既有控件**上（包列表 + 卡片空白处），不另造 drop_zone
    drop_filter_ = new FileDropFilter([this](const QStringList& paths) { add_paths(paths); }, this);
    drop_filter_->attach(package_list_);
    drop_filter_->attach(card());

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
        // 解包结果写进**日志栏**（原来是改写卡片里那行提示；那行已按用户要求删掉，
        // 结果不能因此没地方去）。失败也照旧可见。
        log_message(result.ok
                        ? QStringLiteral("解包完成：%1 个文件、%2 字节（失败 %3 项）%4")
                              .arg(result.files_written)
                              .arg(result.bytes_written)
                              .arg(result.failures.size())
                              .arg(utf8(result.failure_note()))
                        : QStringLiteral("解包失败：%1").arg(utf8(result.error)));
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
    push_source(gated, entries);
}

void FsPage::push_source(const QString& source,
                         const std::vector<std::pair<std::string, bool>>& entries) {
    // 路径侧的闸门已经在调用方过掉（`add_source` / `add_paths`）—— 这里只动模型与界面
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

int FsPage::add_paths(const QStringList& paths) {
    int loaded = 0;
    load_errors_.clear();
    for (const QString& raw : paths) {
        // ① 先过插件解密闸门（⑫12.1）：拒绝就**不加载**，理由进日志
        QString gated = raw;
        QString gate_error;
        if (!gate_source(gated, &gate_error)) {
            const QString why = QStringLiteral("拒绝加载（需要解密插件）：%1 —— %2")
                                    .arg(raw, gate_error);
            log_message(why);
            load_errors_ << why;
            continue;
        }
        // ② 目录 ⇒ 散装层；文件 ⇒ 包（SquashFS 按 magic、DB 按自洽判定，
        //    歧义时用「打包/解包格式」下拉里选的那个，见 package_entries 的说明）
        std::string error;
        std::vector<std::pair<std::string, bool>> entries;
        if (QFileInfo(gated).isDir()) {
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error);
        } else {
            entries = efs::package_entries(gated.toUtf8().toStdString(),
                                           efs::format_key_to_db(current_format_key()), error);
        }
        if (!error.empty()) {
            // ③ **可见失败**：认不出/解不开都要说清是哪一份、为什么（绝不静默加空包）
            const QString why = QStringLiteral("加载失败：%1 —— %2").arg(raw, utf8(error));
            log_message(why);
            load_errors_ << why;
            continue;
        }
        push_source(gated, entries);
        ++loaded;
        log_message(QStringLiteral("已加载：%1（%2 项）")
                        .arg(raw)
                        .arg(static_cast<int>(entries.size())));
    }
    return loaded;
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
    // ★ 2026-09-27：空状态提示块按用户要求**已删**（见构造函数注释）⇒ 恒为 false。
    //   访问器保留是因为自检里还有"这条不该再出现"的反向断言（见 smoke.cpp）。
    return false;
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
    // ★ 一个包都没加载时（没有任何条目）**条目列表整个收起**，空间全给包列表：
    //   它就是拖放落点、也是"已加载哪些包"的唯一显示处 —— 空着的条目框纯属占地方
    //   （用户真机反馈「比例太丑」的原话场景；空状态块删掉后这条就是空状态的唯一处理）。
    if (entry_list_ != nullptr) entry_list_->setVisible(!row_texts_.empty());
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
