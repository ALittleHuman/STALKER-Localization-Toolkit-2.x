# `src/shell/pages/text_page.cpp`（277 行，全文读完）—— 六页的**标准形状**

> 这是最小的一页，正好把"一个页应该长什么样"钉下来；其余五页（fs 1952 / video 1055 /
> xml 761 / convert 616 / fontpack 421）都是同一套形状 + 各自的业务。

---

## 1. 构造（第 30-139 行）

```
PageBase("text", "文本提取", parent)
① 两个 QLineEdit（源目录 / 输出目录）+ 每行一个 container（QLabel + edit + 「选择…」按钮）
   ★ 每行 container 都走 drop_zones_.attach_row(container, this)
     ⇒ 那个 container 被命名为 "field_row"（见 drop_zone.cpp:17-19）
     ⇒ 这就是 ⑭ 判据扫的候选集来源
② prefix / id_prefix / clean 复选框（一行 opt_row）
③ VirtualList（行高 px(20)）+ row_text_provider
   ★★ H2-4：修前是 body_layout()->addWidget(rows_, 1) —— 行列表**没有自己的条子**，
      而 VirtualList 不自带滚动条、只按 visible_range(top_=0,…) 画 ⇒ 大结果**永远够不着**。
      现在 make_inner_scroll(rows_, "text_rows_viewport") + set_self_scrolling_body(true)
      + offset_delegate(set_scroll_top)
      （不挪行列表，否则 top_ 恒为 0 ＝ 虚拟化被关掉）
④ stats_label_（三行：gameplay / scripts / 总计）
⑤ TaskShell + set_message_handler(add_row) ⇒ body_layout 里放 task_->panel()
   ★★ F3：修前是 `new QLabel(task_->status())` —— `status()` 返回 **QString**，
      于是造出一个**无父标签**（顶层窗口、从不进布局）⇒ 页面消息用户看不见。
      现在 `status_` 有父、进布局。
⑥ add_action("开始提取", objectName="text_start")
```

## 2. `run_batch()`（第 211-266 行）—— 六页共用的 worker 契约

```cpp
task_->run("文本提取", [=](const ProgressSink& progress, const MessageSink& message) {
    progress(0);
    const auto should_stop = [this]{ return task_ && task_->cancel_requested(); };  // ★ 只读原子位
    const stats = extract_one(...);
    progress(100);
    // 统计框只能在主线程更新 ⇒ 排队回主线程，**先于收尾入队**（同一队列保序）
    QMetaObject::invokeMethod(this, [this, for_ui]{ note_stats(for_ui); }, Qt::QueuedConnection);
    if (stats.cancelled) { message("已取消：在文件边界停下（已提取 N 文件 / M 字符串 / K 字符；" "已写出的产物都是完整的）"); return; }
    if (!stats.ok) { 逐条 message("跳过 …"); message("提取失败：" + err); return; }
    message("files=… strings=… chars=… xml=…");
    for (skipped) message("跳过 " + skipped);
});
```

★★ **这段就是 P17-1② 的"正确姿势"**：
- worker 里**只**读 `cancel_requested()`（原子位）与两个 sink；
- 所有控件更新走 `message(...)`（排队回主线程）或 `QueuedConnection`；
- **取消/失败/成功三种结局都必须 message**（零静默）。

⇒ 若某个页/夹具在 worker 里直接 `ui.set_text`，就会被门面线程闸门拒 + WARN。
  这与 `PyUi::available` 的 WARN 对得上。

## 3. 统计口径（第 175-202 行）
`stats_text()` 三行 = gameplay / scripts / **总计**；总计是**逐字段相加**，
"不是再数一遍 —— 免得两处口径分叉"。

## 4. 对话框起点（第 268-275 行）
`source_dialog_start()` / `target_dialog_start()` 都是 `dialog_start_for(...)`：
- 源：`{edit.text()}`, input_is_file=false ⇒ 只有"唯一输入目录"才认；
- 目标：`{source_edit_->text()}` ⇒ 用源目录当起点。

★ 注释写明"两个'选择'调用点的起点判据（与按钮 lambda 里那一行同源）"——
**判据函数与 UI 调用点共用同一份**，这又是"同类只维护一套"。

## 5. 一个可复用的检查清单（读这一页能提炼出来的）

| 检查 | 为什么 |
|---|---|
| 每个 `attach_row` 的 container 有 `field_row` 名 | ⑭ 的候选集 |
| 长列表是否 `make_inner_scroll` + self_scrolling | 否则滚不动（H2-4） |
| 是否有"无父控件"（`new QLabel(某 QString)` 之类） | F3 那类孤儿 |
| worker 里是否只碰 sink/原子位 | 线程闸门 |
| 取消/失败/成功三态是否都 message | 零静默 |

## 6. 待继续

`pages/` 剩 5 个：fontpack(421)、convert(616)、xml(761)、video(1055)、fs(1952)；
另有 `font_picker.cpp`(348)、`smoke.cpp`(10390)。
