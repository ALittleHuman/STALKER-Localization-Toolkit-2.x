# `src/shell/plugin_ui.cpp`(334) ＋ `src/shell/pages/page_base.cpp`(292)

---

## A. `plugin_ui.cpp`（334 行）—— C ABI 16 方法的**唯一实现**

### 1. `kQtUi` 的 16 个函数（第 40-217 行）
`label/button/row/checkbox/entry/text/combo/drop_box/pack/set_text/set_state/text_view/alive/
ask_yes_no/ask_open_file/ask_save_file` —— 参数名照 1.x `TkUiFactory`（第三方契约）。

### 2. ★★ **这一层的 `unwrap` 是裸 `reinterpret_cast`**（第 32-33 行）

```cpp
stk_widget* wrap(QWidget* w)   { return reinterpret_cast<stk_widget*>(w); }
QWidget*    unwrap(stk_widget* w) { return reinterpret_cast<QWidget*>(w); }
```

⇒ **句柄活性注册表只在 `widget_facade_qt.cpp` 的 `unwrap` 里**，这里没有。
  所以"死句柄遮蔽"（`21_hub.md` 第 3 节的根因）**只发生在门面层**，不在控件语义层。

### 3. `ui_alive`（第 179-182 行）与门面的 `alive` **不是同一个实现**
```cpp
int ui_alive(...) { return widget != nullptr && unwrap(widget) != nullptr ? 1 : 0; }
```
即"非空即活"（旧口径）。而门面的 `alive` 走 `handle_alive()`（S-01 修好的版本）。
⇒ 两条路给出不同答案：C ABI 那条仍是旧语义（但 C ABI 已退役，实际无人调用）。

### 4. 逐个控件的口径要点
- `ui_label`：**`setWordWrap(true)`**（P17-51：修前长文案被截断成一行）；
- `ui_button`：`setEnabled(state != 0)`；回调接 `clicked`；
- `ui_row`：裸 `QWidget` + `QHBoxLayout`（1.x 的 frame）；
- `ui_entry`：`width > 0 ⇒ setMaximumWidth(px(width))`；`show` 非空 ⇒ `Password`；
- `ui_text`：`height > 0 ⇒ setMinimumHeight(px(height) * 16)`（**乘 16 是经验行高**）；
- `ui_drop_box`：**只读** `QLineEdit` + `setAcceptDrops(true)`，回调函数指针挂在
  `xrkit_on_file` 属性上（由 `drop_filter.h` 统一处理拖入）；
- `ui_pack`：**`side == "top"` ⇒ `insertWidget(0, …)`（插到最前）**，否则 `addWidget`；
  `padx/pady` 取较大者当 `layout->setSpacing`；
- `dialog_buffer()`（第 197-200 行）：静态 `std::string` 承接对话框返回值
  —— 注释写明"单线程 UI，够用"。

### 5. 文件尾部：shell 侧 trace ring（第 238-332 行）
`ShellPageTraceRing{QMutex, QStringList, dropped, truncated}`；
`append_shell_page_trace`：开关 `XRKIT_PAGE_TRACE=1`、`place` 默认不记、ASCII 子串过滤、
`thread_local last_page` 兜底、4096 上限丢最旧；
`flush_shell_page_trace`：**首写 Truncate、之后 Append**、空则**不碰文件**（幂等）。
★ `flush_engine_page_trace()` 只是转发 `plugin::flush_page_trace()`。

⇒ 与引擎侧（`page_trace_engine.txt`，纯 append）**不对称**：
  引擎文件跨运行累积，外壳文件只留最后一次 —— 读 trace 时必须记住（`15_*.md` 已记）。

---

## B. `pages/page_base.cpp`（292 行）—— 页的外壳

### 1. 构造顺序（第 74-159 行）
```
setObjectName("page_<id>")
setFocusPolicy(Qt::ClickFocus)          // 点空白收焦点（两层修法的①）
outer_  = QVBoxLayout(this)             // 边距/间距全过 px()
header_ = QLabel(title_)                // page_header，+3pt 加粗
subtitle_ = QLabel                      // page_subtitle，默认隐藏
viewport_ = new ScrollViewport(this)    // ★ 每页自带视口
    fit = Content；min_body_height = page_min_h() = px(430)
body_ = QWidget ; card_ = QFrame(body_) // ★ 正文包进卡片
body_layout_ = QVBoxLayout(card_)       // 页面往这里加东西
viewport_->set_body(body_)
action_bar_ = QWidget(this)             // ★ 在视口**之外**（规则 2）
header_row_ = QWidget(this)             // 标题 + 副标题同一行
outer_ -> header_row / viewport(1) / action_bar
5 个"空白容器"装 eventFilter（点空白收焦点，两层修法的②）
```

