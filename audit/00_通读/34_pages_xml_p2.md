# `pages/xml_page.cpp` 后半（第 681-905 行）

---

## 1. 明细的三层（10.3 纪律）
| 层 | 谁调 | 生成什么 |
|---|---|---|
| 摘要（表格那一格） | 每次 `refresh_rows` | `exml::inline_detail()` |
| 完整明细 | **只在选中一行时** | 插件 `detail_builder` ＞ `full_text_detail`（text 模式）＞ `full_detail` |
| 差异 id 列表 | 随完整明细一起 | `detail_ids_for()`（四类来源、**去重保序**） |

★ 插件 `detail_builder` 优先（第 681-686 行）—— 与 `compare` 一样"注册了就要被消费"。
★ `warning` 非空时**拼在明细最前面**（第 687 行）—— 不可信提示先于内容。

## 2. `detail_ids_for`：四类差异来源、去重保序（第 707-731 行）
`only_a` → `only_b` → `text_diff[0]` → `count_diff[0]`；
用 `std::set<std::string> seen` 去重，**保持插入顺序**（`vector` 是输出、`set` 只做查重）。

## 3. ★ `fill_id_tree`：按钮放在**第 0 列**（第 733-756 行）
```cpp
apply_pair_tree_spec(tree, PairTreeSpec{{"复制","来源","id"}, {96,110,240}, 0, false});
```
注释给了理由：
> 「复制id」按钮放在**第 0 列**（窄列），中间的 id 文本吃剩余宽度 —— 按钮在行首，
> **不会像"按钮列在末列"那样被 `stretchLastSection` 拉到右边、中间留一大块空白**。

⇒ 又是"列策略只有一份"的直接受益：这里的特殊布局**没有另写一套**，只是传不同的 spec。

## 4. `show_detail_for_item` 的两个分支（第 771-787 行）
`item == nullptr` ⇒ 清 head/view/id_list + `detail_panel_->setVisible(false)`；
非空 ⇒ 设 head、`setPlainText(detail_text(index))`、`fill_id_tree`、`setVisible(true)`。
★ `index = item->data(0, Qt::UserRole)`（**可见行下标存在第 0 列**，与 fs 包列表同一约定）。

## 5. `make_detail_dialog`：双击弹出的那个（第 789-823 行）
`WA_DeleteOnClose`；内含 head / 只读正文 / id 列表 / 「关闭」按钮（`QDialog::accept`）。
★ 与详情栏**共用** `detail_head_for` / `detail_text` / `detail_ids_for` —— 不重算。

## 6. ★★ P17-6：`export_csv` 自己再挡一道（第 825-836 行）

```cpp
if (task_ != nullptr && task_->running()) {
    status_->setText("比较进行中：导出已拒绝（等它结束或先取消）");
    log_to_host("导出 CSV 被拒：比较任务正在跑（worker 正在改结果表）");
    return false;
}
```
理由（注释原文）：按钮那侧已禁用，这条挡的是**绕过按钮直调**的调用者（自检/内部/插件页）
—— 那时 worker 正在 `results_.clear()+push_back()`，而本函数会遍历 `results_/visible_`
并 `count_kinds()` ⇒ **同一个 `std::vector` 的主/worker 并发读写（UB）**。

⇒ "**按钮禁用 + 函数内自查**"双闸 —— 与我之前记的 `db.h` 的"两道闸"同一条纪律。

## 7. CSV 的三个"同源"（第 837-886 行）
1. 列 = `spec->columns`（表头同一份）；
2. 单元格 = **`cell_text()`**（"表格里看到的与 CSV 里导出的不可能再分叉"——
   修前这里是页面里另抄一份 if-else 链）；
3. 计数 = **`count_kinds()`**（与统计标签同一份）。
★ 范围 = **当前筛选后**的行（`visible_`），不是全部结果（10.5）。
★ `encodings_used`：从结果里取两侧**第一个非空**的实际编码（`A=… / B=…`）。

## 8. `export_csv_auto` 的生成时间（第 889-894 行）
修前按钮里**写死** `"1970-01-01 00:00:00"`；现在 `QDateTime::currentDateTime()`。

---

## 9. 这一页给出的检查项（补充）

| 检查 | 为什么 |
|---|---|
| 插件回调（compare / detail_builder）是否真被调用 | 审计 3-7 "注册成功但无人消费" |
| 主线程直调与 worker 路径是否分叉 | 判据自己直调时不能禁用控件 |
| 导出/清除是否与比较互斥（双闸） | 同一个 vector 的主/worker 并发 |
| 表格/CSV/统计是否同源 | `cell_text` + `count_kinds` 各一份 |
| 每一行数据是否从**第 0 列**读 | 与 fs 包列表同一约定 |
