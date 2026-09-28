#include "fs_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/squashfs.h"

#include "../drop_filter.h"
#include "../virtual_list.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMetaObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>

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
    // ★ 2026-09-28（用户："FS 的方便程度比 1.x 下降了不少，你回头看看 1.1.0"）——
    //   本页布局按 **1.x `apps/fs_app.py` 的控件组织**重排（照代码，不照印象）：
    //     · 包列表带自己的按钮行（加包/扫描目录/全选包/加载选中/取消加载/移除/清空 + 计数）
    //     · 条目列表带自己的行（搜索框 + 包内全选/全不选 + 计数）
    //     · 顶部一行：打包/解包格式 + **解包输出目录**（1.x 的「输出」行，含浏览/拖放/打开）
    //   ——1.x 的"方便"主要就来自这三块控件各自贴着自己的列表，而不是全塞进底部操作栏。
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
    //   几乎同义的话，是重复）。删完卡片里只剩：打包格式行 + 各列表自己的按钮行。
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("打包/解包格式：")), 0);
    row->addWidget(format_combo_, 0);
    row->addSpacing(xrkit::core::px(12));
    // 1.x 的「输入」行在这里换成了「解包到」：2.x 的包是"加进来"的（加包/拖放/扫描），
    // 所以缺的不是输入行而是**解包输出行**（原来写死 `<包路径>.out`，用户没法选）。
    row->addWidget(new QLabel(QStringLiteral("解包到：")), 0);
    out_edit_ = new QLineEdit;
    out_edit_->setObjectName(QStringLiteral("fs_output_dir"));
    out_edit_->setAcceptDrops(true);  // 6.4 同族：拖放挂在既有控件上
    out_edit_->setPlaceholderText(QStringLiteral("输出目录（留空＝与包同目录）"));
    row->addWidget(out_edit_, 1);
    auto* browse_out = new QPushButton(QStringLiteral("选择…"));
    browse_out->setObjectName(QStringLiteral("fs_browse_output"));
    row->addWidget(browse_out, 0);
    open_out_button_ = new QPushButton(QStringLiteral("打开"));
    open_out_button_->setObjectName(QStringLiteral("fs_open_output"));
    row->addWidget(open_out_button_, 0);
    add_button_ = new QPushButton(QStringLiteral("加包"));
    add_button_->setObjectName(QStringLiteral("fs_add_package"));
    row->addWidget(add_button_, 0);
    body_layout()->addLayout(row);

    // 包列表的按钮行（1.x `_build_db_panel` 的 bar：扫描目录/全选/加载选中/取消加载/移除/清空 + 计数）
    {
        auto* bar = new QHBoxLayout;
        scan_button_ = new QPushButton(QStringLiteral("扫描目录"));
        scan_button_->setObjectName(QStringLiteral("fs_scan_dir"));
        bar->addWidget(scan_button_, 0);
        select_all_button_ = new QPushButton(QStringLiteral("全选包"));
        select_all_button_->setObjectName(QStringLiteral("fs_select_all_packages"));
        bar->addWidget(select_all_button_, 0);
        auto* load_sel = new QPushButton(QStringLiteral("加载选中"));
        load_sel->setObjectName(QStringLiteral("fs_load_selected"));
        bar->addWidget(load_sel, 0);
        QObject::connect(load_sel, &QPushButton::clicked, this, [this] { load_selected(); });
        auto* unload_sel = new QPushButton(QStringLiteral("取消加载"));
        unload_sel->setObjectName(QStringLiteral("fs_unload_selected"));
        bar->addWidget(unload_sel, 0);
        QObject::connect(unload_sel, &QPushButton::clicked, this, [this] { unload_selected(); });
        auto* remove_sel = new QPushButton(QStringLiteral("移除"));
        remove_sel->setObjectName(QStringLiteral("fs_remove_packages"));
        bar->addWidget(remove_sel, 0);
        QObject::connect(remove_sel, &QPushButton::clicked, this, [this] { remove_selected(); });
        clear_button_ = new QPushButton(QStringLiteral("清空"));
        clear_button_->setObjectName(QStringLiteral("fs_clear_packages"));
        bar->addWidget(clear_button_, 0);
        bar->addStretch(1);
        package_count_label_ = new QLabel;
        package_count_label_->setObjectName(QStringLiteral("fs_package_count"));
        bar->addWidget(package_count_label_, 0);
        body_layout()->addLayout(bar);
    }
    body_layout()->addWidget(package_list_, 1);

    // 条目列表的按钮行（1.x `_build_file_panel`：全选/全不选 + 计数 + **搜索**）
    {
        auto* bar = new QHBoxLayout;
        check_all_button_ = new QPushButton(QStringLiteral("包内全选"));
        check_all_button_->setObjectName(QStringLiteral("fs_check_all_files"));
        bar->addWidget(check_all_button_, 0);
        check_none_button_ = new QPushButton(QStringLiteral("包内全不选"));
        check_none_button_->setObjectName(QStringLiteral("fs_check_no_files"));
        bar->addWidget(check_none_button_, 0);
        bar->addWidget(new QLabel(QStringLiteral("搜索")), 0);
        search_edit_ = new QLineEdit;
        search_edit_->setObjectName(QStringLiteral("fs_search"));
        search_edit_->setPlaceholderText(QStringLiteral("按路径过滤（边打边筛）"));
        bar->addWidget(search_edit_, 1);
        entry_count_label_ = new QLabel;
        entry_count_label_->setObjectName(QStringLiteral("fs_entry_count"));
        bar->addWidget(entry_count_label_, 0);
        body_layout()->addLayout(bar);
    }
    body_layout()->addWidget(entry_list_, 1);

    // ── ★ 1.x 的**封包面板**（`_build_pack_panel`）：这一整块是"FS 方便程度"的最后一块拼图
    //    （1.x 能"解包 → 改 → 重新封包"，2.x 原来只解不封）。布局照 1.x 三行：
    //    格式+输出+浏览+封包 / ＋文件＋目录 移除 清空 + 计数 / 待打包列表。
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("封包格式：")), 0);
        pack_format_combo_ = new QComboBox;
        pack_format_combo_->setObjectName(QStringLiteral("fs_pack_format"));
        for (const efs::FsFormatItem& item : efs::fs_formats()) {
            // 1.x 的封包下拉 = 非 auto 的全部格式（六种 DB + SquashFS）；取值仍是**内部键**
            pack_format_combo_->addItem(utf8(item.label), utf8(item.key));
        }
        row->addWidget(pack_format_combo_, 0);
        row->addWidget(new QLabel(QStringLiteral("封包输出：")), 0);
        pack_out_edit_ = new QLineEdit;
        pack_out_edit_->setObjectName(QStringLiteral("fs_pack_out"));
        pack_out_edit_->setPlaceholderText(QStringLiteral("输出文件（留空＝弹保存框）"));
        row->addWidget(pack_out_edit_, 1);
        auto* browse_pack = new QPushButton(QStringLiteral("选择…"));
        browse_pack->setObjectName(QStringLiteral("fs_pack_browse"));
        row->addWidget(browse_pack, 0);
        pack_button_ = new QPushButton(QStringLiteral("封包"));
        pack_button_->setObjectName(QStringLiteral("fs_pack"));
        row->addWidget(pack_button_, 0);
        body_layout()->addLayout(row);

        auto* bar = new QHBoxLayout;
        auto* add_files = new QPushButton(QStringLiteral("＋文件"));
        add_files->setObjectName(QStringLiteral("fs_pack_add_files"));
        bar->addWidget(add_files, 0);
        auto* add_dir = new QPushButton(QStringLiteral("＋目录"));
        add_dir->setObjectName(QStringLiteral("fs_pack_add_dir"));
        bar->addWidget(add_dir, 0);
        auto* rm = new QPushButton(QStringLiteral("移除"));
        rm->setObjectName(QStringLiteral("fs_pack_remove"));
        bar->addWidget(rm, 0);
        auto* clr = new QPushButton(QStringLiteral("清空"));
        clr->setObjectName(QStringLiteral("fs_pack_clear"));
        bar->addWidget(clr, 0);
        bar->addStretch(1);
        pack_count_label_ = new QLabel;
        pack_count_label_->setObjectName(QStringLiteral("fs_pack_count"));
        bar->addWidget(pack_count_label_, 0);
        body_layout()->addLayout(bar);

        pack_list_ = new QListWidget;
        pack_list_->setObjectName(QStringLiteral("fs_pack_list"));
        pack_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        pack_list_->setMinimumHeight(xrkit::core::px(90));
        body_layout()->addWidget(pack_list_, 1);

        QObject::connect(add_files, &QPushButton::clicked, this, [this] {
            const QStringList picked = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择要打进去的文件"), pack_base_dir(),
                QStringLiteral("所有文件 (*.*)"));
            if (!picked.isEmpty()) add_pack_files(picked);
        });
        QObject::connect(add_dir, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择要打进去的目录（递归，自动排除包文件）"), pack_base_dir());
            if (!dir.isEmpty()) add_pack_dir(dir);
        });
        QObject::connect(rm, &QPushButton::clicked, this, [this] { remove_pack_selected(); });
        QObject::connect(clr, &QPushButton::clicked, this, [this] { clear_pack(); });
        QObject::connect(browse_pack, &QPushButton::clicked, this, [this] {
            // 1.x 走 `_resolve_pack_target` 的保存框分支；这里显式选一次（选完回填）
            const QString picked = QFileDialog::getSaveFileName(
                this, QStringLiteral("封包输出文件"), pack_out_edit_->text(),
                QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
            if (!picked.isEmpty()) pack_out_edit_->setText(QDir::toNativeSeparators(picked));
        });
        QObject::connect(pack_button_, &QPushButton::clicked, this, [this] {
            QString summary;
            run_pack(&summary);
            log_message(summary);
        });
    }

    QObject::connect(add_button_, &QPushButton::clicked, this, [this] {
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要加载的包（可多选）"), QString(),
            QStringLiteral("包 (*.db *.db0 *.db1 *.db2 *.db3 *.db4 *.db5 *.db6 *.db7 *.db8 "
                           "*.db9 *.xdb *.sqfs *.sq_textures *.sq_levels *.sq_base *.sq_meshes);;"
                           "所有文件 (*.*)"));
        if (!picked.isEmpty()) add_paths(picked);
    });
    QObject::connect(browse_out, &QPushButton::clicked, this, [this] {
        const QString picked =
            QFileDialog::getExistingDirectory(this, QStringLiteral("选择解包输出目录"),
                                              out_edit_->text());
        if (!picked.isEmpty()) out_edit_->setText(QDir::toNativeSeparators(picked));
    });
    QObject::connect(open_out_button_, &QPushButton::clicked, this, [this] {
        const QString dir = resolve_output_dir();
        if (!dir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    // 6.4：拖放挂在**既有控件**上（包列表 + 卡片空白处），不另造 drop_zone
    drop_filter_ = new FileDropFilter([this](const QStringList& paths) { add_paths(paths); }, this);
    drop_filter_->attach(package_list_);
    drop_filter_->attach(card());

    // ── 操作栏（规则 2：贴底、不进视口）：1.x 顶部那两个解包入口
    unpack_checked_button_ = add_action(QStringLiteral("解包选中"));
    unpack_checked_button_->setObjectName(QStringLiteral("fs_unpack_checked"));
    unpack_all_button_ = add_action(QStringLiteral("批量解包"));
    unpack_all_button_->setObjectName(QStringLiteral("fs_unpack_all"));

    // 包列表勾选框（1.x `_db_toggle`：勾一个包 = 加载它 + 该包条目全勾）
    QObject::connect(package_list_, &QListWidget::itemChanged, this,
                     [this](QListWidgetItem* item) { on_package_item_changed(item); });
    QObject::connect(select_all_button_, &QPushButton::clicked, this, [this] {
        std::vector<std::string> visible;
        for (const Source& source : sources_) visible.push_back(source.key);
        selection_.select_all(visible);
        // 只动包列表：包内勾选**一个都不碰**（6.3 的两套状态）
        for (int i = 0; i < package_list_->count(); ++i) package_list_->item(i)->setSelected(true);
    });
    QObject::connect(check_all_button_, &QPushButton::clicked, this, [this] { check_all(true); });
    QObject::connect(check_none_button_, &QPushButton::clicked, this, [this] { check_all(false); });
    // 包列表那一行按钮（1.x `_build_db_panel` 的同名动作）
    QObject::connect(scan_button_, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("扫描目录里的包（递归）"), resolve_output_dir());
        if (!dir.isEmpty()) scan_dir(dir);
    });
    QObject::connect(clear_button_, &QPushButton::clicked, this, [this] { clear_all(); });
    // 搜索：边打边筛（1.x 的 `search_var.trace_add` 同一行为）
    QObject::connect(search_edit_, &QLineEdit::textChanged, this, [this] { refresh_rows(); });
    // 6.11：两个解包入口都走**同一条** `extract_to`（页面里 `extract_tree(` 只许出现一次）
    QObject::connect(unpack_checked_button_, &QPushButton::clicked, this, [this] {
        QString summary;
        unpack_selected(&summary);
        log_message(summary);
    });
    QObject::connect(unpack_all_button_, &QPushButton::clicked, this, [this] {
        QString summary;
        unpack_all(&summary);
        log_message(summary);
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
    Source record;
    record.path = source;
    record.key = source.toUtf8().toStdString();
    record.loaded = true;      // 「加包/拖放/散装层」进来的就是已加载（1.x 拖入目录=全部加载）
    sources_.push_back(record);
    entries_.push_back(entries);
    rebuild_package_list();
    rebuild_tree();
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

void FsPage::rebuild_tree() {
    // 合并树 = **已加载的那些源**按 `sources_` 顺序重放一遍（后者为准）。
    // 「取消加载/移除/清空」都走它 —— 不引入"从树里局部删节点"这种第二套逻辑。
    tree_ = efs::MergeTree{};
    loaded_order_.clear();
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source& source = sources_[i];
        if (!source.loaded) continue;
        efs::MergeInput input;
        input.source = source.key;
        input.entries = entries_[i];
        efs::MergeTree incoming = efs::build_tree(input);
        if (tree_.sources.empty()) {
            // `build_tree` 自己就把这个源写进 `sources`（**别再 push 一次**：多算一个会让
            // 下一个源的 `other_source_index` 变成 2，合并出来的节点 source_index 越界、
            // 界面上"这一份来自哪个包"就整列空了 —— 2026-09-28 实测踩过）
            tree_ = incoming;
        } else {
            // `merge` 会把对方的 sources 追加进来，序号 = 当前已加载源数
            tree_.merge(incoming, static_cast<int>(tree_.sources.size()));
        }
        loaded_order_.push_back(i);   // 合并树里的第 k 个源 ←→ sources_ 的第 i 个
    }
    refresh_counts();
    refresh_rows();
}

QString FsPage::source_path_for_index(int source_index) const {
    if (source_index < 0 || source_index >= static_cast<int>(loaded_order_.size())) return QString();
    const size_t row = loaded_order_[static_cast<size_t>(source_index)];
    if (row >= sources_.size()) return QString();
    return sources_[row].path;
}

int FsPage::loaded_index_for_source(size_t row) const {
    for (size_t i = 0; i < loaded_order_.size(); ++i) {
        if (loaded_order_[i] == row) return static_cast<int>(i);
    }
    return -1;
}

void FsPage::refresh_counts() {
    // 包计数（1.x `db_count`：`f"{len} 个"`；2.x 额外把"已加载几个"说出来）
    if (package_count_label_ != nullptr) {
        int loaded = 0;
        for (const Source& source : sources_) {
            if (source.loaded) ++loaded;
        }
        package_count_label_->setText(sources_.empty()
                                          ? QString()
                                          : QStringLiteral("%1 个（已加载 %2）")
                                                .arg(static_cast<int>(sources_.size()))
                                                .arg(loaded));
    }
    // 条目计数（1.x `entry_count`：`f"{nf} 文件"`；查看的是**过滤后**的数）
    if (entry_count_label_ != nullptr) {
        int files = 0;
        for (const efs::MergeNode* node : tree_.flatten()) {
            if (!node->is_dir && row_matches(node->path)) ++files;
        }
        entry_count_label_->setText(files == 0 ? QString() : QStringLiteral("%1 文件").arg(files));
    }
}

bool FsPage::row_matches(const std::string& path) const {
    // 1.x `_render`：`q = search.lower().replace("\\","/")`，命中即显示
    if (search_edit_ == nullptr) return true;
    const QString query = search_edit_->text().trimmed().toLower().replace(QLatin1Char('\\'),
                                                                          QLatin1Char('/'));
    if (query.isEmpty()) return true;
    return utf8(path).toLower().contains(query);
}

void FsPage::refresh_rows() {
    row_texts_.clear();
    for (const efs::MergeNode* node : tree_.flatten()) {
        if (!row_matches(node->path)) continue;   // 搜索过滤（1.x 同一行为）
        QString text = utf8(node->path);
        if (node->is_dir) {
            text += QStringLiteral("/");
        } else if (!source_path_for_index(node->source_index).isEmpty()) {
            // "这一份来自哪个包" = 引擎实际会读到的那一份（6.1）
            text += QStringLiteral("　←　") + source_path_for_index(node->source_index);
        }
        if (node->checked) text += QStringLiteral("　[勾选]");
        row_texts_.push_back(text);
    }
    entry_list_->set_row_count(static_cast<int>(row_texts_.size()));
    // ★ 一个包都没加载时（没有任何条目）**条目列表整个收起**，空间全给包列表：
    //   它就是拖放落点、也是"已加载哪些包"的唯一显示处 —— 空着的条目框纯属占地方
    //   （用户真机反馈「比例太丑」的原话场景；空状态块删掉后这条就是空状态的唯一处理）。
    if (entry_list_ != nullptr) entry_list_->setVisible(!row_texts_.empty());
    refresh_counts();
}

QString FsPage::output_dir() const {
    return out_edit_ != nullptr ? out_edit_->text().trimmed() : QString();
}

QString FsPage::resolve_output_dir() {
    // 1.x `_resolve_output`：空则按"第一个包所在目录"推断，并**回填**输入框
    //（界面必须反映真实落盘位置 —— 1.x 的 H1 就是"框里空着，实际落到别处"）。
    QString out = output_dir();
    if (out.isEmpty()) {
        if (!sources_.empty()) {
            out = QFileInfo(sources_.front().path).absolutePath();
        } else {
            out = QDir::toNativeSeparators(QDir::currentPath());
        }
        if (out_edit_ != nullptr) out_edit_->setText(QDir::toNativeSeparators(out));
    }
    return out;
}

QString FsPage::package_folder_name(const QString& package_path) {
    // 1.x `_package_folder`：`os.path.splitext(os.path.basename(p))[0]`
    //   gamedata.db0 → gamedata；gamedata.sq_base → gamedata；a.b.db0 → a.b
    return QFileInfo(package_path).completeBaseName();
}

int FsPage::unpack_selected(QString* summary) {
    // 1.x `_unpack_checked`：只解**勾选的**文件，按"条目归属哪个包"分组逐包导出，
    // 每包落到 `<输出>/<包名去掉扩展名>/`（1.x `os.path.join(out, _package_folder(src))`）。
    const QString out_root = resolve_output_dir();
    int written = 0;
    int failed = 0;
    QStringList per_package;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (!sources_[i].loaded) continue;
        const int merged_index = loaded_index_for_source(i);
        std::vector<std::string> only;
        for (const efs::MergeNode* node : tree_.flatten()) {
            if (node->is_dir || !node->checked) continue;
            if (node->source_index != merged_index) continue;
            only.push_back(node->path);
        }
        if (only.empty()) continue;
        const QString image = sources_[i].path;
        const QString target =
            QDir(out_root).filePath(package_folder_name(image));
        const efs::ExtractTreeResult result =
            extract_to(image, target, efs::Selection::Listed, only);
        written += static_cast<int>(result.files_written);
        failed += static_cast<int>(result.failures.size());
        per_package << QStringLiteral("%1→%2（%3 文件%4）")
                           .arg(QFileInfo(image).fileName(), package_folder_name(image))
                           .arg(result.files_written)
                           .arg(result.ok ? QString()
                                          : QStringLiteral("，失败：") + utf8(result.error));
    }
    if (summary != nullptr) {
        *summary = per_package.isEmpty()
                       ? QStringLiteral("没有勾选任何条目（包内全选/勾单个文件后再解）")
                       : QStringLiteral("解包选中：%1 个文件、失败 %2 → %3\n  %4")
                             .arg(written)
                             .arg(failed)
                             .arg(out_root)
                             .arg(per_package.join(QStringLiteral("\n  ")));
    }
    return written;
}

int FsPage::unpack_all(QString* summary) {
    // 1.x `_unpack_batch`：**所有已加载的包**整包导出（不看勾选）
    const QString out_root = resolve_output_dir();
    int written = 0;
    int failed = 0;
    QStringList per_package;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (!sources_[i].loaded) continue;
        const QString image = sources_[i].path;
        const QString target = QDir(out_root).filePath(package_folder_name(image));
        const efs::ExtractTreeResult result =
            extract_to(image, target, efs::Selection::All);
        written += static_cast<int>(result.files_written);
        failed += static_cast<int>(result.failures.size());
        per_package << QStringLiteral("%1→%2（%3 文件%4）")
                           .arg(QFileInfo(image).fileName(), package_folder_name(image))
                           .arg(result.files_written)
                           .arg(result.ok ? QString()
                                          : QStringLiteral("，失败：") + utf8(result.error));
    }
    if (summary != nullptr) {
        *summary = per_package.isEmpty()
                       ? QStringLiteral("没有已加载的包（先「加包」/「扫描目录」并加载）")
                       : QStringLiteral("批量解包：%1 个文件、失败 %2 → %3\n  %4")
                             .arg(written)
                             .arg(failed)
                             .arg(out_root)
                             .arg(per_package.join(QStringLiteral("\n  ")));
    }
    return written;
}

