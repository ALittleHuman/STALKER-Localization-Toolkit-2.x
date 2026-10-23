# `src/shell/task_shell.cpp`（327 行，全文读完）—— **P17-1② 的真因在这里**

---

## 1. `run()` 的顺序（第 124-188 行）

```
1. worker == nullptr ⇒ return false（不计 rejected_）
2. if (running_) { ++rejected_; return false; }        // 重入闸门（主线程同步置位）
3. running_ = true
4. reap_finished_workers()                             // ★ 在闸门**之后**（P17-4）
5. prev_status_ / run_line_ / error_text_ / last_progress_ 复位
6. cancel_requested_.store(false)                      // ★ 每轮 run 都清零
7. cancel_ 文案复位、bar 归零、status 置 title
8. 造两个 sink（progress / message）——都是 **QueuedConnection 回主线程**
9. workers_.push_back(thread{ ... })
       worker(progress, message)
       catch ⇒ invokeMethod(apply_error)   ← 排队，先于 finish
       invokeMethod(apply_finish)          ← 排队
       if (hook) invokeMethod(hook)        ← 排队（★ 我的探针）
       done->store(true)                   ← **最后一句**
```

★ `done->store(true)` 在**最后**，`reap_finished_workers()` 以它判定"这一格跑完了"
（注释：join 一个刚置位的线程最多等几微秒）。

---

## 2. `request_cancel()`（第 225-242 行）—— ★★ P17-1② 的关键

```cpp
void TaskShell::request_cancel(const QString& reason) {
    if (!running_) return;            // ★★ 没在跑 ⇒ 什么都不做
    cancel_requested_.store(true);
    ...
}
```

注释写明这是**有意的**：「空闲时点一下不留痕（否则会污染下一轮的结论）」。

### ⇒ P17-1② 失败的真因

夹具的 worker 是一个**很快跑完**的循环。测试流程是：
`task_run(...)` → 点「取消」→ 等 marker 里出现 `cancel_after`。

若**点取消时 worker 已经跑完**（`apply_finish` 已把 `running_` 置回 false）⇒
`request_cancel()` **第 227 行直接 return** ⇒ `cancel_requested_` **从未置位** ⇒
worker 当然读不到 ⇒ marker 里**永远不会有 `cancel_after`**。

⇒ 这不是产品 bug（语义是对的、注释也写了），而是**判据的时序假设不成立**：
  "点了取消就一定在跑着"这条前提没有保证。

### 三种可修的方向（下一批择一）
1. **夹具让 worker 等到被取消为止**（带一个总超时，例如最多 2 秒），
   这样"点取消时一定还在跑"成为**结构保证**；
2. 判据接受**两种合法结局**：`cancel_after` **或** `worker_done 早于点击`（并断言"若已完成则取消不改状态"）；
3. 在点击前断言 `task_status`/`running` 仍为真 —— 最弱，因为仍有竞态窗口。

★ 我倾向 1（结构保证）＋ 2（把"已完成"作为**具名合法结局**记进 note）。

---

## 3. `apply_finish()`（第 284-318 行）：状态栏定稿的优先级

```
next = final_status
if (next 空 且 无错误 且 status_text_ == run_line_)  next = prev_status_   // 13.4
if (next 空 且 无错误)                                next = idle_status_   // 13.5
if (有错误)                                           next = "失败：" + error_text_
if (cancel_requested_ 且 cancel_reason_ 非空)          next = cancel_reason_ (+ 错误)
if (next 空)                                          next = idle_status_ 或 "就绪"
→ status_text_ / status_->setText / events_ 记 'f' → finish_handler_()
```

★ 取消理由**优先收尾**（否则会被 worker 最后一句话盖掉），并且**错误不许被吞**。

---

## 4. `apply_message`（第 263-273 行）：P12c-2 的合法通道

```cpp
if (on_main && message_handler_) message_handler_(text);
```
worker 里 `message(...)` → 排队回主线程 → 这里在主线程调 handler ⇒
**handler 里可以碰控件**（P12c-2 的"干完活刷新界面"就走这条）。

⇒ 若 P12c-2 的回调没改到控件，先确认它走的是 `task_on_message`（或 `finish_handler`），
  而不是在 worker 里直接 `ui.set_text`（那会被门面线程闸门拒 + WARN）。

---

## 5. `~TaskShell`（第 62-85 行）：先取消再 join

审计 3-6 的原话：「`~TaskShell` 只 `join()` 不 `request_cancel()` ⇒ **暂停 + 关窗 ⇒ 进程必须退出**」。
修法：析构里**直接写原子位**（不走 `request_cancel`，因为它有 `if (!running_) return;` 的闸门），
然后 `stop_and_wait()`。

★ 残余如实登记：**没有取消位的页**（`CancelButton::No`）其 worker 内部没有轮询点
⇒ 置了位也得等它跑完，析构仍会阻塞到那一步。

---

## 6. `report_*` 四个公开入口（第 190-221 行）：P17-5

修前它们直接 `apply_*` ⇒ "只能在主线程用"，从 worker 调就是跨线程碰控件（偶发崩溃）。
现在一律 `on_main_or_queue`：主线程直落，其它线程 `QueuedConnection`。

⇒ **"公开 API 的地雷"**这个说法值得抄：同族函数一半线程安全一半不安全，比全不安全更危险。

## 7. `CancelButton::No` 时**连控件都不造**（第 40-55 行）
不是隐藏、不是置灰（"那两种都还是假的取消"）。用户原话：「本质是完全没有的，故直接删除即可」。

## 8. 结论

1. **P17-1② 是判据的时序假设问题**（点取消时 worker 可能已结束 ⇒ `request_cancel` 空转）。
   ⇒ 修夹具（让 worker 等到被取消）＋ 把"已完成"作为合法结局具名化。
2. **`running_` 闸门在 `reap` 之前**是有意的（reap 里 join 会阻塞 UI）——我之前的实验证明了这一点。
3. `apply_message` 是唯一"回调里能碰控件"的合法通道。
