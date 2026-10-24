# 第三方全量审计（Hana）核验记录

> 2026-10-24。规矩：**第三方结论只当线索，每条自己查证**。下表每条都给"我怎么验的"。
> 状态：`✅ 确认` / `🟡 部分` / `❌ 修正` / `🔧 本轮已修`。

## 一、九条问题的核验结果

| # | 它说的问题 | 我的核验（证据） | 状态 |
|---|---|---|---|
| **H1** | `--lzhuf` 输出路径按 ANSI 打开（`std::ofstream out_file(operands[2], …)`），中文/西里尔路径打不开 | `tools/cli/main.cpp:5301` 原文确实是 `std::ofstream out_file(operands[2], …)`；同文件 `--decode` 用 `path_from_utf8`、`patch.cpp:612` 也把这条写成过教训 | ✅ 🔧 已修（改 `path_from_utf8`） |
| **H2** | 两处 `.string()`（CP_ACP 窄化）：`overlay.cpp:23`、`log.cpp:95` | 实测 `log.cpp:95`/`:110`（**排序键**也用 `.string()`）＋ `overlay.cpp:23`；另有 `glyph.cpp:130`、`registry.cpp:730` 同类（未改，见 §四） | ✅ 🔧 已修（log 两处 + overlay 一处，改 `path_to_utf8`） |
| **H3** | 大小写折叠只折 ASCII（`path_equals`/`component_equal`/`artifact_key`/`dup_key`），NTFS 是 Unicode 折叠 ⇒ 俄文名下漏判"同一份产物" | 确认ascii 折叠属实；**但它漏说了一件有利事实**：仓里已有 `include/xrkit/engine/text/case_fold.h`（`case_fold_lower` / `path_lower`，表由 CPython 导出）⇒ 修法不是"造表"，是**复用** | 🟡 确认问题、修正修法（复用它） |
| **H4** | `pack_db(items, DbFormat::Unknown)` 无守卫 ⇒ `build_header` 落 `else` 静默按 11xx 写；而 `unpack_db` 明确拒 Unknown | `db.cpp` 的 `pack_db` 开头确实无 fmt 检查；`build_header` 的 `else` 分支写 11xx 形状（`{1, off, sz}`） | ✅ 🔧 已修（Unknown 直接拒并点名） |
| **H5** | 整文件读闸门只在 DB 一条路上；`convert_file`/`read_text_bytes`/`identify_file`/`theme::load_user_config` 无上限 | 确认多点无上限：`theme.cpp:303/329`、`xml_bytes.cpp:109`、`patch.cpp:621/1002`、`engine_facade_impl.cpp:953`（+ convert/sniff 同族） | ✅ 待修（需统一 `read_file_capped`） |
| **H6** | 非原子写还留几处：`extract.cpp::write_bytes`、`batch.cpp::write_stats_csv`、CLI `--decode` | **位置修正**：`write_bytes` 实际在 `src/engine/font/package.cpp:22`（不是 extract.cpp）；`batch.cpp:544 write_stats_csv` 确认；CLI 另有 `main.cpp:3434`（`--decode` 的 `ofstream(operands[2])`，**同 H1 的坑**）与 `:612/:647` | 🟡 部分（位置错、数量更多） |
| **H7** | `free_tree()` 零调用 + `Packer` 的 Node 树不释放（"看起来会释放"的死代码） | `git grep free_tree` ⇒ 只有定义 + 自递归，**零外部调用** | ✅ 🔧 已修（删函数，并把"树不释放"写成事实） |
| **H8** | JSON 解析器不校验尾随垃圾 | `build_version.cpp:45-50`：`parse()` = `skip_ws(); value(); skip_ws(); return true;` —— **没有 `i_ == s_.size()` 断言** | ✅ 待修（一行断言 + 反例） |
| **H9** | `write_file_atomic(target, bytes, /*overwrite=*/true, error)` 注释与实参不符（第三个是 `atomic`） | `theme.cpp:310` 原文确认；`atomic_write.h` 签名第三个是 `atomic` | ✅ 🔧 已修（注释改 `/*atomic=*/`） |

## 二、它自己"顺带复现"的台账条目

R2 / R25 / R5 / R12 / D1 / R8 —— 与我的台账一致，无异议（这几条我本来就在队列里）。

## 三、结构性与工具链的说法（我复核过）

| 说法 | 复核 |
|---|---|
| `smoke.cpp` 12k 行、`run_ci.py` 7.5k、`main.cpp` 5.3k（`run_cli` 2.6k 行） | ✅ 量级一致（smoke 现在 12,112 行、120 步/936 checks） |
| 没有 `/WX` | ✅ `git grep /WX` 零命中 ⇒ 告警可见不可拦 |
| 没有 sanitizer | ✅ cmake/preset 里零命中 |
| 没有 `.clang-format` | ✅ 文件不存在（有 `.editorconfig`） |
| `third_party/` 只有 3 个文件进 git | ✅ `git ls-files third_party` = 3（CMakeLists / SOURCES.md / sources.lock.json） |