int FsPage::scan_dir(const QString& dir) {
    // 1.x `_scan_dir`：递归找 `_is_db` / `_is_sq` 命中的文件，加进**已知包列表**（不自动加载），
    // 按 basename 小写排序（6.2 的加载序同源）。
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return 0;
    QStringList found;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (efs::is_package_file_name(path.toUtf8().toStdString())) found << path;
    }
    found.sort(Qt::CaseInsensitive);
    int added = 0;
    for (const QString& path : found) {
        bool known = false;
        for (const Source& source : sources_) {
            if (source.path.compare(path, Qt::CaseInsensitive) == 0) known = true;
        }
        if (known) continue;
        Source record;
        record.path = path;
        record.key = path.toUtf8().toStdString();
        record.loaded = false;      // 扫进来 ≠ 加载；由「加载选中」或勾选决定
        sources_.push_back(record);
        entries_.emplace_back();
        ++added;
    }
    // 列表按 basename 排序显示（与 6.2 的"后者为准"顺序一致，免得显示顺序与合并顺序打架）
    std::vector<size_t> order = efs::sort_sources_by_basename([this] {
        std::vector<std::string> paths;
        for (const Source& source : sources_) paths.push_back(source.key);
        return paths;
    }());
    std::vector<Source> sorted_sources;
    std::vector<std::vector<std::pair<std::string, bool>>> sorted_entries;
    for (size_t index : order) {
        sorted_sources.push_back(sources_[index]);
        sorted_entries.push_back(entries_[index]);
    }
    sources_.swap(sorted_sources);
    entries_.swap(sorted_entries);
    rebuild_package_list();
    rebuild_tree();
    log_message(QStringLiteral("扫描目录：新增 %1 个包，共 %2 个（未加载）")
                    .arg(added)
                    .arg(static_cast<int>(sources_.size())));
    return added;
}

