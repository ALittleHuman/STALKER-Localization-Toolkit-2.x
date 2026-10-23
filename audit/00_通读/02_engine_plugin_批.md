# `engine/plugin/` 头文件批（registry / loader / py_host / facade_api / host_facade）

> 全文读完 5 个：`registry.h`(397)、`loader.h`(36)、`py_host.h`(133)、`facade_api.h`(68)、
> `host_facade.h`(77)。共 711 行。`engine_facade.h`(616) 下一轮读。

---

## 1. `registry.h`（397 行）

### 10 个注册点（第 226-253 行）
`decryptor / format / command / menu_item / option_entry / option / panel / tool(×2 重载) / compare_mode`
—— 注释说"10 个注册点 = 9 个 `api.register_*` + `register_compare_mode` 这个贡献点"（第 4-5 行）。

### 三条排序/匹配口径（第 9-17 行）
- **12.4 槽位精确匹配**：非法寻址**注册时**就拒 + 写 `registration_errors`（绝不静默丢弃）。
- **12.5 排序**：`order` 升序 + **注册顺序**稳定；`option_entries_for` 不排序、按注册顺序保序去重；
  `compare_modes_for` 同名时**内置优先**（与注册顺序无关）。
- **12.6 `when`**：None/空 dict 恒真；**可调用抛异常 ⇒ False**。

### ★ P17-97（第 37-44 行）：`when` 的值按**字符串**比较，类型不参与
修前 Python 侧用 `str()` ⇒ `1`/`True`/`"1"` 全变 `"1"`、互相冒充；现在用 `repr()`。
宿主喂进 context 的值本来就是字符串（如 `fs_page.cpp` 的 `context["loaded"]="1"/"0"`）。
⇒ **对判据的含义**：任何"when 没命中"的怀疑，先看插件写的是 `1` 还是 `"1"`。

### ★★ P17-98（第 46-59 行）：注册表的线程约定（这条是我们最需要的）
- **写（注册/注销/回滚）只在主线程**：全部来自 `Hub::load_plugins()` → `load_all_plugins_from_dir()`。
- **读可以在工作线程**：`menu_items_for()` / `panels_for()` / `invoke()` 属于"发布后只读"。
- **边界**：前提是"加载完成后没有并发写"；将来要在工作线程注册必须先加锁。
- 另一处并发点是**插件日志缓冲**（`api.log` 可在工作线程调）——**已加锁**，在
  `py_host.cpp` 的 `append_plugin_log_line`/`take_python_log_lines`，判据
  `tests/check_plugin_log_thread_safety.py`。

⇒ **对 P13/P16 的意义**：菜单相关注册只在主线程写，读是安全的 ⇒ **不是并发问题**。

### S1-2（第 181-220 行）：按 owner 记账 + 按 owner 回滚
- 缺陷：`init(&api)` 返回 0 时宿主只 `FreeLibrary` 不回滚 ⇒ 表里留着指向已 unmap 代码段的裸指针 ⇒ AV。
- 为什么按 owner 逐容器：`registration_count()` 是**总量**，"总量相等"可能是"撤错别家的"。
- `errors_` **不在**回滚范围（只存字符串、不可能悬垂），但必须**可见**（零静默）。
- `PluginResidue::registrations()` = 9 个容器之和（不含 errors_）；`describe()` 一行 `k=v` 供判据解析。

