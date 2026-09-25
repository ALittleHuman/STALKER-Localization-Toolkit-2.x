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

#include "xrkit/engine/xml/compare.h"

#include <QString>

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
    std::string current_mode_key() const;
    // 10.4：切模式 → 重建筛选项 + **清空结果**，不重跑 I/O
    void set_mode(const QString& key);

    // 比较两个目录（界面与自检共用）：扫两侧 *.xml，逐个比；返回行数
    int compare_dirs(const QString& dir_a, const QString& dir_b, const QString& enc_a,
                     const QString& enc_b);
    int result_count() const { return static_cast<int>(results_.size()); }
    int visible_count() const { return static_cast<int>(visible_.size()); }
    // 10.4 的判据：切模式**不许重跑 I/O** —— 这个计数只在真正扫目录时涨
    int scan_count() const { return scan_count_; }
    QString row_text(int index) const;
    QString detail_text(int index) const;  // 完整明细（1.x 的 detail_builder）
    bool export_csv(const QString& path, const QString& generated_at);

    QComboBox* mode_combo() const { return mode_combo_; }
    QComboBox* filter_combo() const { return filter_combo_; }
    QLineEdit* dir_a_edit() const { return dir_a_edit_; }
    QLineEdit* dir_b_edit() const { return dir_b_edit_; }
    VirtualList* rows() const { return rows_; }
    TaskShell* task() const { return task_; }
    QLabel* status() const { return status_; }

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

    QLineEdit* dir_a_edit_ = nullptr;
    QLineEdit* dir_b_edit_ = nullptr;
    QComboBox* enc_a_combo_ = nullptr;
    QComboBox* enc_b_combo_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QComboBox* filter_combo_ = nullptr;
    VirtualList* rows_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<Row> results_;
    std::vector<int> visible_;   // results_ 的下标（当前筛选后）
    std::vector<QString> row_texts_;
    int scan_count_ = 0;
};

}  // namespace xrkit::shell
