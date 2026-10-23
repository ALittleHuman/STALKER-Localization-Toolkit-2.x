# shell 批二：`scroll_viewport.cpp`(380) ＋ `plugin_context_menu.cpp`(42) ＋ `drop_filter.cpp`(101) ＋ `log_box.cpp`(79) ＋ `pair_tree.cpp`(49)

---

## A. `scroll_viewport.cpp`（380 行）—— P5 的另一半

### 1. 三条"条子只能自己造"的落实（第 37-51 行）
`vbar_/hbar_` 都是本组件 `new QScrollBar(this)`（门禁静态扫描 `new QScrollBar(`）；
★★ 2026-10-07 补的两条连接（**普通视口只有滚轮能动画面**是真缺陷，P5 判据抓到）：
```cpp
connect(vbar_, &QScrollBar::valueChanged, this, [this](int){ sync_body_offset(); });
connect(hbar_, &QScrollBar::valueChanged, this, [this](int){ sync_body_offset(); });
```
`sync_body_offset()`：`self_scrolling_` 时**直接返回**（自带滚动的画布由 offset delegate 接管，两条路不打架）。

### 2. `Fit` 三种模式与 `min_body_height`
`set_fit()` / `set_min_body_height()` 都只存字段然后 `sync_layout()`。
★ 判据 P5 读的正是这两个（`viewport()->fit() == Content && minsize_ok()`，
而 `minsize_ok()` = `min_body_height() == page_min_h() && > 0`）。

### 3. 滚轮路由：**按事件身份认领**（第 22-33 行 + `route_wheel`）
- `serial_of(event)` = 事件对象地址（"一个 QWheelEvent 实例只会被派发一次；沿父链冒泡时地址不变"）；
- 全局 `g_last_serial / g_last_claim_ms`：同一事件只允许**一个**条子动；
- 没有事件身份时退回 **60 ms 时间窗**（`kClaimWindowMs`）；
- 沿父链上溯上限 `kMaxParentHops = 60`（1.x 同值）。

⇒ **"同一个滚轮事件不许两个视口都滚"** —— 这是嵌套视口（页视口＋插件视口）必须的。
  我们的 P5 判"拖条子画布真的挪了"时，走的是 `valueChanged` 那条正路，不经过滚轮路由。

### 4. 控件级拦截（第 58-64 行 + `eventFilter` 361-370）
`install_wheel_filter` 递归给 body 的所有子控件装过滤器；
`ChildAdded` 事件里**补装**（运行期加进来的子控件也要覆盖）。

---

## B. `plugin_context_menu.cpp`（42 行）

`append_registry_context_items(menu, registry, host_page_id, context, owner)`：
1. `registry->menu_items_for(page_id, "context", context)`；
2. **空 ⇒ 返回 0 且连分隔符都不加**（1.x「无注册即无痕迹」）；
3. 非空 ⇒ 先 `addSeparator()`（仅当菜单已有项），再逐条 `addAction`；
   `objectName = "plugin_ctx_" + id`；回调走 `registry->invoke(plugin, label, handler)`。

★ **这是唯一实现**（原来在 `PageBase::append_plugin_context_items`，P13-2a-2 抽出来；
`PageBase` 那份现在只转发）。

---

## C. `drop_filter.cpp`（101 行）

- 视觉反馈用**属性** `drop_hover`（`unpolish/polish/update`）而不是盖色块
  —— "反馈必须**不改变布局**"（`--shot` 与页高判据都靠几何）；
- `attach()`：`setAcceptDrops(true)` + **先把属性建出来（false）**（QSS 属性选择器要求属性存在）；
- `dispatch()`：`DragEnter/DragMove` 只接受**本机文件**（`url.isLocalFile()`），
  `Drop` ⇒ 回调路径列表；`DragLeave/Leave` 撤高亮但**不吃掉事件**（"leave 是别人也在用的常规事件"）。

---

## D. `log_box.cpp`（79 行）—— ★ "4.4 攒批"那条判据的对象

```cpp
void append_batched(line) { pending_ << line; if (timer_id_==0) timer_id_ = startTimer(80); }
void flush() {
    killTimer; if (pending_.isEmpty()) return;        // ← 空则**不计数**
    text = pending_.join('\n'); pending_.clear();
    插入文档（一次 QTextCursor 批量）
    if (persist_) persist_(text);                     // ← 落盘镜像
    ++flush_count_;                                   // ← ★ 在 persist 之后、**无条件**
    enforce_limit();                                  // 超 5000 删到 3000（++trim_count_）
}
```

⇒ 三个判据点：
1. **`flush_count_` 只在"有内容"时递增** ⇒ "500 行只落一次"要等 80 ms timer 真的触发；
2. **`persist_` 可能为空**（纯 UI 测试没有 set_persist）⇒ 计数照样 +1（我 10-22 的修法正确）；
3. `clear_log()` 清 pending + kill timer + `clear()` —— **不碰 `flush_count_`**。

---

## E. `pair_tree.cpp`（49 行）—— 列策略的**唯一实现**

- `setColumnCount` / `setHeaderLabels` / `setUniformRowHeights(true)` / `setAlternatingRowColors(false)`；
- ★ **`setRootIsDecorated(true)`**：包列表"平表时代"留过 `false`，树化后会让顶层节点**没有展开箭头**；
- **`setExpandsOnDoubleClick(false)`**：展开只认"单击条目身体"（`click_on_indicator` 判定）；
- 每列 `Interactive` + `resizeSection(px(width))`，`setMinimumSectionSize(px(48))`；
- ★★ **`setStretchLastSection(true)`** 而不是把某列设成 `Stretch`
  —— "后者会**失去拖拽**"，正是用户 2026-09-29 要的可拖分隔条
  （第一版用首列 Stretch 被自检当场拦下：树①/树② 两条红）。

`click_on_indicator`：`x < row.x() + indentation*depth + (PM_IndicatorWidth + 8)` ⇒
点箭头 vs 点身体的分界。

---

## F. 这批的结论

1. **P5 的"条子动了画布才动"是 2026-10-07 补的连接**（之前只有滚轮能动）——判据抓到的真缺陷。
2. **`log_box.cpp` 的三个计数语义**（见 D）决定了"4.4 攒批"那条判据该怎么读。
3. **列策略只有一份**（`pair_tree.cpp`），`facade_tree` 只做转发 —— "同类只维护一套"的落地。
4. 右键菜单**空即无痕迹**（连分隔符都不加）。

## G. 待继续

shell 还剩：`page_host.cpp`(169)、`virtual_list.cpp`(189)、`split_pane.cpp`(266)、
`font_picker.cpp`(348)、`theme.cpp`(438)、`main.cpp`(410)、`dialog_start.cpp`(55)、
`row_action_delegate.cpp`(58)、`drop_zone.cpp`(39)、`screen_scale.cpp`(69)、`fake_ffmpeg.cpp`(79)、
`pages/*` 6 页（fs 1952 / video 1055 / xml 761 / convert 616 / fontpack 421 / text 225）、
`smoke.cpp`(10390)。
