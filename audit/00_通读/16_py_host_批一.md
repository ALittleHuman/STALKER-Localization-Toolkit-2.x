# `src/engine/plugin/py_host.cpp` 批一（第 1-600 行 / 共 4146 行）

> 这个文件是整个插件体系的**实现主干**。分批读，逐段记。

---

## 1. 文件作用域 vs 匿名命名空间：作者自记的两个编译错误（第 40-45 行）

> `PyPluginApi::log`（在匿名命名空间里）要调它，而一个匿名命名空间从第 40 行一直开到三千多行：
> · 声明写在匿名命名空间里、定义写在文件作用域 ⇒ **两个不同的函数** ⇒ `LNK2019`；
> · 声明写在文件作用域、调用点在匿名命名空间里且在声明之前 ⇒ **C3861 找不到标识符**。
> ★ 这两条都是我**真踩过的**（P17-97 回退一次、P17-98 再踩一次），所以写在这里当路标。

⇒ 这正是我们改这个文件时最容易踩的地方（我加探针时也遇到 `trace_event_raw` 的前置声明问题）。
  **规矩**：文件作用域的声明＋定义必须同命名空间、同 `#ifdef`。

---

## 2. 插件日志缓冲（第 46-77 行）：写侧唯一入口

- `plugin_log_mutex()` / `plugin_log_storage()` 用 `new` 出来**永不析构**
  （"静态析构顺序会咬人"）；
- 容量 200 行，满了 `erase(begin())`；
- `append_plugin_log_line` 是**写侧唯一入口**（带锁）；读+清空是 `take_python_log_lines`
  （同一把锁里 swap 出来）。注释："**两个函数以外不许再直接碰那个 vector**"；
- ★ 没编 Python 时**也要有定义**（`--selftest` 的跨线程压力判据在两种构建里都跑）。

⇒ 这正是 `registry.h:57-59` 提到的"另一处并发点已加锁"的落地。

---

## 3. `PyHostStatus` 的 `enabled` / `reason`（第 96-109 行）

没编 Python 时 `enabled=false` + 一条**带原因**的 message（"构建期未启用 CPython 宿主（CMake 选项…）"）。
编了但没起来时，`reason` 若为空则填"解释器尚未启动（还没调用 init_python）"。

⇒ **零静默**：任何时候 `enabled=false` 都必须能读出"为什么"。

---

## 4. ★★ `when_from_python`（第 196-244 行）：`repr` 而不是 `str`

修前值被 `str` 化 ⇒ `1`、`True`、`"1"` **全都变成 `"1"`** ⇒ 类型信息在比较前就没了，
于是 `when={"n": 1}` 会被宿主喂进来的字符串 `"1"` **命中**（那不是插件写的意图）。
改法：`repr` 把类型也写进那串字（`1`→`"1"`、`"1"`→`"'1'"`、`True`→`"True"`）。

★ 另有一处细节：`if (!item.second.is_none()) value = py::repr(...)` ——
**`None` 不取 repr**，保持 `nullopt`（表示"期望这个键不存在"）。

可调用 `when` 走 `spec.predicate`：**先保活**（`keepalive()->push_back`）、
取 GIL、构造 ctx dict、`catch (error_already_set) → false`（12.6 口径）。

---

## 5. `wrap_python_handler`（第 246-260 行）

把 Python 可调用包成 `CommandHandler`：取 GIL，`error_already_set` **转成 `std::runtime_error`**
交给 `Registry::invoke` 记账（"绝不冒到 Qt 事件循环里"）。

⇒ 这是"异常必须在本层转成 C++ 异常"的样板：**Py 异常不能穿到 Qt 事件循环**。

---

## 6. `PyPluginApi`（第 271-414 行）

### 注册面（第 287-351 行）
`register_command` / `register_menu_item` / `register_panel` / `register_tool` /
`register_option_entry` / `register_option` —— 每个都：
- 先 `PyCallable_Check`，不是可调用就 `registry_->add_error(...)` 并返回 false/none（零静默）；
- 走 `wrap_python_handler` / `wrap_python_builder`；
- `register_tool` 额外打三条 trace（`register|plugin=… name=… callable=0/1` → `ret=0/1`）。

