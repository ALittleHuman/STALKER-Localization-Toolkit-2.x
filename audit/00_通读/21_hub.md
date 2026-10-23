# `src/shell/hub.cpp`（1054 行，全文读完）—— ★★★ P5 的根因在这里找到了

---

## 1. `sync_pages()`（第 483-755 行）：页装配的唯一一处

流程：① 摘掉不可见的页（`removeWidget` + `deleteLater`）；② 补/排（选择排序，逐位对齐）；
③ 计数与当前页还原；④ `sync` trace。

### 新建一页的完整序列（第 516-700 行）

```
container = new QWidget;                                  // ← builder 的父容器
{
    UiPanelScope panel_scope(container, page_key, &registry_);   // 设 root + page 身份
    built = reg->builder(container);                        // ★ parent 永远是 container（非空）
    ... settle trace（在 scope 内量）...
}
builder 抛异常 ⇒ built=nullptr + 日志（S-02）
页归属：built 是 PageBase ⇒ 页就是它（container deleteLater）；
        否则 ⇒ 页是 container（built 嵌进去）
anything_built = !builder_threw && (built != nullptr || placed->count() > 0)
    ⇒ 否则 make_placeholder + 日志「没造出控件，已用占位页顶替」
否则（有内容但不是 PageBase）⇒ 包一层 PluginPage（P16-2c）
setWindowTitle / set_subtitle / attach_page_services
page_ready 钩子
stack_->insertWidget / page_tabs_->insertTab / 三个数组 insert
```

★ **关键事实：`reg->builder(container)` 的实参永远是新建的 `container`，不可能为 nullptr。**
  ⇒ 所以 `py_builder_parent|parent=1` 是**必然**的；`current_panel_root()` 在 scope 内也**必然非空**。
  ⇒ 那 `resolve|... current_root=0` 是怎么来的？**请看第 3 节。**

### 三层"没造出控件"的判别（第 632-677 行）
| 情况 | 处里 | 日志 |
|---|---|---|
| builder 抛异常 | 占位页（一票否决，即使塞了半截控件） | 「builder 抛了 C++ 异常…已用占位页顶替（S-02）」 |
| `anything_built == false` | 占位页 | 「没造出控件，已用占位页顶替」 |
| 有内容但不是 PageBase | 包 `PluginPage` | （无，正常路径） |

⇒ **产线里"占位页"一定有日志**。我们观测到的 `anything=0` 那几次，日志里应该有
  「没造出控件，已用占位页顶替」—— 这是下一批要核对的第一个点。

---

## 2. `render_plugin_slots()`（第 773-987 行）—— 注意 `sync_pages()` 在循环里

```
for (owner : pages_.owners()) unregister_owner(...)
for (tool : registry_.tools_sorted()) {
    ... pages_.register_page(...) ...
    sync_pages();                              // ← 每注册一个 tool 就同步一次（第 829 行）
    drain_page_diagnostics();
    if (page_count() > 0) set_current_page(0);  // ← 每次都切回第 0 页！
}
sync_pages();                                   // 循环后再同步一次
```

★ **每注册一个插件页都会 `set_current_page(0)`** ⇒ 若判据此时正在看第 N 页，会被切走。
  ⇒ 这与 P5 的 `wait_page_ready` 在同一进程里跑时**有交互**（判据切页后如果又有重渲染，会被切回 0）。
  ★ 但重渲染只在 `load_plugins` 里发生一次（启动时），所以正常时序下不冲突。

---

## 3. ★★★ 根因：`unwrap()` 会**误把"新分配的地址"当成"已死的旧句柄"**

这是我从三处代码交叉读出来的，链条完整：

### （a）句柄活性注册表（`widget_facade_qt.cpp:240-260`）
```cpp
std::unordered_map<plug::UiHandle, QPointer<QWidget>>& handle_registry();
QWidget* unwrap(plug::UiHandle handle) {
    if (handle != nullptr) {
        auto it = handle_registry().find(handle);
        if (it != end && it->second.isNull()) return nullptr;   // ★ 已登记但控件已死 ⇒ 当没有
    }
    return reinterpret_cast<QWidget*>(handle);
}
```
并且注释明确写着（`widget_facade_qt.cpp:238-239`）：
> ★ **表不删死项**：删了之后同一个悬垂句柄会退回"没登记 ⇒ 非空 ⇒ true"，正好把本修法抵消。

### （b）`resolve_parent`（`widget_facade_qt.cpp:286-307`）
```cpp
QWidget* widget = unwrap(parent);
QWidget* resolved = widget != nullptr ? widget : unwrap(plug::current_panel_root());
```
⇒ 如果 `unwrap()` 对**当前面板根**返回 nullptr，`resolved` 就是 nullptr。

### （c）`sync_pages` 每次新建 `container = new QWidget`（`hub.cpp:526`）
**container 不是门面造出来的 ⇒ 从未登记。** 但如果**之前某个门面造的控件**占过这个地址、
并且已经被销毁（`deleteLater`），注册表里就留着一条 `地址 → 空 QPointer` 的死项。