void FsPage::rebuild_package_list() {
    // 包列表每行：勾选框 + basename + [已加载]/[未加载] + 大小。
    // ★ 勾选框 = 该包在合并树里的**整包勾选态**（1.x `_db_chk_state` 的 partial-aware 三态），
    //   勾上/取消 = 该包贡献的条目全勾/全不勾（1.x `_db_toggle` ⇒ 引擎 `toggle_source`）。
    rebuilding_package_list_ = true;
    int selected_row = -1;
    for (int i = 0; i < package_list_->count(); ++i) {
        if (package_list_->item(i)->isSelected()) selected_row = i;
    }
    package_list_->clear();
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source& source = sources_[i];
        const QFileInfo info(source.path);
        const QString state = source.loaded ? QStringLiteral("[已加载]")
                                            : QStringLiteral("[未加载]");
        auto* item = new QListWidgetItem(QStringLiteral("%1　%2　%3 B")
                                             .arg(info.fileName(), state)
                                             .arg(info.size()));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        Qt::CheckState check = Qt::Unchecked;
        if (source.loaded) {
            int files = 0;
            int checked = 0;
            tree_.source_check_state(loaded_index_for_source(i), &files, &checked);
            check = checked == 0 ? Qt::Unchecked
                                 : (checked == files ? Qt::Checked : Qt::PartiallyChecked);
        }
        item->setCheckState(check);
        package_list_->addItem(item);
    }
    rebuilding_package_list_ = false;
    if (selected_row >= 0 && selected_row < package_list_->count()) {
        package_list_->item(selected_row)->setSelected(true);
    }
    refresh_counts();
}