### ★★ `wrap_python_builder`（第 354-409 行）—— **P5 的核心**

```
① gil_scoped_acquire
② py_builder_enter|facade=0/1 on_ui=0/1/-1 thread_std=…   ← 我 2026-10-23 加的探针
③ py_builder_parent|parent=0/1
④ UiPanelScope scope(parent);          ← **1 参构造 ⇒ 只设 root，不设 page_id**
⑤ py_builder_scope|root_after=0/1
⑥ result = kept()                      ← 调 Python build()
   ├ result is None ⇒ return kNoWidget（合法：插件只往根里塞控件）
   └ 否则 handle_of_widget(result)
⑦ catch (error_already_set / std::exception):
   core::log(Error) + registry->add_error + return kNoWidget
```

**三条关键事实**（读代码确认，不是推测）：

1. **builder 抛异常会被抓住**，记 `add_error` + `core::log`，然后返回 `kNoWidget`
   ⇒ 宿主走"占位页"路径。
   ⇒ 我们观测到 `build_error.txt` **缺失**（`门面树夹具页` 那次）时，
   **说明 builder 根本没抛异常** —— 排除"异常被吞"这一支。
2. **`UiPanelScope scope(parent)` 是 1 参构造** ⇒ **不设 page_id**（`widget_facade.cpp:425-435`）。
   ⇒ 所以 builder 里 `current_page_id()` 是 Hub 那层 3 参 scope 设的；
   py 层析构时也只恢复 root。**"root 变 0"如果发生在 builder 内部，则只能来自 Hub 层**。
3. **`parent` 为空时 `UiPanelScope(nullptr)` 会把 root 设成 nullptr**
   （`scope_root_ = nullptr`、`panel_root_storage() = nullptr`）⇒ 之后所有 `ui.*(parent=None)`
   都会落到 null 根 ⇒ 一条控件也建不出来。
   ★ 我们实测到的 `py_builder_parent|parent=1` 说明这次 Hub 传进来了非空 parent；
   但**只要有一次 parent==nullptr**，就会复现"anything=0 且无异常"的形态。
   ⇒ **这是 P5 最可疑的单点**，下一批要在 Hub 侧核 `sync_pages()` 给 builder 传的到底是什么。

---

## 7. `PyWidget` 与句柄转换（第 416-437 行）

`PyWidget` **只有一个 `UiHandle`**，没有任何方法能解引用成 Qt 对象（表外无门）。
`handle_of_widget()`：None ⇒ `kNoWidget`；不是 `PyWidget` 实例 ⇒ `kNoWidget`（**静默**）。

⇒ 注意最后这条：**类型不对时静默返回 kNoWidget**。
  如果插件返回了别的东西（例如 Qt 包装的旧类型），宿主只会看到"没建出控件"，
  而**没有**任何错误记录 —— 这是"静默"的一处。

---

## 8. 入参转换（第 439-600 行）

- `tree_columns_from_python` / `tree_nodes_from_python`：list ↔ POD，缺字段取默认；
- `font_options_from_python` / `text_batch_options_from_python` / `text_backup_options_from_python` /
  `text_stats_from_python`：dict → 门面 POD，`take_string`/`take_u64` 逐个取；
- `text_hooks_from_python`：**hooks 会从工作线程被叫** ⇒ 保活 + GIL + 不让异常掀翻宿主
  （`catch → false`）。

⇒ 共同手法：**每个 dict 字段"有就取、没有就用默认"**，不抛。
  这正是"Python 友好值类型"的落地方式（`engine_facade.h:16-17` 的口径）。

---

## 9. 待继续（下一批 601-1000）

`text_hooks_from_python` 的后半、`PyUi`（控件门面在 Python 侧的实现）、
以及 `PyHost` 类本体（初始化解释器、加载插件、注册 `register(api)`）。
