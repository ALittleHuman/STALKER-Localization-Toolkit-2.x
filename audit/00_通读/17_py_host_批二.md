# `src/engine/plugin/py_host.cpp` 批二（第 601-4146 行，读完）

---

## 1. `PyUi`：控件门面的 Python 侧（第 2101-3119 行）

### 结构
每个方法都是同一个形状：
```cpp
if (const UiFacade* facade = available("xxx")) { return wrap(facade->xxx(...)); }
return py::none();   // 或 false / 0 / ""
```
回调参数一律：`PyCallable_Check` → 不合格就 `core::log(Warn, "... 不是可调用对象")` →
`keepalive()->push_back(kept)`（保活）→ 包成 C++ 回调（取 GIL、`catch(error_already_set)` 只记日志）。

### ★★★ `PyUi::available()`（第 3085-3112 行）—— **线程闸门的唯一落点**

```cpp
if (!ui_facade_has(method)) { 记 WARN「控件门面不可用（ui.X）：<reason>」; return nullptr; }
const UiFacade* facade = ui_facade();
if (facade && facade->on_ui_thread && !facade->on_ui_thread()) {
    if (ui_method_worker_safe(method)) return facade;      // 白名单两条
    记 WARN「控件门面只许在宿主 UI 线程调用（ui.X 在别的线程被调用，已拒绝）」;
    return nullptr;
}
return facade;
```

⇒ **这是 P5/P12c-2 最关键的机制**：
- 门面没装 ⇒ **每个 ui.* 调用都打一条 WARN**（"控件门面不可用"）；
- builder 跑在非 UI 线程 ⇒ **每个 ui.* 调用都打一条 WARN**（"只许在宿主 UI 线程调用"）。

★★ **所以"ui.* 全空且无异常"一定会在日志里留下整片 WARN。**
  ⇒ 判"页为什么是空的"，第一条就该去**日志里数这两类 WARN** ——
  一条都没有 ⇒ 门面在、线程对 ⇒ 那就是**插件代码自己没调 ui.***（或调了但 parent 解析成 null）。
  这正是我们 `门面树夹具页 anything=0 且无异常` 那次的下一步验证手段。

### 其它值得记的
- `PyWidget` 只有 `__repr__` 与 `__bool__`；**布尔化 = 句柄非空**（第 3276-3280 行）。
  ⇒ 插件写 `if not label: return` 就能判"没造出来"。
- `filetypes_to_qt`（第 2304-2322 行）：只认**两元组**，其它形状**跳过不猜**。
- `task_run`（第 2638-2669 行）：worker 异常 → `core::log(Error)` + **`message("插件任务抛异常：…")`**
  （零静默：显示在任务壳上）。

---

## 2. `ensure_api_module()`（第 3121-3478 行）

- 走 `sys.modules["xrkit"] = m`（**不用** `PyImport_AppendInittab`，理由："与 PyConfig 初始化路径的
  交互随版本变"）；
- 类注册**只做一次**（`static bool class_registered`）—— 与"`shutdown_python()` 后不支持重启"配套；
- 注册 5 个类：`Host` / `Engine` / `Ui` / `Widget` / `Api`，方法表就是 `PyUi`/`PyEngine`/`PyHost` 的成员；
- `Api` 的只读属性：`plugin_file` / `plugin_dir` / `api_version` / `ui` / `engine` / `host`。

⇒ **这里是"表外无门"的实际边界**：Python 能看到的只有这些 `.def`。
  `tests/check_ui_capability_table.py` 用 `--ui-facade` 探针把 `dir(xrkit.Api/Ui/Widget/Engine/Host)`
  逐条打出来对账，**多一个就红**。

---

## 3. `resolve_python_home()`（第 3492-3520 行）：四步

① 显式参数 → ② `XRKIT_PYTHON_HOME` 环境变量 → ③ exe 旁 `python\`（发行包）→
④ exe 旁有 `python3XX.zip`（**扁平** embed 形态）→ ⑤ 构建期烘焙的 home → 空串。

★ ③ 排在 ④ 之前，理由："发行包在自己的机器上跑时，**包内那份**才是唯一能保证版本一致的"。
★ ⑤ 必须有（实测）：不设 home 时嵌入解释器按**宿主 exe 所在目录**推 prefix
⇒ 去 `build/…/tools/cli/Lib` 找标准库 ⇒ `Failed to import encodings module`。

---

## 4. `init_python()`（第 3522-3645 行）—— 三个硬教训

### （a）★★ P17-106：**先判"已经起着吗"，再判"曾经起过吗"**（第 3541-3548 行）
> ★★ **顺序有讲究**（我这里第一版就写错了，门禁当场抓到）：
> **先**判"已经起着吗"（幂等直接返回），**再**判"曾经起过吗"。
> 反过来的话，宿主正常活着时再调一次 `init_python()`（`load_python_plugins_from_dir`
> 就是不带 home 再调一次）会被当成"重启"**误拒** ⇒ `--py-plugins` 整条链全红。

⇒ **"幂等判断"必须排在"拒绝条件"之前** —— 这是所有"一次性初始化"函数的通用坑。

### （b）扁平 embed 形态必须**显式给 `module_search_paths`**（第 3575-3609 行）
只设 `home` 仍然 `Failed to import encodings module`；还要加 `.zip`（标准库）＋ home（.pyd）
＋ 存在的话 `DLLs`/`Lib`。

### （c）GIL 必须放开（第 3637-3640 行）
`main_thread_state() = PyEval_SaveThread(); status.gil_released = ...`
—— 注释："主线程一直攥着它的话，工作线程里回调进 Python 会死等"；
`gil=released` 是门禁 `[python-host]` 步的判据之一，改回"不放"即红。

---

## 5. `load_python_plugins_impl()`（第 3727-3856 行）—— 加载主流程

```
① 扫目录（不依赖 Python：没有宿主时也要如实说出"有几个 .py 没加载"）
   · 目录不存在 ⇒ 一条 error，**不算启动失败**（宿主照常起来）
   · 没有 .py ⇒ 一条都不记（不制造噪声）
