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
//
// ★★ 2026-10-06（D2 批）补齐的三件（1.x `xml_compare_app.py` 有、2.x 只有引擎没有入口）：
//   ① **结果表改多列**：1.x 是按模式声明的多列 `ttk.Treeview`（`:860`、`_apply_columns`
//      `:467-489`）；2.x 原来是**单列** `VirtualList`、用 `\t` 拼文本 ⇒ 改成本仓库已有的
//      `QTreeWidget` + `apply_pair_tree_spec`（列策略：一律 `Interactive` ＋ 末列
//      `setStretchLastSection(true)` —— 用户 2026-09-29 明确要过"不许设成 Stretch"）。
//   ② **详情栏**：选中一行 ⇒ 头部行 `[ rel ] ✓/✗ 状态  ID 数: A= B=`（1.x `:937-950`）
//      ＋ 完整明细正文 ＋ 下半部分"差异 id（逐条复制）"（1.x `:456-458`、`:1195-1199`）。
//   ③ **双击行 ⇒ 明细弹窗**（1.x `:907-914`、`:901-905`）。
//   每一条差异 id 后面都有各自的「复制id」按钮（1.x `:1183`；「复制全部 id」是 1.x
//   自己删掉的，这里也不做）。剪贴板用法照 `fs_page.cpp` 的 `QGuiApplication::clipboard()`。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/xml/compare.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace xrkit::shell {

class TaskShell;