void FsPage::on_package_item_changed(QListWidgetItem* item) {
    if (rebuilding_package_list_ || item == nullptr) return;   // 重建列表时不要把状态回灌
    const int row = package_list_->row(item);
    if (row < 0 || row >= static_cast<int>(sources_.size())) return;
    const bool want = item->checkState() != Qt::Unchecked;
    if (!sources_[static_cast<size_t>(row)].loaded) {
        // 1.x `_db_toggle`：勾选一个**未加载**的包 ⇒ 先加载，再整包勾上
        if (!want) return;
        package_list_->item(row)->setSelected(true);
        load_selected();
        rebuild_package_list();
    }
    const int merged = loaded_index_for_source(static_cast<size_t>(row));
    if (merged < 0) return;
    tree_.toggle_source(merged, want);
    refresh_package_checks();
    refresh_rows();
}

void FsPage::refresh_package_checks() {
    rebuilding_package_list_ = true;
    for (int row = 0; row < package_list_->count() && row < static_cast<int>(sources_.size());
         ++row) {
        int files = 0;
        int checked = 0;
        if (sources_[static_cast<size_t>(row)].loaded) {
            tree_.source_check_state(loaded_index_for_source(static_cast<size_t>(row)), &files,
                                     &checked);
        }
        QListWidgetItem* item = package_list_->item(row);
        item->setCheckState(checked == 0 ? Qt::Unchecked
                                        : (checked == files ? Qt::Checked : Qt::PartiallyChecked));
    }
    rebuilding_package_list_ = false;
}