## 四、我没照它做的地方（以及为什么）

1. **H3 不新建表**：直接用 `text/case_fold.h` 的 `path_lower`（它已经是从 CPython 导出的 Unicode 表）。
   但**有个前提要查**：fs 层 include text 层的头会破坏"引擎内部分层"吗？——需要一次方向性判断，
   我倾向"允许"（两者都是 engine 内部，且 `path_lower` 已经在 `text/extract.cpp` 之外暴露给判据用）。
2. **H6 不一次全改**：`main.cpp:3434`（`--decode`）与 `:612/:647` 是**同一类**（ANSI 路径 + 非原子），
   与 H1 同批修最省事；`batch.cpp` 的 stats csv 是"诊断产物"，原子性优先级低 —— 分开做。
3. **`glyph.cpp:130` / `registry.cpp:730` 的 `.string()`**：同类问题但不在它点名的两处，本轮**没动**
   （避免一次改太多、也避免我没读过的路径里引入行为变化）→ 进队列。
4. **它建议的"五件事"我排序不同**：`ASan/UBSan --san 档`排在**最后**（要新 CI 档 + 全部负向夹具重跑，
   成本远高于前四项；而前四项都是几十行级）。

## 五、我这一轮实际改了什么（都可回滚、逐条对应）

| 改动 | 文件 | 判据 |
|---|---|---|
| H1 `--lzhuf` 输出走 `path_from_utf8` | `tools/cli/main.cpp` | 现成判据 `cli-exit-codes` / `pack-db-api` 不覆盖；**新增判据**见下 |
| H2 两处 `.string()` → `path_to_utf8` | `src/core/log.cpp`（含 include）、`src/engine/fs/overlay.cpp` | 编译 + 现有字母序判据 |
| H4 `pack_db` 拒 `Unknown` | `src/engine/fs/db.cpp` | `check_pack_db_api.py` 仍绿（E1/E2/E3 走合法格式） |
| H7 删 `free_tree` | `src/engine/fs/squashfs_write.cpp` | 编译零引用 |
| H9 注释改 `/*atomic=*/` | `src/core/theme.cpp` | 纯注释 |

★ **H1 需要一条判据**（现在没有）：CLI 写盘路径的"非 ASCII 输出名"端到端。
  这条与 H6 的 `main.cpp:3434` 一起做更值（同一类）。→ 进队列 **H1+H6b**。

## 六、对"代码质量"的总评（我的版本）

同意它的方向和大致分数，但**扣分理由要重排**：

- 它把"一致性"扣到 7.5，理由是"同一把尺没贯彻"——我同意，而且**证据更硬**：
  `path_lower`（Unicode）与 `artifact_key`/`dup_key`（ASCII）**已经在同一个仓库里并存**，
  这不是"还没做"，是"做了两套"。这类问题比 bug 更贵，因为它会让判据给出**互相矛盾的绿**。
- 它给"工程化 7"、"无 sanitizer"，我同意但**排序不同**：`/WX` 比 sanitizer 便宜一个数量级，
  应当先上 `/WX`（先把现有 0 警告钉住），再谈 sanitizer。
- 它给"正确性 9"——我认为**偏高**：`run.cpp` 的 P1（259 退出码）、`feed_lines` 的 pending 无界、
  JSON 尾部不校验，都是"真实可触发"的正确性缺口，只是概率低。给 8.5 更合适。
- **它最值钱的一句**我完全同意：「凡是没有判据兜底的口径，注释迟早会说谎」——
  H3/H4 正是活例子（注释写得很硬、代码没跟上）。

---

## 七、第二版（Hana 的三条事实修正）核验

| 它说 | 我核到的原文 | 结论 |
|---|---|---|
| `main.cpp:3434` 是 `--extract` 不是 `--decode` | `:3415` 打印 `EXTRACT error reason=`、`:3429` 用 `operands[2]` 判已存在、`:3434` 直接 `std::ofstream out(operands[2], …)` | **它对，我上一版写错了** |
| `--decode` 的写盘在 `:5249` 且已用 `path_from_utf8` | `:5249 std::ofstream out(path_from_utf8(operands[1]), …)` | **它对** |
| `:612/:647` 是 smoke C-13 夹具、传 `cfs::path`、`:630 .string()` 只比 ASCII | `:612 std::ofstream out(f, …)`（`f` 是 `cfs::path`）、`:647 out(small / names[i], …)`、`:630` 只做 `rfind("log_",0)` | **它对** |