// 详情栏里"逐条复制"的那一条（1.x `_detail_ids` 的 (来源标签, id 文本)）。
struct XmlDetailId {
    QString category;  // "仅 a" / "仅 b" / "文本不同" / "次数不同"
    QString id;
};

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

    // ── ★ 2026-10-06（D2 批）：多列结果表 / 详情栏 / 双击弹窗 ─────────────
    QTreeWidget* result_tree() const { return result_tree_; }
    QWidget* detail_panel() const { return detail_panel_; }
    QLabel* detail_head() const { return detail_head_; }
    QPlainTextEdit* detail_view() const { return detail_view_; }
    QTreeWidget* id_list() const { return id_list_; }

    // 详情头一行（1.x `:937-950` 的 `[ rel ] ✗ 状态  ID 数: A= B=`）
    QString detail_head_for(int index) const;
    // 该行"差异 id"逐条清单（1.x `_detail_ids`：去重、保序，四类来源汇总）
    std::vector<XmlDetailId> detail_ids_for(int index) const;
    // 详情栏当前显示的可见行下标（-1 = 没选）。自检读它判"选中回调真的接上了"。
    int selected_visible_row() const { return selected_visible_; }
    // 程序化选中（自检用：不走鼠标也能验证"选中回调 → 详情栏"这条接线）
    void select_visible_row(int index);
    // 造一个明细弹窗（1.x `:907-914` 双击 → `_show_detail_popup_for`）。
    // **不 exec**：调用方决定 show() 还是只检查内容（自检拿到它就不阻塞）。
    QDialog* make_detail_dialog(int index, QWidget* parent = nullptr);
    // 复制单条 id 到剪贴板（1.x `_copy_one_id`；失败要**留痕**不静默）
    bool copy_id(const QString& id);

    QComboBox* mode_combo() const { return mode_combo_; }
    QComboBox* filter_combo() const { return filter_combo_; }
    // ★★ 2026-10-06（审计 3-5）：比较期间"模式/筛选控件"到底禁用没有 —— 判据要读得到
    //   （审计那条判据的后半句是"顺带把比较期间模式/筛选控件按需禁用，消除 results_ 的
    //   主/worker 并发写"）。**只读**：控件不在（构造期）也算"没禁用"。
    //   ⚠ 定义在 .cpp：`QComboBox` 在本头里只是前向声明，inline 体里调 `isEnabled()` 会 C2027。
    bool compare_controls_enabled() const;
    // D3 批：编码下拉的**唯一一份**判据是文件里的 `kEncodings`（1.x
    // `xml_compare_app.py:302` 的 `ENCODINGS` 六项）。暴露出来只是为了让自检
    // 能钉住"那一页**故意不含** big5/shift_jis/euc-kr"（见 D3④），不新增第二份表。
    QComboBox* enc_a_combo() const { return enc_a_combo_; }
    QLineEdit* dir_a_edit() const { return dir_a_edit_; }
    QLineEdit* dir_b_edit() const { return dir_b_edit_; }
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
    // ★★ 2026-10-06（审计 3-7）：当前模式的**注册项**（含插件的 `compare`/`detail_builder`
    //   两个回调）—— 修前页面只认内置声明表，插件注册的回调**一个都不被调用**（登记了没人用）。
    //   内置模式也在注册表里，但它们的 `compare` 是空的 ⇒ 走内置引擎函数（行为逐字节不变）。
    const plugin::CompareModeReg* mode_reg() const;
    // ★★ 2026-10-06（审计 3-5）：比较期间的"模式/筛选控件按需禁用"（只在**工作线程跑**时禁，
    //   主线程直调的判据路径不禁 —— 自检要在比较前后来回切模式）。
    void set_compare_controls_enabled(bool on);
    void rebuild_filter_combo();
    void refresh_rows();
    bool row_matches(const Row& row) const;
    std::string name_a() const;
    std::string name_b() const;
    // 四个计数（一致/不一致/仅 A/仅 B）：**只有这一份**，标签与 CSV 都读它
    void count_kinds(uint64_t* consistent, uint64_t* inconsistent, uint64_t* only_a,
                     uint64_t* only_b) const;
    // 一行的"状态"列文本（**唯一一份**：结果表 / CSV / 详情头都用它）
    QString row_status_text(const Row& row) const;
    // 一列一格（**唯一一份**：结果表与 CSV 都读它 ⇒ 表格与导出不可能分叉）
    std::string cell_text(const Row& row, const engine::xml::XmlColumn& column) const;
    // 结果表的列/表头由**模式声明**给（1.x `_apply_columns`）；列策略走共用装配件
    void apply_result_columns();
    // 选中一行 → 刷新详情栏（1.x `_on_row_select`）
    void show_detail_for_item(QTreeWidgetItem* item);
    // 把"逐条 id + 复制id 按钮"填进一棵树（详情栏与弹窗**共用这一份**）
    void fill_id_tree(QTreeWidget* tree, const std::vector<XmlDetailId>& ids);
    QTreeWidgetItem* item_for_visible_row(int index) const;

    QLineEdit* dir_a_edit_ = nullptr;
    QLineEdit* dir_b_edit_ = nullptr;
    QComboBox* enc_a_combo_ = nullptr;
    QComboBox* enc_b_combo_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QComboBox* filter_combo_ = nullptr;
    QTreeWidget* result_tree_ = nullptr;
    QWidget* detail_panel_ = nullptr;
    QLabel* detail_head_ = nullptr;
    QPlainTextEdit* detail_view_ = nullptr;
    QTreeWidget* id_list_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* stats_label_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    bool last_compare_cancelled_ = false;
    int last_compare_total_ = 0;
    int selected_visible_ = -1;
    // 已应用到结果表上的"列签名"：列变了才重装表头（免得每次刷新都把用户拖过的列宽复位）
    std::string applied_columns_;
    // 上一次比较**用的**两侧目录（用于统计标签里显示文件夹名 —— 与结果同一次快照，
    // 不跟着输入框的后续编辑乱变；1.x 同理：`self.name_a` 在 `_start_compare` 里定下）。
    QString compared_a_;
    QString compared_b_;
    std::vector<Row> results_;
    std::vector<int> visible_;   // results_ 的下标（当前筛选后）
    std::vector<QString> row_texts_;
    std::vector<QTreeWidgetItem*> row_items_;  // 与 visible_ 一一对应
    int scan_count_ = 0;
    const plugin::Registry* registry_ = nullptr;  // 可空（见 set_registry）
    // ★★ 2026-10-06（审计 3-5）：起 worker 之前把模式 key 快照在这（worker 不许读控件）
    QString mode_key_at_task_start_;
};

}  // namespace xrkit::shell
