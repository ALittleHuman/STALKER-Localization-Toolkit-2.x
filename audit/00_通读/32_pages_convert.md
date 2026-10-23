# `pages/convert_page.cpp`（732 行，全文读完）

> 这是六页里**最长寿、坑最多**的一页（编码确认弹窗、暂停门、四个状态收口都在这里）。

---

## 1. ★★ P17-87：编码确认器 —— `BlockingQueuedConnection` 的正确用法（第 88-120 行）

```cpp
if (etxt::has_encoding_confirm()) return;          // ① 已经有确认器（自检替身）⇒ 不抢装
QThread* const ui_thread = QThread::currentThread();
etxt::set_encoding_confirm([owner, ui_thread](req, chosen) {
    const auto ask = [owner, &req]{ QMessageBox box(owner); … return box.exec()==Ok ? Confirmed : Cancelled; };
    if (QThread::currentThread() == ui_thread) return ask();      // ② 同线程**直接弹**
    … QMetaObject::invokeMethod(owner, …, Qt::BlockingQueuedConnection) … // 工作线程投递回主线程
    if (!invoked) return Cancelled;                                // ③ 投递失败 ⇒ 保守按取消
});
```

★★ **两条"不抢装"的判据**（注释说是本批踩出来的）：
1. 已有确认器（自检/测试装的替身）⇒ **不抢** —— 否则自检会在离屏环境里弹真模态、**永久卡住**；
2. **`BlockingQueuedConnection` 在同一线程上自投递会死锁**（自己排队、自己等自己）
   ⇒ 必须按"当前线程是不是安装时那个线程"分两支。

★ `~ConvertPage()` 里 `etxt::set_encoding_confirm(nullptr)`：
它捕获了 `this`，页面一走必须撤 —— 否则进程级留着指向死窗口的回调
（本批实测症状：**自检卡在模态上不动**）。

⇒ 这三条合起来是"**进程级回调的注入/撤销纪律**"的完整样板。

---

## 2. ★★ P17-1 / F-02：暂停等待**必须能被取消打断**（第 399-415 行）

```cpp
bool ConvertPage::wait_if_paused(bool block) {
    if (!paused_.load()) return true;
    ++pause_gate_hits_;
    while (block && paused_.load()) {
        if (task_ && task_->cancel_requested()) return false;   // ★ 取消位并进循环
        sleep_for(10ms);
    }
    return !paused_.load();
}
```

修前事实：循环只看 `paused_`，而 `~TaskShell()` **只置取消位、完全不碰 `paused_`**
⇒ 用户点「暂停」后关窗：worker 卡在 `while(…paused_)` 里**永远不出来** ⇒ 主线程 join 永久阻塞
⇒ **进程关不掉**。

⇒ 规则：**任何阻塞等待都必须能被取消打断**。

---

## 3. ★★ P17-122：四个状态**各自独立判**，互不覆盖（第 460-486 行）

```cpp
if (needs_confirm)  → "需要用户确认（未转换）：成功…/跳过…/失败…"
else if (cancelled) → stop_reason
else if (exit_code != 0) → "完成，但有失败：…"
else → "全部完成：…"
```
修前是一条 `exit_code != 0 → cancelled → 全成` 的链，而"需要确认"与"取消"都带非 0
`exit_code` ⇒ 两者都被"有失败"吃掉：
- 需要确认 ⇒ 显示「完成，但有失败：成功 0 / 跳过 0 / 失败 0」（自相矛盾）；
- 取消 ⇒ 显示"有失败"而不是 `stop_reason`。

★ 并且 `qualifier` 把"低置信 / 否决声明 / 判定≠实解"三个计数一并收口
（"不是失败，但用户必须一眼看到这批里有多少条是猜的"）。

---

## 4. ★ 页内视口的正确接线（第 225-245 行）—— 与 text_page 同款

修前 `addWidget(rows_, 1)` ⇒ 行数超了**永远够不着**（用户原话："编码转换的中间那块区域并没有设计滚动条"）。
修法三条（缺一不可）：
1. `make_inner_scroll(rows_, "convert_rows_viewport")`；
2. `set_self_scrolling_body(true)`（**不挪**行列表，否则 `top_` 恒 0 ＝ 虚拟化被关掉）；
3. `set_offset_delegate(set_scroll_top)` —— **不做这一步，用户拖条子看不出任何变化**
   （自检 #6 ③ 实测：`value=1935` 而 `scroll_top()=0`）。

---

## 5. 备份勾选框的启用逻辑（第 212-216、355-360 行）
输出目录非空 ⇒ 备份框**取消勾选并禁用**（备份只对"原地转换"有意义）。
`backup_extensions()`：扩展名为空 ⇒ 退回 `.xml`（否则按钮"点了没反应"）。

## 6. 四个 worker 的 hooks 形状（第 514-535、556-599、620-662 行）
```cpp
hooks.should_stop = [this]{ return task_ && task_->cancel_requested(); };
hooks.gate        = [this]{ return wait_if_paused(true); };
hooks.message     = [&message](const std::string& t){ message(t); };
hooks.progress    = [&progress](int done, int total){ progress(total<=0?100:done*100/total); };
```
结果回主线程用 `QMetaObject::invokeMethod(…, QueuedConnection)`（`record_stats`/`set_busy`）。

★ 三个 worker（转换/恢复/清空）的"零静默"分支各自都覆盖了：
`dir_missing` / `total==0` / `cancelled` / 正常 + 逐条 `errors`。

## 7. `export_stats` 的"没有快照"（第 693-717 行）
没有统计快照 ⇒ 用**当前界面值**，计数留 0，并明确提示"还没有可导出的统计"（不静默）。

## 8. `open_output` 的三条失败路径（第 669-691 行）
无输出目录 ⇒ 退源目录；目录不存在 ⇒ 明说；`openUrl` 失败 ⇒ 明说
（1.x 原先**忽略返回值** ⇒ 点了没反应）。

## 9. 对话框起点（第 719-730 行）
与其它页一致：判据函数与按钮 lambda **共用同一份**。

---

## 10. 这一页给出的新增检查项

| 检查 | 为什么 |
|---|---|
| 进程级回调（确认器）是否在析构里撤销 | 否则指向死窗口 |
| 阻塞等待是否有取消出口 | 暂停 + 关窗 = 进程关不掉 |
| 状态分支是否互不覆盖 | `exit_code` 不能同时表达四种语义 |
| 页内视口三件套是否齐 | 缺一件就"拖了没反应" |