### ⇒ 于是这条路径成立（**无需任何异常、无需任何 WARN**）

```
① 上一个页的某个门面控件在地址 A，被销毁（QPointer 置空，但注册表保留死项）
② 本页 new QWidget 恰好复用地址 A（分配器行为，取决于执行历史 ⇒ **随执行顺序漂**）
③ builder 里 ui.label(None) → resolve_parent(nullptr)
       → unwrap(current_panel_root()) = unwrap(A) → 命中死项 ⇒ **nullptr**
④ table()->label(nullptr, wrap(nullptr), …) ⇒ 造出一个**无父**的 QLabel
⑤ place(nullptr, label) ⇒ **第 327 行直接 return**（连 trace 都没有）
⑥ 容器里一个控件都没有 ⇒ `settle|placed=-1 anything=0 layout=none`
   —— 与 `builder_threw=false`、`parent=1`、`root_after=1` **完全吻合**
```

### 为什么这条能解释我们所有的观测
- `py_builder_parent|parent=1` ✓（Hub 一定传 container）
- `py_builder_scope|root_after=1` ✓（scope 刚设完，此时没人调 unwrap）
- `resolve|parent_arg=0 current_root=0` ✗→ 这里 `current_root` 打印的是 `unwrap(current_panel_root())`
  —— **注意 trace 打的就是 unwrap 之后的值**（`widget_facade_qt.cpp:293`），
  所以 "current_root=0" **不代表 `current_panel_root()` 真的返回了 nullptr**，
  而是**它被 unwrap 判成了死句柄**！
- `settle|anything=0 layout=none` ✓（place 从没被调用过，容器没布局）
- **无异常、无 WARN** ✓（`ui.label` 走的是正常门面路径，门面在、线程对）
- **偶发、随执行顺序漂** ✓（分配器地址复用是执行历史的函数）

### ★ 这条同时解释了"为什么 trace 开/关会影响通过率"
trace 开关改变 I/O 时序 ⇒ 改变对象分配/释放的先后 ⇒ 改变**地址复用是否发生**。

---

## 4. 修法（本轮只读，未改）

最小、最安全的一行：**在 `sync_pages` 新建 container 之后，把该地址的陈旧登记清掉**：

```cpp
auto* container = new QWidget;
plugin::forget_dead_handle(container);   // 或直接 handle_registry().erase(container)
```

（需要在门面侧暴露一个"这个地址我要重新用"的入口；等价于"新对象不该被旧死项遮住"。）

更根本的两条：
1. **注册表按"发号"而非"地址"记账**（句柄 = 序号，表里存 `QPointer`），彻底没有地址复用问题；
2. `unwrap()` 只在"这个句柄确实是我们发出去的"时才查表 —— 需要一张"在册"集合。

★ 而 `place(parent==nullptr)` 应该**记一条 trace + 一条 WARN**（现在完全静默）：
  这条本身就是"零静默"红线的缺口。

---

## 5. `Hub::load_plugins`（第 1019-1052 行）

- `dump_plugin_files(dir)`：加载前把 `.py` 的 size/mtime/sha256 写 `%TEMP%/plugin_files.txt`
  （判别"夹具没落盘"vs"落盘了没被扫到"）；
- `load_all_plugins_from_dir` → `plugin_errors_ = registry_.registration_errors()`；
- 逐条 `log_line("插件问题：…")`、`plugin_notice_`、`render_plugin_slots()`。

★ 注意：**"Python 插件已加载：xxx" 那条是 `core::log`（进文件+stderr），
  而这里的「插件问题」「插件页：」「插件槽位：」是 `log_line`（进 LogBox）**。
  ⇒ 判据若在 LogBox 里找「已加载」，只有 `core::log` 那条**不会**出现
  （与 `17_py_host_批二.md` 的结论一致）。

## 6. 其它值得记的

- `Hub::~Hub`（354-360）：`clear_qt_ui_facade_log_sink(this)` + 只在 `active_registry()==&registry_` 时复位；
- `trace_page_event` 已有 `append_shell_page_trace`（第 65 行），但**缩进错乱**（第 63-65 行）——我改的；
- `page_at()` 用 `dynamic_cast<PageBase*>`（不是 `qobject_cast`，因为 PageBase 无 Q_OBJECT）。

---

## 7. 下一步（把这个根因证实/证伪）

1. 在失败现场找「没造出控件，已用占位页顶替」这条日志（**应该有**）；
2. 在 trace 里找 `resolve|… current_root=0` 的那一页，看它前面是否有
   `container_delete_later`（被销毁的容器/控件）—— 地址复用会紧邻发生；
3. 打一条 `settle` 时同时记 `container` 的地址与 `handle_registry().count(container)`
   （是否命中死项）—— 一条 trace 就能定案。
