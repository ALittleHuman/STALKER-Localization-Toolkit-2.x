# src/engine/plugin 批一：`widget_facade.cpp`（470 行，全文读完）

> 这是我们这几轮加了大量探针的那个文件。读完全文后，**P5/P13/P17 的机制链路终于完整**。

---

## 1. 进程级状态（第 14-43 行）

```cpp
std::mutex& facade_mutex();              // 保护下面全部
const UiFacade*& facade_storage();       // 门面指针
UiHandle& panel_root_storage();          // 当前面板根
std::string& page_id_storage();          // 当前页 id（P13-2a-2）
Registry*& page_registry_storage();      // 当前页的注册表
```

注释写得很清楚（第 14-17 行）：
> 用 `std::mutex` 保护**不是为了并发**（UI 全在主线程），而是为了"读的人不会读到写一半"
> —— 探针/自检可能从别的线程问可用性。

⇒ 所以 `set_current_panel_root` 先取锁、改值、**放锁**，然后再写 trace（我加的）；
  如果持锁时调 `current_page_id()`（也要同一把锁）就会**自死锁** —— 这正是我在 2026-10-22 踩的坑，
  现在代码里已经用"无锁版 `trace_event_raw`"解决了。

---

## 2. ★★ 两张必须逐项对上的名单（第 46-196 行）

```cpp
const char* const kMethodNames[] = { "label", "button", ... };   // 110 项左右
bool has_impl(const UiFacade& f, const std::string& name) { if (name == "label") ... }
```

这是**同一份事实的两种写法**（名字表 + 判定链）。第 114-116 行记着 P16-0 的翻车：

> ★★ P16-0：补登记（修前只有字段与 Qt 实现，`kMethodNames`/`has_impl` 都没有 ⇒
> `ui_facade_has("copy_text")` 恒 false、`api.ui.copy_text()` 恒返回 False，
> 而 xml 插件不看返回值照样显示"已复制 ID"）。见 `tests/check_facade_registry.py`。

⇒ **这正是我们该警惕的形态**：我加的 `menu_ready` / `page_ready` 钩子如果只在实现里有、
  不在任何登记表里，就会长出同族 bug（"看着在、实际拿不到"）。
  ⇒ 审计建议：任何新钩子必须同时出现在**实现 + 登记 + 判据**三处。

### `ui_facade_available()` 的语义（第 210-230 行）
**要求 `kMethodNames` 里每一个都有实现**，否则返回 false 并给出"缺方法 X"。
⇒ 这意味着：**部分实现 = 整个门面不可用**（不是"缺的那个不可用"）。
  判据/插件若依赖 `ui_facade_available()` 判"能不能用"，会得到很粗的结论。
  真正细粒度的是 `ui_facade_has(name)`。

---

## 3. ★ 跨线程白名单只有**两项**（第 249-277 行）

```cpp
const char* const kWorkerSafeMethods[] = { "task_cancel_requested", "on_ui_thread" };
```

理由逐条写在注释里：
- `task_cancel_requested` 只读 `TaskShell` 的**原子取消位**（插件 worker 轮询取消的唯一入口）；
- `on_ui_thread` 只比线程 id。

⇒ **P17-1② 的关键事实**：`task_cancel_requested` **在**白名单里 ⇒ worker 里读它是**合法的**。
  我们看到的偶发"没读到"不是契约问题（与 `widget_facade.h:334-351` 的注释一致）。

---

## 4. `set_current_panel_root` 的实现（第 283-299 行）

```cpp
{ lock; old_root = storage; page = page_id_storage(); storage = root; }
detail << "old=" << old << " new=" << root << " page=" << page << " reason=" << reason
       << " thread_std=" << hash(this_thread);
trace_event_raw(page, "root_set", detail.str());
```

