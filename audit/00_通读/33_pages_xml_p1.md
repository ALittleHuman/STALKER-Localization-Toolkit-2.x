# `pages/xml_page.cpp`（905 行；本轮读 1-680）

---

## 1. 结果表是**按模式声明的多列**，列策略与 fs 两棵树同一份（第 109-115、596-614 行）
```cpp
apply_result_columns():
    spec = mode()；把 spec->columns 翻成 PairTreeSpec（headers/widths）
    signature = 列 key 拼接；**列没变 ⇒ 不重装**（"免得把用户拖过的列宽复位"）
    apply_pair_tree_spec(result_tree_, tree_spec);   // ← 唯一装配件（pair_tree.cpp）
```
⇒ 与 `facade_tree.cpp:75-87` 完全同一条纪律：**列策略只有一份实现**。
★ `QTreeWidget::setRootIsDecorated(false)` —— 平表，不画展开箭头（与 fs 的树相反）。

## 2. 详情栏的"平时隐藏"与"点空白收起"（第 117-158、188-196 行）

- `detail_panel_->setVisible(false)`：**没选中行时不占位**
  —— 注释给了实测理由：详情栏常驻会把页顶到 836px，800px 窗口里统计行被挤到视口以下
  （`GEOM xml_stats y=720 / 视口 583`）；
- ★ **2026-10-22 的修复**（用户实测反馈）：
  ```cpp
  const QList<QTreeWidgetItem*> selected = result_tree_->selectedItems();
  show_detail_for_item(selected.isEmpty() ? nullptr : selected.first());
  ```
  修前用 `currentItem()`，而"清空选择"后 `currentItem()` **仍指向上一行**
  （current 与 selected 是两件事）⇒ 详情栏永远收不起来；
- 另加显式入口 `xml_detail_collapse`（"收起明细"按钮）。

## 3. ★★ 审计 3-7：插件比较模式"注册成功但无人消费"（第 372-399 行）

修前 `mode()` 用 `exml::find_mode(key)`，而它**只遍历内置表**
⇒ 插件模式的 key 永远解析不出来 ⇒ 落回 `builtin_modes().front()`
⇒ 表头/筛选/行取值**全用内置那份**（"下拉里看得见它、选中它却什么都不是"）。

现在两层：
- `mode()`：**注册表优先**（内置也在注册表里，owner 是 `kBuiltinPlugin`）→ 再退 `find_mode` → 最后退内置首项；
- `mode_reg()`：取**注册项**本身（`compare` / `detail_builder` 在它身上）。

★ `compare_dirs()` 里 `options.compare = reg_this_round->compare;`（第 467-470 行）
—— 插件回调**真的被调用**（"审计 3-7 那条不许回退"）。

## 4. ★★ 审计 3-5：worker 不许读控件 + 比较期间禁用四件（第 213-222、508-538 行）

修前事实（审计原文）：
> 叠加"比较期间模式/筛选控件未禁用"（全文件 `setEnabled` 零命中）
> ⇒ 主线程 `results_.clear()` 与 worker `push_back` **并发**（UB / 崩）。

修法两条：
1. **起 worker 之前**在主线程快照 `mode_key_at_task_start_`；`current_mode_key()` 在
   **非主线程**时读快照（第 361-370 行）；
2. `set_compare_controls_enabled(false/true)` —— ★ 只禁**四个**控件：
   模式、筛选、**导出 CSV**、**清除**。
   注释把两条的风险**如实分开**（P17-6）：
   - **导出 CSV ＝ 真竞态**（遍历 `visible_/results_` 与 worker 的 `clear()+push_back()` 并发）；
   - **清除 ＝ 口径不一致**（`clear_dirs()` 只清输入框，但统计/文件夹名按输入框算）。
   并且 `export_csv` 自己还有一道"比较中拒绝"的闸（挡绕过按钮直调）。

★ 还有一条容易漏的：`task_->run` 返回 false（已有任务在跑）⇒ **马上把控件放回去**
（否则页面僵住）—— 第 247 行。

## 5. ★ 主线程直调 vs 工作线程的分叉（第 489-504 行）
```cpp
const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
if (on_main) { refresh_rows(); refresh_stats_label(); }
else { QMetaObject::invokeMethod(this, [..]{ refresh_rows(); …; set_compare_controls_enabled(true); mode_key_at_task_start_.clear(); }, QueuedConnection); }
```
⇒ 与 `set_compare_controls_enabled` 的"只在真 worker 时禁用"配套：
**判据自己直调时不禁用**（否则自检要在比较前后来回切模式会被自己挡住）。

## 6. P17-10：目录遍历错误必须可见（第 300-309、474-478 行）
引擎 `DirDiffReport::errors` → `last_scan_errors_` → 统计行下方追加
`⚠ 目录遍历出错 N 处 ⇒ **本次比较结论不可信**：<第一条>`。

## 7. 四个计数只有一份（第 313-326 行）
`count_kinds()`：`consistent → only_a → only_b → 其余算不一致`。
标签、CSV 都读它 ⇒ "不可能分叉"。

## 8. ★ 行文本与表格同源（第 642-648 行）
`row_texts_[i]` 保留为 `"状态\t相对路径\t内联明细"`：**"这一行会显示成什么"的单一口径**，
自检与日志都读它；表格按模式声明的列画（两者同源于 `cell_text`）。

⇒ 这是"**同一事实两个消费者**"的正确做法：不是各算一遍，而是**留一份中间表示**。

## 9. `name_a()/name_b()` 的"比较开始时定下"（第 430-447 行）
用 `compared_a_/compared_b_`（比较开始时快照），而不是实时读输入框
——"否则统计行显示的名字会与这次结果对应的目录错位"。

## 10. 待继续（第 681-905 行）

`detail_text` 的插件分支、`fill_id_tree`/`show_detail_for_item`/`make_detail_dialog`、
`export_csv_auto`、`visible_count`、三个对话框起点。