int FsPage::load_selected() {
    // 1.x `_load_selected`：只加载**选中的**包，勾选态不碰
    int loaded = 0;
    const QList<QListWidgetItem*> chosen = package_list_->selectedItems();
    for (QListWidgetItem* item : chosen) {
        const int row = package_list_->row(item);
        if (row < 0 || row >= static_cast<int>(sources_.size())) continue;
        if (sources_[static_cast<size_t>(row)].loaded) continue;
        QString gated = sources_[static_cast<size_t>(row)].path;
        QString gate_error;
        if (!gate_source(gated, &gate_error)) {
            const QString why = QStringLiteral("拒绝加载（需要解密插件）：%1 —— %2")
                                    .arg(sources_[static_cast<size_t>(row)].path, gate_error);
            log_message(why);
            load_errors_ << why;
            continue;
        }
        std::string error;
        std::vector<std::pair<std::string, bool>> entries;
        if (QFileInfo(gated).isDir()) {
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error);
        } else {
            entries = efs::package_entries(gated.toUtf8().toStdString(),
                                           efs::format_key_to_db(current_format_key()), error);
        }
        if (!error.empty()) {
            const QString why = QStringLiteral("加载失败：%1 —— %2")
                                    .arg(sources_[static_cast<size_t>(row)].path, utf8(error));
            log_message(why);
            load_errors_ << why;
            continue;
        }
        sources_[static_cast<size_t>(row)].path = gated;
        sources_[static_cast<size_t>(row)].key = gated.toUtf8().toStdString();
        sources_[static_cast<size_t>(row)].loaded = true;
        entries_[static_cast<size_t>(row)] = std::move(entries);
        ++loaded;
    }
    rebuild_package_list();
    rebuild_tree();
    log_message(QStringLiteral("加载选中：%1 个（共 %2 个已加载）")
                    .arg(loaded)
                    .arg(static_cast<int>(std::count_if(
                        sources_.begin(), sources_.end(),
                        [](const Source& s) { return s.loaded; }))));
    return loaded;
}