### 活动注册表钩子（第 332-395 行）——CLI 与 GUI 不许分叉
用户原话：「GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。」
- `set_active_registry` / `ActiveRegistryScope`（RAII，给 CLI 那种"局部注册表、多出口"的用）。
- ⚠ 存的是**指针**，注入方必须在注册表析构前复位。
- `gate_active_read()`：路径已在 `decrypt_output_dir()` 里就原样放行（否则二次解密）。
- S-05（第 362-388 行）：解密产物落点 `%TEMP%\xrkit_decrypted\<pid>-<随机token>\`，
  文件名是源路径 32 位哈希 + `.decrypted`（**不带原名**，防同机进程按包名猜）；
  `DecryptSessionScope` 可嵌套，最外层析构才真删（Windows 句柄原因）。

---

## 2. `loader.h`（36 行）——决策 1「C ABI 退干净」的落地

- C 插件路**整体删除**：没有 `stk_api`、没有 `stk_plugin_init`、没有 `LoadLibrary` 插件路径、
  没有 DLL 句柄表（第 4-7 行）。
- 保留**唯一**加载入口 `load_all_plugins_from_dir(dir, registry)`（第 34 行），
  理由是"让两边加载同一批东西、进同一份 Registry 变成结构性事实"。
- `LoadResult.registrations`：**失败时 = 本次已撤回的注册条数**（回滚几条也要看得见）。

---

## 3. `py_host.h`（133 行）

- **放在引擎层且 Qt-free**：宿主必须被 Hub 与 CLI 共用（第 8-11 行）。
- 三条可判红的口径（第 13-25 行）：零静默 / 降级不许装成正常（release 档见 `enabled=0` 即 FAIL，
  **不是 SKIP**，防"一键关掉锁"的后门）/ Python home 只有一份解析口径（显式参数 → exe 旁 `python\`
  → exe 旁 `python3XX.zip` → 构建期烘焙 → 空串）。
- **P17-106**（第 47-52 行）：`ever_initialized` 单独一位；`shutdown_python()` 之后 `initialized=false`
  看不出"曾经起过"⇒ 再 `init_python()` 会得到"半死宿主"。现在 `ever_initialized=true` 时**明确拒绝**。
- **C-04**（第 74-80 行）：`python_host_report()` 是**机器协议**（一行一条），而解释器错误文本常带换行
  ⇒ `py_host_one_line()` 把 `\r\n\t` 洗成空格，**只洗这一个字段**（路径里的空格不许动）。
- **P17-98 写侧**（第 111-115 行）：`append_plugin_log_line()` 带锁 + 200 行上限；
  作者注明"除了 `py_host.cpp` 自己，只有跨线程压力判据调它"。
- `run_python_host_probe`（第 121-131 行）：收在引擎里因为**发行包只有 `xrkit_app.exe`（没有 CLI）**，
  "包内自带的 Python 能不能起来"必须在**包内**判；返回 0/1/**2=构建期没带 Python（显式跳过）**/3=用法错误。
  ⇒ **判据里 rc=2 的含义在 py host 这条上也是"未覆盖"**，与本项目 `check_package_fixture_pages.py` 的 rc=2 同族。

---

## 4. `facade_api.h`（68 行）——`stk_ui` 16 方法

- 决策 1 第一刀：这张表原来在 `include/xrkit/plugin/abi.h` 里，随 C ABI 一起要删；但 `stk_ui`
  **不是 C 插件专属**（外壳 `widget_facade_qt.cpp` 实现它、py_host 与门面共用同一张表）
  ⇒ 先搬到独立头（第 5-11 行）。
- 16 方法 = 9 造控件 + 7 改控件/取输入；`parent`/返回值都是**不透明句柄**。
- 参数名与 1.x `TkUiFactory` 一致（第三方照那套写）。

---

## 5. `host_facade.h`（77 行）——宿主服务面（第三张表）

- 既不是控件面也不是引擎面，是"宿主替插件办的杂事"：`pref_get/pref_set`、`app_dir`、
  `system_font_dirs`、`run_process`（第 51-67 行）。
- **P12b**：`run_process` 的语义唯一一份在 `engine/process/run.cpp`
  （取消要真停、退出码如实报、stderr 尾巴可见、不许弹控制台）。
- **P16-0 的坑**（第 63-66 行）：字段名原叫 `host_run_process`，而插件可见名是 `run_process`
  ⇒ `host_facade_has("run_process")` **恒 false**，"能力看着在、实际拿不到"。
  **这是本仓一个典型 bug 族：内部字段名与对外登记名不一致 ⇒ 自查恒假。**

---

## 6. 由这批读出的、与当前红点直接相关的判断

1. **P13/P16 的菜单不是并发问题**：注册表在主线程写、工作线程只读（P17-98），菜单数据本身安全。
2. **P17-1② 的取消位**：`task_cancel_requested` 在白名单里（widget_facade.h:334-351），
   `TaskShell` 的取消位是**原子**的 ⇒ "worker 里读到"这条路是通的；偶发"没读到"要查**时机**。
3. **判据 rc=2 的语义在本仓有先例**：`run_python_host_probe` 明确把"构建期没带 Python"返回 2
   （显式跳过），与本项目"rc=2 = 未覆盖"一致 —— 这不是我发明的约定。
4. **一个新的 bug 族线索**（P16-0 同族）：凡是"内部字段名 ≠ 对外登记名"的地方，自查都会恒假。
   我加的 `menu_ready`/`page_ready` 钩子若名字与判据读取不一致，会出现同类假绿/假红 —— 需逐处核对。

## 7. 待核对

- `registry.h:22` 依赖 `engine/xml/compare.h`（下一批读）。
- `py_host.h:131` `run_python_host_probe` 的 `args` 约定（"去掉 exe 名的完整参数表"）需要在
  `src/shell/main.cpp` / `src/cli` 里核对调用方是否真的去掉了 exe 名。
- `host_facade.h:63-67` 的三处登记名（`kMethodNames` / `has_impl` / pybind `.def`）需与实现核对。
