# src/engine/plugin 批三（收尾）：`engine_facade.cpp`(209) ＋ `engine_facade_impl.cpp`(1109)

> 这两个文件读完，**`src/engine/plugin/` 8/8 全部完成**。

---

## A. `engine_facade.cpp`（209 行）

- 与 `widget_facade.cpp` / `host_facade.cpp` 同构：`kMethodNames[]`（约 78 项）＋ `has_impl` 逐项；
- ★ **默认表**：`facade_or_default()` = 注入的？否则 `builtin_engine_facade()`。
  理由（第 145-148 行）：字体那几件只依赖引擎，而 CLI（`--py-plugins`）也是插件宿主 ——
  "实现在外壳里时，CLI 里的插件调 `font_size_slots()` 拿到的是 None"。
- ★★ **两个 available\* 不是一回事**（第 165-169 行，P17-39）：
  - `engine_facade_available(reason)` ＝ **整体**可用性（门面在不在）；
  - `EngineFacade::available(method)`（py_host 内部）＝ **单个方法**在不在。
  同名不同义；插件 UI 能力表走 per-method 那一侧。

⇒ 三张门面的注入语义：
| 门面 | 未注入时 |
|---|---|
| `UiFacade` | **null**（每个 ui.* 都打 WARN） |
| `HostFacade` | 引擎**默认表** |
| `EngineFacade` | 引擎**默认表** |

---

## B. `engine_facade_impl.cpp`（1109 行）

整份是**转发**：门面签名 → 引擎函数；注释明说"不在这一层加业务逻辑：阈值/默认值/文案一律从引擎拿"
（"本仓已经因为'旁边再写一份差不多的'被门禁拦下过两次"）。

### 1. 三套 `to_facade` / `from_facade` 转换（第 67-215 行）
`XmlCompareResult`、`XmlCsvReport`、`VideoMediaInfo` / `TranscodeRequest`。
**每一个字段都要手工搬** ⇒ 门面结构体加了字段而这里没跟上，就是**静默丢字段**（与 P17-134 同族）。

★ 一个**有意的例外**（第 92-94 行）：
> 故意**不转** `id_text_a/id_text_b`：那是"明细里要看原文"的内部表……
> 传过去只会诱使插件自己拼一份明细，两处口径就会分叉。

### 2. 合并树句柄表（第 218-233 行）
`std::map<int, efs::MergeTree>*`（`new` 出去、永不析构）+ `next_tree_handle()` 从 1 递增。
`fs_tree_free` = `erase(handle)`；**插件忘了 free 就常驻内存**（注释写了"插件自己负责"）。

### 3. 逐组转发里有三处值得记的

**（a）`fs_loose_entries`（第 717-733 行）**：把引擎 `LooseInfo{truncated, truncated_reason}`
抄到门面的 `FsLooseInfo` —— 这是 P17-134 修复的落点（"信号在门面层丢掉"）。

**（b）`fs_package_entries`（第 847-868 行）**：`info` 出参抄 `DbReadInfo` 三项。
★★ 注释里有一条**判据脆弱性警告**（第 856-858 行）：
> 参数注释**不要**写成块注释（哪怕只是在行注释里引用它的字面量也不行）——
> `check_facade_order.py` 的标签正则会把它当成一个聚合初始化项
> （本批实测踩到两次：多出一项 `registry` ⇒ **整张门面表从那一项起全部错位**）。

⇒ 这是"**判据靠正则解析源码**"的代价：注释能弄坏判据。

**（c）`fs_pack_db`（第 932-980 行）**：给了 `&pack_err` ⇒ 启用 4 GiB 窄化两条检查；
命中就塞进 `out.error` 直接返回、**绝不 write**（"不落空包"）。
注释点名"这一处同时覆盖两条调用链：门面自己的 `fs_pack_db` 与 `py_host.cpp` 的 Python 绑定"。

### 4. ★ 读出来的一个真 bug（第 1047 行）