int FsPage::unload_selected() {
    // 1.x `_unload_selected`：只改"加载"状态，勾选态跟随原节点（merge 的既有语义）
    int unloaded = 0;
    for (QListWidgetItem* item : package_list_->selectedItems()) {
        const int row = package_list_->row(item);
        if (row < 0 || row >= static_cast<int>(sources_.size())) continue;
        if (!sources_[static_cast<size_t>(row)].loaded) continue;
        sources_[static_cast<size_t>(row)].loaded = false;
        ++unloaded;
    }
    rebuild_package_list();
    rebuild_tree();
    return unloaded;
}

int FsPage::remove_selected() {
    std::vector<size_t> doomed;
    for (QListWidgetItem* item : package_list_->selectedItems()) {
        const int row = package_list_->row(item);
        if (row >= 0 && row < static_cast<int>(sources_.size())) {
            doomed.push_back(static_cast<size_t>(row));
        }
    }
    std::sort(doomed.begin(), doomed.end(), std::greater<size_t>());
    for (size_t row : doomed) {
        selection_.toggle(sources_[row].key, false);
        sources_.erase(sources_.begin() + static_cast<long>(row));
        entries_.erase(entries_.begin() + static_cast<long>(row));
    }
    rebuild_package_list();
    rebuild_tree();
    return static_cast<int>(doomed.size());
}

