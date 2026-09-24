#pragma once
// xrkit::shell —— 编码转换页（M2-6c2）。
//
// 页面只做三件事：把参数收上来、把引擎的结果画出来、把日志/状态栏写好。
// **判定链与有损编解码一行都不在这里**（`engine::text` 的 `sniff` / `convert`）——
// 这样"标签与判定不一致"那类事故（⑨9.7）在结构上就不可能再发生：
// 行文本里的定案步骤直接取 `ConvertOutcome::step_label`，页面没有能力自己拼一个。
//
// 对照表里属本页的口径：
//   · ⑨9.7 每行标出**链条的哪一步**做的决定（来自判定，不是复算）；
//   · ⑬13.5 空闲文案必须非空（`就绪`），⑬13.6 **暂停/继续状态机归 App**
//     （任务壳只管 running/线程/失败路径/进度）；
//   · ⑭14.3 所有按 96 DPI 定的像素都过 `core::px()`。

#include "page_base.h"

#include "xrkit/engine/text/convert.h"
#include "xrkit/engine/text/sniff.h"

#include <QString>

#include <atomic>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;

namespace xrkit::shell {

class TaskShell;
class VirtualList;

class ConvertPage : public PageBase {
public:
    explicit ConvertPage(QWidget* parent = nullptr);

    // 同步转换一个文件（界面与自检共用同一条入口）。
    // `out_dir` 为空 ⇒ 原地（此时只有 `needs_rewrite` 为真才写盘）。
    engine::text::ConvertOutcome convert_one(const QString& source_path, const QString& out_dir,
                                             const QString& source_enc, const QString& target_enc);

    // ⑬13.6：暂停状态机在 App 这边（任务壳不管它）。
    // 工作循环每一步都调 `wait_if_paused()`；被拦住就计数（自检据此证明"真的拦住了"）。
    void set_paused(bool paused);
    bool paused() const { return paused_.load(); }
    void toggle_pause();
    // 只在"暂停中"返回 false（并记一次拦截）。自检用非阻塞模式验证语义。
    bool wait_if_paused(bool block);

    void add_row(const QString& text);
    void refresh_rows();

    QComboBox* source_combo() const { return source_combo_; }
    QComboBox* target_combo() const { return target_combo_; }
    QLineEdit* source_edit() const { return source_edit_; }
    QLineEdit* output_edit() const { return output_edit_; }
    VirtualList* rows() const { return rows_; }
    TaskShell* task() const { return task_; }
    QLabel* status() const { return status_; }
    int pause_gate_hits() const { return pause_gate_hits_; }

    // 源编码下拉的取值："auto" 或某个编码名（内部键，与引擎同名）
    std::string current_source_encoding() const;
    std::string current_target_encoding() const;

private:
    void fill_encoding_combos();
    // 跑一批（走任务壳；工作线程里**不碰任何 UI**，只回投进度/消息）
    void run_batch(const std::vector<QString>& files, const QString& out_dir,
                   const QString& source_enc, const QString& target_enc);

    QLineEdit* source_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QComboBox* source_combo_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    VirtualList* rows_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<QString> row_texts_;
    std::atomic<bool> paused_{false};
    int pause_gate_hits_ = 0;
};

}  // namespace xrkit::shell