```cpp
if (!out.ok) out.error = "有 %d 条没装上（见 failures）" + std::to_string(out.failures.size());
```

C++ 里 `const char* + std::string` 合法，但结果是把数字**接在**那句字面量后面 ⇒ 错误文案变成
`有 %d 条没装上（见 failures）3` —— **`%d` 原样留在文案里**（没走 `printf`）。
对照 `pkg_restore`（第 1085-1087 行）就是对的（`"有 " + std::to_string(n) + " 条没还原"`）。

⇒ **严重度：低（仅文案）**，但这是"照抄时漏改"的典型 —— 记下来，将来一并修。

### 5. `pkg_install` 的备份顺序（第 1022-1037 行）
先 `copy_file(target → backup)`（**仅当目标已存在**），成功才继续落盘；
`out.overwritten.push_back(rel)`（覆盖必须可见）。
任一环失败 ⇒ 记入 `failures` 并 `continue`（不中断整批）。

### 6. `pkg_write_manifest`（第 981-1001 行）
原子写 + **写后回读逐字节比对**（`read_back != text` ⇒ 报错）。
⇒ 与 `core::build_version` 的"写后读回"同一纪律。

---

## C. `src/engine/plugin/` 8 个文件读完后的小结

| 文件 | 行数 | 一句话 |
|---|---|---|
| `widget_facade.cpp` | 470 | 两张名单（kMethodNames/has_impl）+ 线程白名单 + root/page 作用域 + trace ring |
| `py_host.cpp` | 4146 | CPython 宿主：GIL/config/home、插件加载与回滚、PyUi/PyEngine/PyHost、探针 |
| `registry.cpp` | 830 | 10 个注册点 + 五种排序口径 + 按 owner 回滚 + 解密闸门与 S-05 |
| `engine_facade.cpp` | 209 | 78 项方法表 + 默认表 + 两个 available\* 的语义差异 |
| `engine_facade_impl.cpp` | 1109 | 全转发 + 三套 to/from 转换 + 合并树句柄表 |
| `host_facade.cpp` | 87 | 5 项方法表 + 默认表 |
| `host_facade_impl.cpp` | 69 | 默认表的五条 lambda |
| `loader.cpp` | 23 | 唯一入口 → py_host |

★ **共性**：三张门面都是"名字表 + has_impl + available(整体) + has(单项)"四件套，
**任何一项手写遗漏都会长出 P16-0 那族 bug**（copy_text / host_run_process 都是这么来的）。

---

## D. 对本项目红点的最终判断（这条链已经读完）

从 `wrap_python_builder` → `PyUi::available` → `UiFacade` → 引擎，**链路已完整读过**：
1. builder 抛异常 ⇒ `add_error` + `core::log` + 占位页（**有痕迹**）；
2. builder 返回 None ⇒ **合法**（插件只往根里塞控件）⇒ `kNoWidget` ⇒ 占位页（**无痕迹**）；
3. builder 里 `ui.*` 全被拒 ⇒ **每条都打 WARN**（门面缺方法 / 非 UI 线程）；
4. builder 里 `ui.*` 成功但 parent 为 null ⇒ `resolve_parent` 落到 `current_panel_root()`；
   若那为 null ⇒ 控件建在 null 上（Qt 允许，但不在任何容器里）⇒ **页看起来是空的，且可能无 WARN**。

⇒ **第 4 条是唯一"既无异常也无 WARN"的空页路径**。
它正好对应我们 `门面树夹具页 anything=0 且无异常、且未见 WARN` 的形态。
⇒ 下一批该看 **`widget_facade_qt.cpp` 的 `resolve_parent`**（外壳侧）与 **Hub 传进来的 parent**。

## E. 待继续

`src/engine` 其余 33 个文件（`fs/`、`text/`、`font/`、`xml/`、`video/`、`patch/`、`process/`、`engine.cpp` 等）
→ 然后 `src/shell`（57 个，`hub.cpp`/`widget_facade_qt.cpp` 是重点）→ `tests` → `scripts`。