② 没有 Python 支持 ⇒ 一条 error（带 python_status().reason）+ registry.add_error
③ init_python()；没起来 ⇒ 一条 error（带 status.reason）
④ for file in sorted(files):
     trace load_start
     spec_from_file_location → module_from_spec → exec_module
     ★ PLUGIN_API 检查在 register(api) **之前**（比宿主新 ⇒ 当场拒绝；比宿主旧 ⇒ 照常）
     ★ 没有 register / 不可调用 ⇒ 抛
     register_fn(api)
     register 返回 False ⇒ 抛
   catch ⇒ failure
   if failure:
       registry.rollback_plugin(owner)          ← **先回滚再记账**
       errors.push_back({owner, failure+"（已按 owner 回滚 N 条注册）"})
       registry.add_error(owner, ...)
       core::log(Warn, "Python 插件加载失败：" + owner + "：" + failure)
       trace load ret=error
       continue
   ++loaded; registrations += residue_for(owner).registrations(); ++loaded_python_plugins()
   core::log(Info, "Python 插件已加载：" + owner)     ← ★ 这一行
   trace load ret=ok
```

### ★★★ 与我们"日志「已加载」"那条红直接相关

`core::log(Info, "Python 插件已加载：" + owner)` 走的是 **core 日志**：
写 **stderr** ＋（若挂了文件）**日志文件**。
而判据那条检查的是 **`log_hub.log_view()->toPlainText()`（LogBox 控件文本）**。

**这两条通道不是同一条**：
- `core::log` 与 `log_block` 都不碰 LogBox；
- LogBox 只接收宿主自己 `log_line(...)` 写进去的行（含 `UiFacade::log_append` 那条路）。

⇒ **"已加载"这三个字很可能从来没进过 LogBox**，而判据却在 LogBox 里找它
  ⇒ 表现为"日志长度 849（有内容）但找不到「已加载」"—— **正好是我们观测到的形态**。
  ⇒ **这是判据侧的读数错位，不是产品 bug。**
  下一批验证：把那条 check 改成读 **日志文件**（`Hub::log_file_path()`）或直接读 stderr 捕获。

### 另外两条
- `PLUGIN_API` 检查放在 `register` 之前 ⇒ 拒绝时**一条注册都不留**（不需要回滚，但仍兜底）；
- 失败时**先 `rollback_plugin(owner)` 再记账**（"半个插件的注册留在表里＝指向坏插件的回调"）。

---

## 6. `run_python_host_probe()`（第 3895-4144 行）

四个开关：`--python-host` / `--py-plugins [目录]` / `--ui-facade` / `--py-plugin-call [目录] [JSON]`。
退出码：**0=通 / 1=有失败 / 2=构建期没带 Python / 3=用法或 JSON 错**。

### 三条实测血账（都写在注释里）
1. **GIL 不能嵌套取**（第 4020-4023 行）：`init_python()` 已 `PyEval_SaveThread()` 放开 GIL，
   外面再套 `gil_scoped_acquire` = 两套记账叠加 ⇒ **访问违例**（三个探针插件全崩 0xC0000005）。
   ⇒ "GIL 在下面**只取一次**"。
2. **判"没找到"必须用 `!entry` 而不是 `entry.is_none()`**（第 4055-4058 行）：
   后者比的是 `m_ptr == Py_None`，对**空指针**返回 false ⇒ 判据放过 ⇒ 后面调空指针 ⇒ 0xC0000005。
   ⇒ **"空对象"与"None"是两件事**（与 `LinkProbe::StatFailed` / `truncated` 同族）。
3. **`py::object` 的析构必须在持 GIL 时**（第 4097-4099 行）：`call_modules.clear()` 要放在
   持锁区里，否则 crash 出现在"调用成功之后"，看着像巧合。

---

## 7. 这份文件给"红点"的可执行结论（三条，按优先级）

1. **★★ 日志「已加载」是判据读数错位**（见 5）：`core::log` 不进 LogBox。
   ⇒ 判据应读**日志文件**/stderr，而不是 `LogBox::toPlainText()`。**这条可以立刻修。**
2. **★★ "ui.* 全空"必然伴随整片 WARN**（见 1）：判 P5 的空白页时，
   第一件事是在日志里数「控件门面不可用」/「只许在宿主 UI 线程调用」两类 WARN。
   没有 WARN ⇒ 门面与线程都正常 ⇒ 问题在**插件代码/builder 本身**。
3. **"空对象 ≠ None"**（见 6.2）：我们判据里凡是用"取不到就是空"的地方，都要问
   "是真的空，还是我没取到？"

## 8. 待继续

`src/engine/plugin/` 还剩 `registry.cpp`(707)、`engine_facade.cpp`(184)、
`engine_facade_impl.cpp`(1060)、`host_facade.cpp`(72)、`host_facade_impl.cpp`(58)、`loader.cpp`(11)。
