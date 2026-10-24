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