- **锁内只做赋值**（不做 I/O、不调别的加锁函数）——正确的锁粒度；
- trace 在**锁外**写（`trace_event_raw` 自己取 `trace_ring_mutex`，与 `facade_mutex` 不同一把）；
- `page` 是**锁内快照**的，所以 trace 里的 page 是"改之前那一页"，与 `old_root` 同一时刻。

⇒ 这一点很重要：`root_set|page=` 反映的是**写之前**的页上下文。
  判"谁把 root 写空"时，如果看到 `page=` 与预期不符，先想这是**写之前**的页。

---

## 5. trace 管线（第 312-413 行）—— 我们加的

- `page_trace_enabled()`：`XRKIT_PAGE_TRACE=1` 才开（static 缓存一次）；
- `page_trace_filter()` + `page_trace_accepts()`：**ASCII 子串**（`find`），不按整串比；
- `trace_ring`：4096 条，满则 `erase(begin())`（丢最旧）；
- `PageTraceAutoFlush`：静态对象构造时 `std::atexit([] { flush_page_trace(); })`
  —— **引擎自己兜底**，不依赖外部调用；
- `flush_page_trace()`：swap 出 ring → 追加写 `%TEMP%/page_trace_engine.txt`；
  **幂等**（ring 空就返回）；
- `trace_event_raw`：`thread_local engine_last_page` 兜底空页；`place` 默认不记
  （`XRKIT_PAGE_TRACE_PLACE=1` 才记）；行格式
  `page|kind|detail|monotonic|hash(thread)|thread`。

★ 一处**已知的设计不一致**（不是 bug，但读 trace 时要记住）：
  - 引擎侧 `page_trace_engine.txt` 用 **append**；外壳侧 `page_trace_shell.txt` **首写 Truncate**；
  - 所以"跑一次"后，engine 文件可能含**多次运行**的行（`xrkit_app` 每次启动 append），
    而 shell 文件只含最后一次。**看 engine 文件时必须按 monotonic 或只看尾部**。

---

## 6. `UiPanelScope` 的两种构造与析构（第 425-468 行）

```cpp
UiPanelScope(UiHandle root)                       // 1 参：只设 root，**不设 page**
UiPanelScope(root, page_id, registry)             // 3 参：连页面身份一起设
~UiPanelScope() {
    trace_event("scope_dtor", ... current_before/own_root/lifo ...);
    set_current_panel_root(previous_, restores_page_context_ ? "scope_dtor" : "py_scope_dtor");
    if (restores_page_context_) set_current_page_context(previous_page_id_, previous_registry_);
}
```

⇒ **Python builder 走的是 1 参构造**（`py_host.cpp:374`），所以：
  - py 层的 scope **只保存/恢复 root**，**不碰 page_id**；
  - builder 里看到的 `current_page_id()` 是 **Hub 那层 3 参 scope** 设的；
  - py 层 dtor 后 `page_id` 仍是 Hub 设的那一页（直到 Hub 的 scope 析构才还原）。

★ 这解释了"钩子在页上下文还原后触发时 page 为空"——那是 **Hub scope 已析构**之后的事。

---

## 7. 本批的可执行结论

1. **新钩子必须三处齐全**（实现 + 登记 + 判据），否则就是 P16-0 同族（A.2）。
2. **`ui_facade_available()` 是"全有或全无"**，细粒度判断要用 `ui_facade_has(name)`（A.2 末）。
3. **`root_set|page=` 是"写之前"的页**（A.4）。
4. **engine trace 是 append 的**，跨运行累积；shell 是 truncate 的（A.5）。
5. **`task_cancel_requested` 合法可从 worker 调用**（A.3）—— P17-1② 不是契约问题。

## 8. 待核对

- `tests/check_facade_registry.py` 是否真的逐项比对 `kMethodNames` 与 `has_impl`
  （读 `tests/` 时核）。
- `py_host.cpp:374` 的 `UiPanelScope scope(parent);`（1 参）—— 下一批读 `py_host.cpp` 时确认。