★★ **对 P5 的直接含义**：`PluginPage` 构造时 `PageBase(page_id, title, nullptr)`
⇒ **页在构造期是顶层控件**（没有 parent）⇒ `width()==0`、`isVisible()==false`，
直到 `stack_->insertWidget()` 才 reparent。
⇒ 任何"在 `insertWidget` 之前量宽度/可见性"的判据都会得到 0 —— 这正是我们
  `wait_page_ready` 那个"要求在 show/switch 之后"的修法所针对的。

### 2. `ui_thread_guard`（第 41-72 行）
没有 `QCoreApplication` ⇒ 放行（"不存在 UI 线程这回事"）；
当前线程 ≠ app 线程 ⇒ 记 `Warn` 日志 + 计数 + 记最后一条 what，返回 false；
`ui_thread_violation_count()` / `ui_thread_last_violation()` / `reset_ui_thread_violations()`
—— **自检可以断言"违规次数为 0"**。

⇒ 这是**第三处**线程判定（另外两处：`widget_facade_qt.cpp` 的 `on_ui_thread`、
`py_host.cpp` 的 `PyUi::available`）。三处口径不完全一致，值得统一（审计建议）。

### 3. `minsize_ok()`（第 231-234 行）
```cpp
return viewport_ != nullptr && viewport_->min_body_height() == page_min_h() &&
       viewport_->min_body_height() > 0;
```
⇒ **P5 那条"宿主外壳不对"的判据读的就是它**（`smoke.cpp:7266-7269`）：
`page_base->viewport()->fit() == Content && page_base->minsize_ok()`。
★ 所以只要页不是 `PageBase`（占位页/裸容器），`page_base == nullptr` ⇒ 走 else 分支 ⇒ 报
  "没被套上宿主视口"。**这与我们的根因（空页 ⇒ 占位页 ⇒ 不是 PageBase）完全一致。**

### 4. `make_inner_scroll`（第 193-208 行）：页内第二层视口
一页只造一层（幂等）；**先把内容件挂到视口下再 `set_body`**（否则它会变成顶层窗口）；
`sync_inner_scroll` 从 `content->property("xrkit_content_height")` 取高，
没有就用 `sizeHint().height()`。

### 5. `add_action` 的一个真缺陷修复（第 221-229 行）
那个 `addStretch(1)` 是构造时**第一个**加进来的（index 0）⇒ "插在末尾 stretch 之前"其实插到了
stretch **前面** ⇒ 按钮全被顶到左边（与"操作栏贴右"相反）。
修法：**追加到 stretch 之后**（`addWidget` 即可）。

### 6. 插件槽位消费端（第 241-290 行）
`append_plugin_context_items` 转发到 `plugin_context_menu.cpp`（唯一实现）；
`clear_plugin_option_combos` / `clear_plugin_action_buttons`：**先摘后删**
（`removeWidget` + `setParent(nullptr)` → `deleteLater`），只认 `plugin_` 前缀。

---

## C. 这批的结论

1. **`unwrap` 有两套**：控件语义层是裸 cast（安全），门面层带死句柄检查（**根因所在**）。
   ⇒ 修 P5 只需动门面层，不必碰 `plugin_ui.cpp`。
2. **`PageBase` 在 `insertWidget` 之前是顶层控件**（宽 0、不可见）⇒ 判据必须等挂载。
3. **线程判定有三处**（`on_ui_thread` / `PyUi::available` / `ui_thread_guard`），口径需统一。
4. **`minsize_ok()` 就是 P5 判据读的那个函数** —— 页不是 `PageBase` 时必红，与根因吻合。

## D. 待继续

`src/shell` 还剩 53 个（`smoke.cpp` 10390 是最大的一块，`pages/*` 6 页、`facade_tree.cpp`、
`scroll_viewport.cpp`、`task_shell.cpp`、`plugin_context_menu.cpp`、`drop_filter.h` 等）。
