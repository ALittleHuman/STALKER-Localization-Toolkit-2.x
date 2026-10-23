# src/core 批二：`check.cpp`(52) ＋ `version.cpp`(43) ＋ `theme.cpp`(441)

---

## A. `check.cpp`（52 行）—— 自检运行器

- `run_registry` 逐条跑，**每条独立捕获异常**（`std::exception` 与 `...` 都记成一条失败，
  文案"抛出异常 <what>"）⇒ 一个检查崩了不影响后面的。
- `-v` 时逐条打 `  PASS ` / `  FAIL `（纯 ASCII）；最后**总是**打
  `SELFTEST pass=<n> fail=<m>`，返回 `fail==0 ? 0 : 1`。
- 失败明细在收尾前逐行 `FAIL <name>: <detail>` 打出来。

⇒ 形态与 `--smoke` 的 `SHELL-SMOKE ok checks=935 failed=0` 一致（都是 ASCII 收尾行）。
  **但它只证明"跑了 N 条"，不证明覆盖了什么**（见 `engine.h` 的"自报常量"教训）。

---

## B. `version.cpp`（43 行）

- `version_string()` = `主.次.补` + （预发布非空时）` + " " + kPrerelease`；
- `version_with_build()` = `主.次.补+<kBuildCount>`；
- `version_string_is_well_formed`：三段**非空数字串**，段间 `.`，末尾可选 ` <标记>`（标记非空）。

⇒ 手写解析，**不引正则** —— 与"core 层不依赖外部"一致。

---

## C. `theme.cpp`（441 行）

### 1. ★ 2.x 的配色覆盖是**有意的分歧**（第 31-58 行）
`kOverrides[]` 里 18 个角色硬编码 2.x 配色，理由写明：
"配色属**表现层**，与行为口径无关；改这里**不动** 1.x 源、也**不手改**生成的
`theme_tables.inc`（手改会让 `gen_theme_tables.py --check` 变红）"。

⇒ **"生成物只许由生成器改"** 这条纪律，是"门禁能锁住"的前提。

### 2. ★★ `app_dir()` 的两次翻车（第 224-249 行）—— P17-16

修前：`wchar_t buffer[MAX_PATH]` + 一次 `GetModuleFileNameW(nullptr, buffer, MAX_PATH)`，
再用 `n < MAX_PATH` 判成功。**路径长于 260 时该函数静默截断并返回缓冲大小（不是失败码）**
⇒ 判断不成立 ⇒ `return "."`：**app 目录变成当前工作目录**（日志/`user.ltx`/插件目录全找错地方，
且没有任何提示）。

第一次改法（`GetModuleFileNameW(nullptr, nullptr, 0)` 问所需长度）**在这台机器上返回 0**
⇒ 直接掉进 `return "."`，**被 `--smoke` 当场抓住**（"默认落盘目录…实得 ./logs"、
"user.ltx 与 exe 同目录"两条同时红）。

最终：**逐次翻倍重试**（返回 0、或"返回长度 == 缓冲大小"都当可能被截断 ⇒ 缓冲翻倍再来；
上限 32768），只有 `n < size` 才算拿到完整路径。

⇒ **这条是本仓"判据抓住产品 bug"的样板**：如果没有那两条 `--smoke` 检查，
这个 bug 会在长路径机器上静默存在。**也说明为什么"默认落盘目录"值得有判据。**

### 3. ★★ H-02：设置文件名 **`user.ltx` → `xrkit_user.ltx`**（第 259-273 行）

修前设置写在 `app_dir()/user.ltx` —— 与**游戏自己的** `user.ltx` 同名。
工具放进游戏根目录时，`save_user_values()` 会把整份文件重新渲染一遍
（键还在，但**注释与空行全丢**）⇒ **对游戏配置就是破坏**，且用户不会立刻发现。