void FsPage::clear_all() {
    sources_.clear();
    entries_.clear();
    selection_ = efs::FsSelection{};
    package_list_->clear();
    rebuild_tree();   // 空树 + 刷计数/行
}

// ═══ 封包（1.x `_build_pack_panel` / `_pack` 一族） ═══════════════════════

QString FsPage::pack_base_dir() const {
    // 1.x `_pack_root`：设了输入目录就以它为根（相对路径都相对它算）。
    // 2.x 没有"输入行"，就用「解包到」那一格当根（语义相同：产物相对它归一）。
    return output_dir();
}

int FsPage::add_pack_files(const QStringList& paths) {
    std::vector<std::string> roots;
    for (const QString& path : paths) roots.push_back(path.toUtf8().toStdString());
    const efs::PackCollectResult collected =
        efs::collect_pack_files(pack_base_dir().toUtf8().toStdString(), roots);
    int added = 0;
    for (const efs::PackFileItem& item : collected.items) {
        bool known = false;
        for (const efs::PackFileItem& have : pack_items_) {
            if (have.rel == item.rel) known = true;
        }
        if (known) continue;
        pack_items_.push_back(item);
        ++added;
    }
    refresh_pack_list();
    log_message(QStringLiteral("＋文件：%1 个（排除包文件 %2 个，不在根之下 %3 个）")
                    .arg(added)
                    .arg(collected.skipped_package_files)
                    .arg(collected.skipped_outside_base));
    return added;
}

int FsPage::add_pack_dir(const QString& dir) {
    // 1.x `_add_dir`：base = 输入目录；没设就把"这个目录的父目录"当 base，
    // 这样**目录本身**会成为包里的顶层（用户视角：拖进去的那个文件夹还在）。
    QString base = pack_base_dir();
    if (base.isEmpty()) base = QFileInfo(dir).absolutePath();
    const std::vector<std::string> roots{dir.toUtf8().toStdString()};
    const efs::PackCollectResult collected =
        efs::collect_pack_files(base.toUtf8().toStdString(), roots);
    int added = 0;
    for (const efs::PackFileItem& item : collected.items) {
        bool known = false;
        for (const efs::PackFileItem& have : pack_items_) {
            if (have.rel == item.rel) known = true;
        }
        if (known) continue;
        pack_items_.push_back(item);
        ++added;
    }
    refresh_pack_list();
    log_message(QStringLiteral("＋目录：%1 个（排除包文件 %2 个，不在根之下 %3 个）")
                    .arg(added)
                    .arg(collected.skipped_package_files)
                    .arg(collected.skipped_outside_base));
    return added;
}

int FsPage::remove_pack_selected() {
    QSet<QString> doomed;
    for (QListWidgetItem* item : pack_list_->selectedItems()) doomed.insert(item->text());
    if (doomed.isEmpty()) return 0;
    const int before = static_cast<int>(pack_items_.size());
    std::vector<efs::PackFileItem> keep;
    for (const efs::PackFileItem& item : pack_items_) {
        if (doomed.contains(utf8(item.rel))) continue;
        keep.push_back(item);
    }
    pack_items_.swap(keep);
    refresh_pack_list();
    return before - static_cast<int>(pack_items_.size());
}

void FsPage::clear_pack() {
    pack_items_.clear();
    refresh_pack_list();
}

void FsPage::refresh_pack_list() {
    pack_list_->clear();
    for (const efs::PackFileItem& item : pack_items_) pack_list_->addItem(utf8(item.rel));
    if (pack_count_label_ != nullptr) {
        pack_count_label_->setText(pack_items_.empty()
                                       ? QString()
                                       : QStringLiteral("%1 个文件")
                                             .arg(static_cast<int>(pack_items_.size())));
    }
}

QString FsPage::pack_output_basename() const {
    // 1.x `_pack_output_basename`（"输入目录"那一支在 2.x 没有 ⇒ 走后面两支）
    QStringList tops;
    for (const efs::PackFileItem& item : pack_items_) {
        const QString top = utf8(item.rel).section(QLatin1Char('/'), 0, 0);
        if (!top.isEmpty() && !tops.contains(top)) tops << top;
    }
    if (tops.size() != 1) return QString();
    const QString top = tops.front();
    if (pack_items_.size() == 1 && !utf8(pack_items_.front().rel).contains(QLatin1Char('/'))) {
        return QFileInfo(top).completeBaseName();   // 单文件：去掉原扩展名
    }
    return top;                                      // 目录：保留目录名
}

QString FsPage::pack_ext_for(const QString& key) {
    // 1.x `_pack_ext`：sqfs ⇒ `.sqfs`；其余 ⇒ `.<内部键>.db`（六种格式产物不同名，
    // 否则用户在资源管理器里分不出打的是哪种格式）
    if (efs::format_key_is_sqfs(key.toUtf8().toStdString())) return QStringLiteral(".sqfs");
    return QStringLiteral(".") + key + QStringLiteral(".db");
}

