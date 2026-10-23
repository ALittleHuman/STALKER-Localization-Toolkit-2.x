# src/engine/plugin 批二：`registry.cpp`(830) ＋ `loader.cpp`(23) ＋ `host_facade.cpp`(87) ＋ `host_facade_impl.cpp`(69)

---

## A. `registry.cpp`（830 行）

### 1. 槽位表**冻结在源码里**（第 81-118 行）
`kHosts`（fs/convert/text/xml/video/font）＋ `kLocations`（toolbar/context）。
来由：这两张表原来在 `contract_tables.inc`（由 1.x AST 生成、门禁逐字节锁），
C ABI 退净把 `contract.*` 删了，但 **host/location 合法性校验必须留着**
—— 它是"插件注册到不存在的槽位"这条拒绝的唯一依据。

⇒ 这是"生成物退役后**手工冻结**进源码"的一次操作，注释写清了"内容与生成物逐条一致"。

### 2. `when_ok`（第 57-77 行）
- **缺键**时 `want` 若为 `nullopt` 照样命中（对应 Python `ctx.get(k) == None`）；
- `predicate` 抛异常 ⇒ false（12.6）。

### 3. 每个注册点的统一形态
先 `when.invalid()` 检查（P17 的"参数非法必须拒绝 + 留痕"）、再 `bad_slot`、
再字段校验、最后 push。**任何一条不过 ⇒ `reject()`**（`errors_.push_back` + `core::log(Error)`）。

### 4. ★ `register_tool` 的两条拒绝（第 297-310 行）
- `replaces == name`（自替换）⇒ 拒绝：**"自替换＝什么都没换"**，而且会让
  "可见页 = 全部 − 被 replaces 命中的"把这一页也摘掉；
- 别人已经覆盖了同一页 ⇒ 拒绝并说出是谁（`other.plugin` / `other.name`）。

### 5. 四条消费端的排序语义**刻意不同**（第 330-418 行）
| 函数 | 排序 |
|---|---|
| `menu_items_for` | order 升序 + 注册顺序（`{order, i}` 双键 stable_sort） |
| `options_for` | order 升序 + 注册顺序 |
| `option_entries_for` | **不排序**，按注册顺序**保序去重** |
| `compare_modes_for` | 同名**内置优先**（与注册顺序无关），其余 order 升序 + 注册顺序 |
| `compare_mode(name)` | **线性取第一条**（不去重、不排序，1.x 同语义） |

⇒ **同名事实在五个函数里有五种排序口径** —— 判据写错一个就会"看起来没排序"。

### 6. `rollback_plugin`（第 527-568 行）
- 逐容器 `remove_if(item.plugin == plugin)`；
- `commands_` 是 map，按 `CommandReg::plugin` 逐条 erase；
- **不碰 `errors_`**（留痕必须看得见）；
- **不按"调用前的总数"截断**（`registration_count()` 是总量不是归属）。

### 7. `gate_db_path`（第 593-629 行）
- 没解密器 ⇒ 原样返回（"一切照旧"）；
- 命中却解不开 ⇒ **明确失败**，绝不回落成"当普通包读"（那会读出垃圾还判成功）；
- 落盘走 `write_file_atomic`；成功后 `register_decrypt_artifact(target)`。

### 8. ★★ S-05：解密产物的落点与命名（第 662-760 行）
修前 `%TEMP%\xrkit_decrypted\<原文件名>.decrypted`：
**名字可预测**（同机任何进程能按包名猜到并读取）且**永不清理**。
现在：
- 落点 `%TEMP%\xrkit_decrypted\<pid>-<每次运行随机 token>\`（不可预测、按进程隔离）；
- 文件名 = 源路径 **FNV-1a 32 位哈希** + `.decrypted`（不泄露包内容清单）；
- `atexit` 删整棵运行目录；启动时 `sweep_stale_decrypt_dirs()` 只清"PID 已不存在"的旧目录；
- `pid_alive` 用 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`；**打不开就当已退出**
  （注释如实写了"权限不足的活进程会被误判"）。