现在：设置写 `xrkit_user.ltx`；旧 `user.ltx` **只在硬条件下**读一次做迁移：
新文件不存在 ＋ 旧文件存在 ＋ 旧文件里有**我们写过的键**（`[theme] mode|scale` /
`[ffmpeg] path|probe`）—— 否则**一个字节都不碰**。
迁移 = 旧内容**原子写**到新路径，然后只读新文件；**旧文件不删、不改、不再写**。

★ 而且迁移是"每进程一次"（`static const int kMigrated = migrate_legacy_user_config();`，
第 325 行）—— 第一次读设置时触发。

⇒ **"与外部文件同名"是最危险的一类命名**。我们的 `page_trace.txt`、`xrkit_smoke_*` 目录
  也在 `%TEMP%` 里，但都带前缀，是安全的；`user.ltx` 那次的教训是**不要用别人的名字**。

### 4. ★★ `load_ui_scale` 的 `strtod("1e400")` = **+∞ ⇒ double→int 越界（UB）**（第 398-416 行）

修前只挡了"非正数"，而 `strtod("1e400")` 返回 **+∞** 且 `*end == '\0'` ⇒ 一路通过 ⇒
`set_ui_scale(∞)` ⇒ `px()` 里 `static_cast<int>(nearbyint(value * ∞))` 是 **UB**；
`shell/theme.cpp:568` 的 `(基准+2.0)*ui_scale()` 也把字号算成 ∞。

现在：`!std::isfinite(value) || value > 4.0` ⇒ 按垃圾值回落。
上限 4.0 的理由写明："正常人不可能的倍率（DPI 档位封顶也在个位数）"。

⇒ **"能解析成数字"不等于"数值可用"** —— 必须再查 `isfinite` 与上界。
  这条对任何"用户可编辑的数值配置"都适用。

### 5. `save_user_values` 走原子写（第 367-377 行）
修前裸 `ofstream(trunc)` ⇒ 写一半失败就留下**半截 `user.ltx`**，而它是"下次启动继承主题与
界面大小"的唯一载体（半截 ⇒ 偏好静默丢失）。
现在走 `write_file_atomic`（`.part` → 自校验 → rename）。
★ 并注明"**丢注释/空行是既定口径**（与 1.x 一致，审计里也写明'别改'）——这里只换写法"。

⇒ 又一例"**照抄有意的缺陷，但把不安全的部分修掉**"。判据要能分清这两类。

### 6. `render_ltx` / `parse_ltx` 的形态（第 159-222 行）
- `parse_ltx` 保留**段顺序**与**段内键值顺序**；同名键**后者覆盖前者**（就地替换，不新增）；
- 忽略 `;`/`#` 注释行与空行；
- `render_ltx` 段间空行、段内每键一行（`key = value`）。

⇒ 这两条决定了"读→改一项→整体写回、保留其它段"能成立；**但注释/空行会丢**（既定口径）。

### 7. `px()` 用 `nearbyint`（第 151-155 行）
1.x `px()`：`int(round(value * _UI_SCALE))`，**四舍六入五成双**（银行家舍入），
`std::nearbyint` 的默认舍入模式正是它。

⇒ **"看起来一样的取整"在 .5 上会差 1** —— 判据对拍像素时要留意。

---

## D. 这批给"红点"的可执行结论

1. **判据真能抓产品 bug**（C.2 的 `app_dir` 翻车）：我们该保留并强化"默认落盘目录"这类
   基础断言 —— 它们比"菜单项数"更值得进 BETA 门槛。
2. **数值配置要查 `isfinite` + 上界**（C.4）：任何用户可编辑的数字都该这样。
3. **不要用别人的文件名**（C.3）。
4. **"能解析" ≠ "可用"**、**"跑了 N 条" ≠ "覆盖了什么"**（A 节末）。

## E. 待核对

- `src/core` 只剩 `build_version.cpp`(513) 与 `theme_tables.inc`(36)（下一批）。
- `shell/theme.cpp:568` 的 `(基准+2.0)*ui_scale()` 需在读 shell 时核（是否也做了 isfinite 保护）。
