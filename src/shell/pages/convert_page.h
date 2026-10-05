#pragma once
// xrkit::shell —— 编码转换页（M2-6c2 + 2026-10-06 D2 批"9 处不必要阉割"补齐）。
//
// 页面只做三件事：把参数收上来、把引擎的结果画出来、把日志/状态栏写好。
// **判定链与有损编解码一行都不在这里**（`engine::text` 的 `sniff` / `convert`）；
// **批处理（遍历 / 排除 / 备份三件 / 统计 CSV）也一行都不在这里**（`engine::text::batch`）
// —— 这样"标签与判定不一致"那类事故（⑨9.7）在结构上就不可能再发生：
// 行文本里的定案步骤直接取 `ConvertOutcome::step_label`，页面没有能力自己拼一个。
//
// 对照表里属本页的口径：
//   · ⑨9.7 每行标出**链条的哪一步**做的决定（来自判定，不是复算）；
//   · ⑬13.5 空闲文案必须非空（`就绪`），⑬13.6 **暂停/继续状态机归 App**
//     （任务壳只管 running/线程/失败路径/进度）；
//   · ⑭14.3 所有按 96 DPI 定的像素都过 `core::px()`。
//   · ★ D2 批：`--` 递归子目录（默认开，且**报扫了几层/多少文件**）/ 扩展名过滤 /
//     排除模式 / 备份到 `源目录\backup\<相对路径>.bak` / 恢复备份 / 清空备份 /
//     打开输出 / 导出统计（utf-8-sig）—— 全部由 `engine::text::batch` 实现。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/text/batch.h"
#include "xrkit/engine/text/convert.h"
#include "xrkit/engine/text/sniff.h"

#include <QString>

#include <atomic>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace xrkit::shell {

class TaskShell;
class VirtualList;
class ScrollViewport;

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
    // ★★ 2026-10-05（用户报告 #6「编码转换的中间那块区域并没有设计滚动条。」）：
    //   结果区（行列表）外面那层**页内视口** —— 自检拿它断言"能滚（maximum>0）/ 装得下不占位"，
    //   `--shot` 拿它量几何。修前没有它（`addWidget(rows_, 1)` 直接放进卡片）⇒ 那条判据红。
    ScrollViewport* rows_viewport() const { return rows_viewport_; }
    // 行数变了 ⇒ 把内容高同步给那层视口（自检直接驱它，避免"只有跑完一批才生效"）
    void sync_rows_viewport() { sync_inner_scroll(); }
    TaskShell* task() const { return task_; }
    QLabel* status() const { return status_; }
    int pause_gate_hits() const { return pause_gate_hits_; }

    // ── ★ 2026-10-06 D2 批的新控件（自检逐条断言；`--shot` 也按对象名量几何）──
    QLineEdit* ext_edit() const { return ext_edit_; }
    QCheckBox* recursive_box() const { return recursive_box_; }
    QCheckBox* backup_box() const { return backup_box_; }
    QLineEdit* exclude_edit() const { return exclude_edit_; }
    QPushButton* start_button() const { return start_button_; }
    QPushButton* pause_button() const { return pause_button_; }
    QPushButton* restore_button() const { return restore_button_; }
    QPushButton* clear_backup_button() const { return clear_backup_button_; }
    QPushButton* open_output_button() const { return open_output_button_; }
    QPushButton* export_button() const { return export_button_; }

    // 上一次批处理的**扫描台账**（"递归了几层 / 扫到多少文件 / 各自为什么被刷掉"）。
    // 零静默那条纪律要求它一定非空；自检读它，而不是把日志串反解析一遍。
    QString last_scan_text() const { return last_scan_text_; }
    // 上一次批处理的结果（导出统计与自检都读它）
    const engine::text::BatchOutcome& last_outcome() const { return last_outcome_; }
    bool stats_ready() const { return stats_ready_; }

    // 从当前界面取值（自检与导出统计共用同一份口径，不再各读一遍）
    QStringList extensions() const;
    QStringList backup_extensions() const;
    std::string current_source_encoding() const;
    std::string current_target_encoding() const;
    engine::text::BatchOptions batch_options_for(const QString& source, const QString& out_dir) const;

    // 「恢复备份」「清空备份」「打开输出」「导出统计」（1.x 同名按钮）
    void run_restore();
    void run_clear_backup();
    void open_output();
    bool export_stats(const QString& path, QString* problem);
    // 跑一批（走任务壳；工作线程里**不碰任何 UI**，只回投进度/消息）
    void run_batch(const QString& source, const QString& out_dir);

    // ★ 2026-10-04（用户第 2 条"目录定向"）：各"选择"调用点的**起点判据**声明（算法只有
    //   `dialog_start.h` 那一份；页面只声明"哪个输入是它的来源"）。自检据此逐条断言。
    DialogStart source_dialog_start() const;
    DialogStart output_dialog_start() const;

private:
    void fill_encoding_combos();
    void sync_backup_enabled();       // 输出目录非空 ⇒ 备份勾选框禁用且取消勾选（1.x 同）
    // 批处理跑完（**主线程**）把结果落到成员上：导出统计与自检都读这份快照
    void record_stats(const engine::text::BatchOutcome& outcome, const QString& source,
                      const QString& out_dir, const QString& source_enc, const QString& target_enc);
    void set_busy(bool busy);         // 三颗"起任务"的按钮忙时禁用（1.x 三者互斥）

    QLineEdit* source_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QLineEdit* ext_edit_ = nullptr;
    QLineEdit* exclude_edit_ = nullptr;
    QCheckBox* recursive_box_ = nullptr;
    QCheckBox* backup_box_ = nullptr;
    QComboBox* source_combo_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* pause_button_ = nullptr;
    QPushButton* restore_button_ = nullptr;
    QPushButton* clear_backup_button_ = nullptr;
    QPushButton* open_output_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    VirtualList* rows_ = nullptr;
    // ★ 2026-10-05（#6）：结果区的页内视口（由 `PageBase::make_inner_scroll()` 造）
    ScrollViewport* rows_viewport_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<QString> row_texts_;
    std::atomic<bool> paused_{false};
    int pause_gate_hits_ = 0;
    QString last_scan_text_;
    engine::text::BatchOutcome last_outcome_;
    // 产生`last_outcome_`那一次的**参数快照**（1.x 的 `_stats_snap` 口径：导出统计不读实时界面值）
    engine::text::ConvertStats stats_{};
    bool stats_ready_ = false;
};

}  // namespace xrkit::shell