### 9. `path_inside_dir`（第 643-654 行）
逐级 `parent_path` 往上走，用 `core::path_equals` 比。**只做词法比较**（不碰盘）。
⇒ 闸门用它认出"这是我自己的产物"（避免二次解密）。

★ 注意它依赖 `core::path_equals` —— 而那个函数**只折 ASCII 大小写**（见 `12_src_core_批一.md`）。
  ⇒ 如果 TEMP 路径里有非 ASCII 且大小写不同，判定会 fail-安全（不认作产物 ⇒ 二次解密）。
  实战里 TEMP 路径一般是 ASCII，风险低。

### 10. 操作级清理（第 764-797 行）
`thread_local session_depth` + `thread_local session_artifacts`；
`DecryptSessionScope` 嵌套，**最外层析构才真删**（Windows 句柄原因）。

### 11. 模块探针（第 823-829 行）
`extern "C" const char* xrkit_engine_probe_plugin_tag()` + `probe_plugin()`
—— 与 `engine.h` 的"模块计数来自真链上的探针"配套：**摘掉模块 ⇒ 链接失败**，而不是少报一个数。

---

## B. `loader.cpp`（23 行）

只有一行实质代码：`load_all_plugins_from_dir` → `load_python_plugins_from_dir`。
注释记着 C ABI 退净删掉的东西（`is_plugin_dll_name` / `load_plugin` / `unload_all` / `loaded_count`）。

⇒ "两个前端唯一的那一个入口"（C-01/C-03 的静态锁仍锁在这个函数上）。

---

## C. `host_facade.cpp`（87 行）

- 与 `widget_facade.cpp` 同构：`kMethodNames[]`（5 项：`pref_get/pref_set/app_dir/
  system_font_dirs/run_process`）＋ `has_impl` 逐项判定；
- ★ **默认表**：`host_facade()` 在"没注入"时返回 `builtin_host_facade()`
  —— 所以 CLI 进程也有 `api.host.*`（与 `widget_facade` 不同：那边没注入就是 null）。
- `host_facade_available()` 同样是"全有或全无"。

⇒ **两张门面的注入语义不同**：控件门面**必须**外壳注入；宿主门面**有引擎默认表**。
  判据若把两者当成同一种，会得出错误结论。

---

## D. `host_facade_impl.cpp`（69 行）

`builtin_host_facade()` 的五条 lambda（函数指针数组式初始化）：
`load_user_value` / `save_user_value` / `core::app_dir` / `efont::system_font_dirs` /
`eprocess::run`（逐字段搬运 `RunOptions`↔`ProcessRunOptions`）。

★ 注释写明住处为什么在**引擎**层：这四件只依赖引擎与 OS ⇒ CLI 与 GUI 都能用
（"把只依赖引擎的东西放外壳，CLI 那个宿主就用不到" —— P7a 的教训）。

---

## E. 这批的结论

1. **`option_entries_for` 不排序**（第 365-377 行）：判据若假设"插件下拉项按 order 排"，
   会与实现不符 —— 它的语义是"保序去重"。
2. **`compare_mode(name)` 线性取第一条**：同名时**先注册的赢**（与 `compare_modes_for` 的
   "内置优先"不同！）。这两个入口对同一个名字可能给出**不同**的 mode。
   ⇒ 真要是"同名插件模式"的场景，这里会分叉 —— 值得记一笔（可能是潜在不一致）。
3. **`path_inside_dir` 依赖只折 ASCII 的 `path_equals`**（A.9）。
4. **两张门面的默认表语义不同**（C 节末）。

## F. 待继续

`src/engine/plugin/` 还剩 `engine_facade.cpp`(184) 与 `engine_facade_impl.cpp`(1060)。
