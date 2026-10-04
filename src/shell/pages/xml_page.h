#pragma once
// xrkit::shell —— XML 校对页（M2-6e3）。
//
// 页面只做 I/O 编排与显示：**比较算法全在 `engine::xml`**（统计模式/文本模式/摘要/
// 完整明细/CSV 渲染）。对照表 ⑩ 里属本页的口径：
//   · 10.4 切模式**清空旧结果、不重跑 I/O**（只重建列与筛选项；重跑要用户点「开始比较」），
//     且筛选项取自**新模式声明的 filters**（不回落到"全部"把行全放出来）；
//   · 10.3 每行的摘要是**按计数拼**的 `inline_detail`（绝不逐行调完整明细）；
//   · 10.5 导出 CSV 走引擎渲染（BOM + `\r\n` + excel 引号），范围＝**当前筛选后**的行；
//   · 10.1 模式/筛选的取值来自模式声明，模式下拉的 `data` 是**内部键**。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/xml/compare.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;

namespace xrkit::shell {

class TaskShell;
class VirtualList;

class XmlPage : public PageBase {
public:
    explicit XmlPage(QWidget* parent = nullptr);

    // 10.1：模式下拉的取值来自内置声明表（data=内部键；插件模式将来走同一张表）
    void fill_mode_combo();
    // ⑫12.1：把**注册表**接进来 —— 插件注册的比较模式要出现在同一个下拉里
    //（`nullptr` = 退回只显示内置模式，页面照样能用）
    void set_registry(const plugin::Registry* registry);
    std::string current_mode_key() const;
    // 10.4：切模式 → 重建筛选项 + **清空结果**，不重跑 I/O
    void set_mode(const QString& key);

    // 比较两个目录（界面与自检共用）：扫两侧 *.xml，逐个比；返回行数。
    // `should_stop` 非空 ⇒ **每个文件之前**轮询它（①：取消要真停；停在文件边界 ⇒
    // 已比较的结果都是完整的）。停下时 `last_compare_cancelled()` 为真、行数是已比较数。
    int compare_dirs(const QString& dir_a, const QString& dir_b, const QString& enc_a,
                     const QString& enc_b, const std::function<bool()>& should_stop = {});
    int result_count() const { return static_cast<int>(results_.size()); }
    int visible_count() const { return static_cast<int>(visible_.size()); }
    bool last_compare_cancelled() const { return last_compare_cancelled_; }
    int last_compare_total() const { return last_compare_total_; }
    // 10.4 的判据：切模式**不许重跑 I/O** —— 这个计数只在真正扫目录时涨
    int scan_count() const { return scan_count_; }
    QString row_text(int index) const;
    QString detail_text(int index) const;  // 完整明细（1.x 的 detail_builder）
    bool export_csv(const QString& path, const QString& generated_at);

    // ── ★ 2026-10-06（⑤ 三个小件，逐条对 1.x）──────────────────────────
    // 「清除」（1.x `:391-392` + `_clear_entries` `:514-518`）：清**两个目录栏**。
    void clear_dirs();
    // 统计标签：`一致 N | 不一致 N | 仅 {A名} N | 仅 {B名} N`
    //   （1.x `:406-409` 建 label、`:675-676` 写值；**用文件夹名**而不是 "A"/"B"）。
    QString stats_text() const;
    void refresh_stats_label();
    // 导出 CSV：**生成时间取真实当前时间**（1.x `:1312` `datetime.now()`）。
    //   2.x 原来把 `generated_at` 写死成 `"1970-01-01 00:00:00"`（`xml_page.cpp:161`）。
    bool export_csv_auto(const QString& path);

    QComboBox* mode_combo() const { return mode_combo_; }
    QComboBox* filter_combo() const { return filter_combo_; }
    QLineEdit* dir_a_edit() const { return dir_a_edit_; }
    QLineEdit* dir_b_edit() const { return dir_b_edit_; }
    VirtualList* rows() const { return rows_; }
    TaskShell* task() const { return task_; }
    QLabel* status() const { return status_; }
    QLabel* stats_label() const { return stats_label_; }
    QPushButton* clear_button() const { return clear_button_; }

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 两个"选择"调用点的起点判据
    //   （B ← A；A 是这一页的第一输入 ⇒ 空/多元时落到工具目录）
    DialogStart dir_a_dialog_start() const;
    DialogStart dir_b_dialog_start() const;

private:
    struct Row {
        engine::xml::XmlCompareResult result;
        std::string kind;  // same / diff / only_a / only_b
    };

    const engine::xml::XmlModeSpec* mode() const;
    void rebuild_filter_combo();
    void refresh_rows();
    bool row_matches(const Row& row) const;
    std::string name_a() const;
    std::string name_b() const;
    // 四个计数（一致/不一致/仅 A/仅 B）：**只有这一份**，标签与 CSV 都读它
    void count_kinds(uint64_t* consistent, uint64_t* inconsistent, uint64_t* only_a,
                     uint64_t* only_b) const;

    QLineEdit* dir_a_edit_ = nullptr;
    QLineEdit* dir_b_edit_ = nullptr;
    QComboBox* enc_a_combo_ = nullptr;
    QComboBox* enc_b_combo_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QComboBox* filter_combo_ = nullptr;
    VirtualList* rows_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* stats_label_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    bool last_compare_cancelled_ = false;
    int last_compare_total_ = 0;
    // 上一次比较**用的**两侧目录（用于统计标签里显示文件夹名 —— 与结果同一次快照，
    // 不跟着输入框的后续编辑乱变；1.x 同理：`self.name_a` 在 `_start_compare` 里定下）。
    QString compared_a_;
    QString compared_b_;
    std::vector<Row> results_;
    std::vector<int> visible_;   // results_ 的下标（当前筛选后）
    std::vector<QString> row_texts_;
    int scan_count_ = 0;
    const plugin::Registry* registry_ = nullptr;  // 可空（见 set_registry）
};

}  // namespace xrkit::shell
