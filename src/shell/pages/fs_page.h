#pragma once
// xrkit::shell —— 文件系统页（M2-6b2）。
//
// 页面只做三件事：把模型画出来、把用户动作翻译成模型调用、把结果写进日志/状态栏。
// **一条算法都不在这里**（合并/路径判据/导出都在 `engine::fs`）—— 这是 1.x 最贵的一条
// 教训：同一件事在 App 与引擎里各写一份，改一处漏一处。
//
// 本页专门收着对照表 ⑥ 的几条"界面形态"判据：
//   · 6.3 `selected`（包列表选中）与 `checked`（包内文件勾选）**两套状态两个按钮**；
//   · 6.4 **没有 `drop_zone` 控件**：拖放直接注册在既有控件上（1.x 明文写着）；
//   · 6.11 选中解包与批量解包**同一个入口**（`extract_to`），不再有两条计数不同的路；
//   · 6.13 格式下拉的**取值是内部键**（显示名只用于展示）。

#include "page_base.h"

#include "xrkit/engine/fs/extract_tree.h"
#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/fs/overlay.h"

#include <QString>

#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace xrkit::shell {

class VirtualList;

class FsPage : public PageBase {
public:
    explicit FsPage(QWidget* parent = nullptr);

    // 载入一个包（真程序里由拖放/文件对话框给路径；自检直接喂条目清单）。
    // 6.2：加载顺序按 basename 小写升序 —— 页面**不自己排**，交给模型。
    void add_source(const QString& source,
                    const std::vector<std::pair<std::string, bool>>& entries);
    // 散装层（用户 2026-09-26"最后并入层"）：把一个**磁盘目录**当"包"加进来 ——
    // 条目表由 `engine::fs::loose_entries` 产出，走的是**同一个** `add_source`，
    // 所以"同名条目以后加载的为准"对散装层与包完全一致（不出现第二套合并逻辑）。
    bool add_loose_dir(const QString& dir, QString* error);

    int source_count() const { return static_cast<int>(sources_.size()); }
    // 空状态：包内条目区在**没有条目**时显示一句提示（用户实测：空列表只有一条细线，
    // 看着像坏了）。自检用这个访问器断言：有货就藏、没货就显示。
    bool entry_hint_visible() const;
    // ★ 排版口径（真机反馈'太空'）：未加载包时的**空状态占位块**（居中，吃满卡片）
    QWidget* empty_block() const { return empty_block_; }
    QString source_at(int index) const;
    int conflict_count() const { return tree_.conflict_count(); }

    // 6.3：两套状态 —— 包列表选中态归「全选/加载选中/取消加载/移除」用
    engine::fs::FsSelection& selection() { return selection_; }
    // 包内勾选态归「全选/全不选/解包」用
    void check_all(bool checked);
    int checked_file_count() const { return tree_.checked_file_count(); }

    // 6.11：**唯一**的导出入口。选中集（Listed）与批量（All）走同一条路、同一套计数。
    // 1.x 是"逐项 --cat（300 s/项）"与"整镜像 --unpack-path（3600 s）"两条路，
    // 成功计数与文案还不同源。
    // ⑫12.1 decryptor 消费端（用户 2026-09-26 决策：NLC 解密是**外部闭源插件**，
    // 工具内不放解密逻辑）。加载包之前问一遍插件解密器：
    //   · 没有解密器命中 ⇒ 原样放行（行为完全不变）；
    //   · 命中并解开 ⇒ 路径换成解密产物，照常加载；
    //   · 命中却解不开 ⇒ **拒绝加载**并把理由显示出来（绝不静默当普通包读）。
    void set_registry(const plugin::Registry* registry) { registry_ = registry; }
    bool gate_source(QString& path_in_out, QString* error_out);
    QString last_gate_error() const { return last_gate_error_; }
    void log_message(const QString& text);

    engine::fs::ExtractTreeResult extract_to(const QString& image_path, const QString& out_root,
                                            engine::fs::Selection selection_kind,
                                            const std::vector<std::string>& only = {});

    QComboBox* format_combo() const { return format_combo_; }
    QListWidget* package_list() const { return package_list_; }
    VirtualList* entry_list() const { return entry_list_; }
    QPushButton* select_all_button() const { return select_all_button_; }
    QPushButton* check_all_button() const { return check_all_button_; }
    QPushButton* unpack_button() const { return unpack_button_; }

    // 6.13 的判据：下拉每一项的 `data` 必须是内部键，且显示名解析不出格式
    void fill_format_combo();
    // 当前选中的格式键（内部键；"sqfs" 表示走 SquashFS 路径）
    std::string current_format_key() const;

    // 把模型的一行行刷进虚拟列表（行文本带"这一份来自哪个包"）
    void refresh_rows();

private:
    struct Source {
        QString path;
        std::string key;
    };

    engine::fs::MergeTree tree_;
    engine::fs::FsSelection selection_;
    std::vector<Source> sources_;
    std::vector<QString> row_texts_;
    const plugin::Registry* registry_ = nullptr;
    QString decrypt_dir_;
    QString last_gate_error_;

    QListWidget* package_list_ = nullptr;
    QComboBox* format_combo_ = nullptr;
    VirtualList* entry_list_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* entry_hint_ = nullptr;
    QWidget* empty_block_ = nullptr;  // 空状态占位块（居中，吃满卡片）
    QPushButton* select_all_button_ = nullptr;
    QPushButton* check_all_button_ = nullptr;
    QPushButton* unpack_button_ = nullptr;
};

}  // namespace xrkit::shell