⇒ **H6 的剩余集合修正为**：`--extract`（功能性缺陷：中文落点整条动作失败）＋ `:3422-3424` 注释自己点名的那一串
（`--decl-fix`/`--resample-dump`/`--font-render`/`--sniff`/`--decode-text`/`--codec-lossy`/`--extract-chars`/`--pack-db`），
`--lzhuf` 已修、`--decode` 本来就对（但**要进 grid 防回归**）。
⇒ 严重度定位：**不是 P0 静默损坏，是"整条动作对中文落点不可用"**（会打 `EXTRACT error reason=无法写输出文件` 并 `kExitFail`）。
⇒ 判据写法随之改：断言"**非 ASCII 落点必须成功且字节一致**"，不是"必须报错"。

## 八、H8 落地（2026-10-24）✅

- `build_version.cpp` 的 `JParser::parse()` 末尾加 `i_ == s_.size()` 断言；报错点名**偏移**与尾巴前 24 字节。
- 只放行**空白尾随**（换行/空格是合法 JSON 结尾）—— 这一条很关键：不加就会把正常的 `version.json`（末尾有换行）判红。
- 新增门禁步 **`version-json-strict`（第 122 步）** + `tests/check_version_json_strict.py`：
  走**真入口** `--version-store`（不是只测函数）——
  ```
  ✓ A 合法 version.json（尾随换行属合法结尾）能读
  ✓ D1 自证：坏文件确实带 garbage
  ✓ B1 尾部 `garbage` ⇒ 非零退出 + 点名「尾随」「偏移」
  ✓ C1 解析失败时文件**逐字节不变**
  ✓ B2 两个对象连写 ⇒ 非零 + 点名尾随；C2 同样不写回
  ```
- `--fast` **PASS**。

## 九、下一步（按三方商定的序）

1. **H1b/H6b**（下一批）：`--extract` 修路径 + `:3422` 那一串 sweep 成 `core::write_file_atomic`（档位按体积选：
   单产物 `Streamed`，大产物别一刀切 `Readback`）＋ **静态形态判据**（CLI 里禁止
   `std::ofstream <ident>(operands[` 这种把 `std::string` 直接喂 ofstream 的形态）；
   grid = `{--lzhuf, --extract, --decode, --patch-apply} × {ASCII, CJK, 西里尔}`，断言**字节**不只 rc。
2. **H3**（再下一批，单独）：折叠原语**下沉到 core**（`text→fs` 已存在 ⇒ `fs→text` 是环），
   连带改生成器输出路径 + `case-fold-table` 门禁步路径 + `extract.cpp` 的 include；
   反例必须**造**：同一清单里 `Тест.txt` / `тест.txt` ⇒ 现 ASCII 折叠**漏判**（天然红，正好当修前证伪）；
   并记"行为变更"（duplicates 计数会上升、`--strict` 会拒以前能过的包）。

---

## 十、C0：写盘点 grid 判据（**先红后绿**的第一步，2026-10-24）

判据：`tests/check_cli_output_paths.py`（**尚未接进门禁** —— C0 的用途是"在父提交上先红"）。
它按**产物形态**取格（不按动作枚举），四类落点 × {ASCII, CJK 文件名, CJK 父目录, 西里尔}：

```
-- 形态 1：单产物 blob（--lzhuf encode → decode 往返）
  ✓ ×4 全绿（out=343 字节，解回原文）          ← H1 的修法**已被钉住**
-- 形态 2：包内单条目（--pack-db → --extract）
  ✓ extract × ASCII                            （哈希相等）
  ✗ extract × CJK 文件名：rc=0「EXTRACT ok」但**期望落点不存在**
  ✗ extract × CJK 父目录：rc≠0「无法写输出文件」
  ✗ extract × 西里尔：rc≠0「无法写输出文件」
```

### ★ 一条比"整条动作不可用"更严重的发现（修正 Hana 的严重度定位）

它把 `--extract` 定为"功能性缺陷：中文落点直接失败、用户看得见"。
实测**不全是**：**CJK 文件名那一格 rc=0 且打印 `EXTRACT ok`，但文件根本不在给定路径上**
（ANSI 转换把名字变成另一个可用名，于是**静默写到别处**）。父目录是 CJK 时才打不开、才报错。

⇒ 严重度应分两档：
- **CJK 文件名 ⇒ 静默错位**（rc=0 + 报成功，产物在"乱码名"下）；比"打不开"危险，因为用户不会去找；
- **CJK/西里尔父目录 ⇒ 明确失败**（rc≠0 + 理由）。
两档都由同一句 `std::ofstream out(operands[2], …)` 造成，修法相同（`path_from_utf8` + 原子写），
但**判据必须断言"期望落点存在 + 字节相等"**，只断言 rc 会把第一档当成绿。

### 同时核实的三个坑（都成立）

