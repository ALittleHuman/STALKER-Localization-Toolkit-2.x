# `pages/fontpack_page.cpp`（506 行，全文读完）—— **P17-1 三步法**的样板

---

## 1. ★★ P17-1（审计 03 F-01 Critical）：`prepare / run_build / apply_result` 三步

修前的事实（注释第 216-220 行）：
> worker 直接调 `build()` ⇒ 跨线程读下拉框、写 `result_label_`/`state_label_`、
> 裸 `++refused_count_`（数据竞争），**并且页析构时 `result_label_` 先于 `task_` 被删
> ⇒ 访问违例**。

修法（三步，各带线程守卫）：

```
① prepare(xml_dir, font, out)     ← 主线程，读控件 → 冻结成 PackageOptions（值类型）
② run_build(options)              ← **worker 可调**：只碰引擎，一个字都不碰控件
   · 先判后建：Missing/Unknown ⇒ refused=true + refused_count_.fetch_add(1) + 不建任何目录
   · 否则 build_package(options)
③ apply_result(outcome)           ← 主线程（finish_handler 里），画到控件上
```

三条守卫：`options_for`/`apply_result`/`build` 都过 `ui_thread_guard(...)`。

★ **`refused_count_` 是 `std::atomic`**（修前裸 `int` ⇒ 数据竞争）。

★ `~FontPackPage()` 里 `task_->stop_and_wait()`：
> worker 算完的结果由**主线程收尾回调**画；但"析构期还有线程在跑"这个窗口要在析构体里关掉。

⇒ **这是本仓"跨线程做重活"的标准答案**，也是 `text_page` 的 `message()` 那条路的加强版：
  - 有**返回值/结构化结果** ⇒ 用 `finish_handler` + `publish_outcome/take_pending_outcome`（带锁）；
  - 只有**逐条文本** ⇒ 用 `message()`（排队回主线程）。

---

## 2. `run_build` 的"先判后建"（11.9）
```
check_font(font_path) → CjkFontState
Missing/Unknown ⇒ refused = true，**没有创建任何目录**
```
判据**只有引擎那一份**（`classify_cjk_font`），页面不自己再判一遍。

★ 并且 `font_unrenderable` 只**告警**不拒绝（"那条路在引擎的 build_package 里以 warning 体现"）。

## 3. 任务壳按 `CancelButton::No` 造（第 181-194 行）
用户 2026-10-06 原话：「**封包和字库的取消，在这里本质是完全没有的，故直接删除即可**」
⇒ **连取消按钮都不创建**。事实依据：字库打包 + 原子写盘不可中途停止。

⇒ 与 `task_shell.cpp:36-39` 的注释一致（"不是隐藏、不是置灰 —— 那两种都还是假的取消"）。

## 4. 字体选择器是**宿主唯一一份**（P8a，第 288-307 行）
`make_font_picker(parent, current, start_dir_fn, on_picked, on_problem)`
——页面与 `api.ui.pick_font` 共用；返回对话框**是为了自检能驱动它**（"双击＝选择那条链"）。

## 5. `remember_font`：写 user.ltx 失败**必须告警**（第 309-322 行）
> 失败**必须明确告警**（"下次启动需重新选择字体"），绝不静默。

## 6. `open_output` 的两个"点了没反应"（第 332-349 行）
- 目录无效 ⇒ 明确说"输出目录无效：<dir>"；
- `QDesktopServices::openUrl` 失败 ⇒ **原先忽略返回值**（点了没反馈），现在报"可能缺少文件管理器关联或权限不足"。

## 7. 下拉的"自定义"两项（第 126-147 行）
`lang` 与 `suffix` 都有"自定义"选项 ⇒ 只有选它时才显示输入框（`sync_lang_edit`/`sync_suffix_edit`），
且非自定义时**清空输入框**（避免"上次的自定义值偷偷生效"）。

## 8. 三个对话框起点的判据（第 489-504 行）
`xml`：第一输入 ⇒ 空时落"工具自身所在文件夹"；
`font`/`out`：`{xml_edit_->text()}` ⇒ 用 XML 目录当起点（唯一才用）。

★ 与 `text_page` 一样：**判据函数与按钮 lambda 共用同一份**。

---

## 9. 这一页给出的"六页通用清单"（补充 `30_pages_text.md`）

| 检查 | 为什么 |
|---|---|
| 有没有 `prepare/run_build/apply_result` 三步 | 跨线程重活的唯一正确形状 |
| 计数器是不是原子 | `refused_count_` 修前是裸 int |
| 析构里有没有 `stop_and_wait()` | 关闭"析构期还有线程在跑"的窗口 |
| `CancelButton::No` 的页是否真的没造按钮 | "假的取消"比没有更糟 |
| 失败/拒绝是否都 message 或写标签 | 零静默 |
| 对话框起点是否走 `dialog_start_for` | 与按钮共用同一份 |