QString FsPage::resolve_pack_target() {
    const std::string key = pack_format_combo_->currentData().toString().toUtf8().toStdString();
    const QString ext = pack_ext_for(utf8(key));
    const QString base = pack_output_basename();
    QString out = pack_out_edit_->text().trimmed();
    if (out.isEmpty()) {
        const QString initial =
            base.isEmpty() ? QStringLiteral("packed") + ext : base + ext;
        out = QFileDialog::getSaveFileName(this, QStringLiteral("封包保存到"), initial,
                                           QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
        if (out.isEmpty()) return QString();
    } else if (QFileInfo(out).isDir()) {
        if (!base.isEmpty()) {
            out = QDir(out).filePath(base + ext);
        } else {
            out = QFileDialog::getSaveFileName(this, QStringLiteral("封包保存到"),
                                               QDir(out).filePath(QStringLiteral("packed") + ext),
                                               QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
            if (out.isEmpty()) return QString();
        }
    } else if (QFileInfo(out).suffix().isEmpty()) {
        out += ext;
    }
    pack_out_edit_->setText(QDir::toNativeSeparators(out));   // 1.x H1：最终路径必须回填
    return out;
}

bool FsPage::run_pack(QString* summary) {
    const auto set_summary = [summary](const QString& text) {
        if (summary != nullptr) *summary = text;
    };
    if (pack_items_.empty()) {
        set_summary(QStringLiteral("封包：列表是空的（先「＋文件」或「＋目录」）"));
        return false;
    }
    const QString out = resolve_pack_target();
    if (out.isEmpty()) {
        set_summary(QStringLiteral("封包：没选输出文件，已取消"));
        return false;
    }
    const std::string key = pack_format_combo_->currentData().toString().toUtf8().toStdString();
    if (efs::format_key_is_sqfs(key)) {
        std::vector<efs::PackItem> items;
        items.reserve(pack_items_.size());
        for (const efs::PackFileItem& item : pack_items_) {
            efs::PackItem pack_item;
            pack_item.path = item.rel;
            pack_item.is_dir = false;
            pack_item.source_path = item.source_path;
            items.push_back(pack_item);
        }
        const efs::PackResult result = efs::pack_squashfs(out.toUtf8().toStdString(), items);
        set_summary(result.ok ? QStringLiteral("封包完成（SquashFS）：%1 个文件、%2 字节 → %3")
                                    .arg(static_cast<int>(items.size()))
                                    .arg(result.file_size)
                                    .arg(out)
                              : QStringLiteral("封包失败（SquashFS）：%1").arg(utf8(result.error)));
        return result.ok;
    }
    const std::optional<efs::DbFormat> fmt = efs::format_key_to_db(key);
    if (!fmt.has_value()) {
        set_summary(QStringLiteral("封包：格式键 %1 既不是 DB 也不是 SquashFS").arg(utf8(key)));
        return false;
    }
    std::vector<efs::DbPackItem> items;
    items.reserve(pack_items_.size());
    for (const efs::PackFileItem& item : pack_items_) {
        std::ifstream in(core::path_from_utf8(item.source_path), std::ios::binary);
        if (!in) {
            set_summary(QStringLiteral("封包失败：读不出 %1").arg(utf8(item.source_path)));
            return false;
        }
        efs::DbPackItem pack_item;
        pack_item.path = item.rel;
        pack_item.is_dir = false;
        pack_item.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        items.push_back(std::move(pack_item));
    }
    const std::vector<uint8_t> raw = efs::pack_db(items, *fmt);
    std::string write_error;
    if (!core::write_file_atomic(out.toUtf8().toStdString(), raw, true, write_error)) {
        set_summary(QStringLiteral("封包失败（写盘）：%1").arg(utf8(write_error)));
        return false;
    }
    set_summary(QStringLiteral("封包完成（%1）：%2 个文件、%3 字节 → %4")
                    .arg(utf8(efs::db_format_key(*fmt)))
                    .arg(static_cast<int>(items.size()))
                    .arg(raw.size())
                    .arg(out));
    return true;
}

efs::ExtractTreeResult FsPage::extract_to(const QString& image_path, const QString& out_root,
                                          efs::Selection selection_kind,
                                          const std::vector<std::string>& only) {
    // ★ 这里是本页**唯一**调用引擎导出的地方（门禁静态锁：整个页面文件里
    //   `extract_tree(` 只许出现一次）—— 6.11 的"两条路合一"就靠它。
    // ★ 这里是本页**唯一**调用引擎导出的地方（门禁静态锁：整个页面文件里只许出现一次）
    //   —— 6.11 的"两条路合一"就靠它。`extract_package` 内部按包类型分派
    //   （SquashFS / X-Ray DB 六格式），所以"DB 包也要能解"和"只有一条路"能同时成立。
    efs::ExtractTreeOptions options;
    options.selection = selection_kind;
    options.only = only;
    return efs::extract_package(image_path.toUtf8().toStdString(), out_root.toUtf8().toStdString(),
                                options, efs::format_key_to_db(current_format_key()));
}

}  // namespace xrkit::shell