1. **`write_file_atomic` 会自动建父目录**（`atomic_write.cpp:124-131`）⇒ 换成它之后
   "输出到不存在目录 ⇒ 失败"会变成功。C1/C2 的 helper 必须**先判父目录**，保持旧语义。
2. **`:5131` 的锚点注释会说谎**（"本文件里 `ofstream out(path_from_utf8(operands[1])…)` 有两处"）——
   sweep 之后就是假话；必须同步改。
3. **`check_cli_exit_codes.py` 用行距启发式**（`LOOKBACK` 往前找"同层或更外层"的最近一条文案）——
   sweep 会把行号全挪掉 ⇒ 那条门禁会假绿/假红。**这一批必须把它的锚点从"行距"改成"动作名 + token"**。

### 11 处写盘点（核实与它给的表一致）

```
3434 --extract        裸 std::string   ← 唯一非 path_from_utf8
3749 --codec-lossy    path_from_utf8
3824 --decl-fix       path_from_utf8
4326 --pack-db        path_from_utf8   ← 它列的"我没列的两处"之一
4550 --font-render dds / 4566 ini      ← ini 那处也是它补的
4648 --extract-chars  4985 --resample-dump  5142 --sniff  5249 --decode-text  5305 --lzhuf(已修)
```
全部 `std::ios::trunc` 直写 ⇒ **11 处都不原子**。而 `--pack-sqfs`（`:4356` 一带）走 `pack_squashfs` 的 `.part`、
GUI `fs_page.cpp:2380` 走 `write_file_atomic` ⇒ **同一条不变量在 CI 真跑的那条路上有洞**（提交信息用这句）。

---

## 十一、C0.5（换尺）与"静默错位"的实测细节（2026-10-24）

### 11.1 C0.5：`check_cli_exit_codes` 的锚点从"行距"改成"动作块"

- 修前：`find_message()` 用 `LOOKBACK` 往前数行找"最近一条文案" ⇒ 一次机械替换（每处缩短 4~5 行）
  就会把行距全挪掉，门禁会因"行号变了"假红/假绿。
- 修后：`action_blocks()` 把 `main.cpp` 按 `if (action == "--xxx")` 切块，`find_message()` **只在本块内**找；
  块边界不随行数漂移。`action.empty()` 那一族不在任何动作块里 ⇒ 保留有限回退（那段没有 sweep 风险）。
- 实测：`CLI-EXIT-CODES ok 11 项`，三族计数仍 **49 / 10 / 1**（与冻结值一致 ⇒ 换尺没改变判定）。

### 11.2 写盘点 grid 加了 `--record/--compare`（C1 的验收判据）

每格记 **五元组**（rc / 落点是否存在 / 产物 sha256 / 输出首行），C0 基线已落盘
（`build/logs/_cli_out_baseline_c0.json`，8 格，含 3 个**已知红**格 —— 红也是事实，要一起记）。
⇒ C1 的通过条件不是"grid 全绿"（本来就有 3 格红），而是 **与基线逐格 diff 为空**。

### 11.3 ★ "CJK 输出名"的真实机制（实测，不是推断）

```
--extract <pkg> a.bin "<tmp>/中文产物.bin" xdb   →  EXTRACT ok path=a.bin bytes=16
目录里实际出现：涓枃浜х墿.bin   （UTF-8 被当 CP936 解释后的乱码名，distinct）
--extract ... "<tmp>/中文甲.bin"                 →  EXTRACT error reason=无法写输出文件
```
⇒ 机制：`std::ofstream(std::string)` 走 **CP_ACP + best-fit**；
**能被 best-fit 映射的**（`中文产物`）⇒ **rc=0、报成功、文件落在乱码名下**（静默错位）；
**映射不了的**（`中文甲`/`西里尔`）⇒ 硬失败。
⇒ 三档严重度：静默错位 > 硬失败；都源于同一句 `std::ofstream out(operands[2], …)`。

**Hana 的"best-fit 会把两个 CJK 名映射到同一个 ASCII 名 ⇒ 互相覆盖"这条，我没复现出来**：
三个测试名（`中文产物` / `中文产物A` / `中文产物B`）落成**三个不同的乱码名**，互不覆盖。
⇒ 结论只能是"**错位**已证实、**覆盖**未证实"；按规矩不能当结论用，我在队列里标成"待复现"。

### 11.4 C1 开工前的两条纪律（采纳）

1. **helper 只返回状态、不打印、不建父目录、不改流**（11 处文案/流/路径来源四个轴都不同，
   helper 一旦打印就会把差异抹平 ⇒ 那就不是"机械替换"了）；
2. **是 11 处不是 10 处**：`--lzhuf`（`5305`）路径虽已修，写仍不原子，也必须换 ——
   否则 C2 的静态判据（`run_cli` 区间内 `std::ofstream` 计数 == 0）会红。
