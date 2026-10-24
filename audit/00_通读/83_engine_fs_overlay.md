# 通读台账 06：`src/engine/fs/overlay.cpp`（**665 行**）

> 2026-10-24。只记录：问题 / 调用关系 / 风险 / 证据。**不重构**。BETA.2 保持可比。
> 六个重点按用户给的组织；§七 给出 **R10/R20 hotfix 的具体落点**（已获准先修）。

## 一、职责（三块）

| 块 | 函数 | 说明 |
|---|---|---|
| **合并树**（多源叠加） | `build_tree` / `MergeTree::merge` / `find` / `toggle_*` / `checked_paths` | 后源覆盖前源；勾选态跟随旧节点 |
| **收集**（磁盘 → 条目） | `collect_tree`（编码转换页用）/ `collect_pack_files`（封包用）/ `loose_entries`（散装层用） | 三套各有各的上限/报错口径 |
| **格式表** | `fs_formats` / `format_key_to_db` / `format_key_is_sqfs` / `is_package_file_name` | 只认内部键；`.xdb` 家族已覆盖 |

## 二、重点 1：walk-one 语义（重复项 / 大小写 / 文件与目录同名 / 覆盖优先级）

| 场景 | 行为 | 证据 |
|---|---|---|
| 同一源内**同路径重复** | **后者为准**（文件覆盖、大小更新）；文件→目录或目录→文件则清空子树 | `:113-126` |
| 跨源同路径 | 后者为准；`++conflicts_`；**勾选态跟随旧节点** | `:134-173` |
| 类型不同（一边文件一边目录） | **整棵子树替换**（不递归合并） | `:147-150` |
| **大小写碰撞** | `child_by_name` 是 `child.name == name` —— **大小写敏感** ⇒ `Textures/A.dds` 与 `textures/a.dds` 成为**两个节点** | `:45-50` ← **R26** |
| 排序 | **文件夹在前、各自按名字**（用户 2026-09-30 口径） | `:52-61` |

**R26（中）**：合并树判"同名"用大小写敏感比较，而 `extract_tree` 的 `component_equal`/`ArtifactTally`、
以及本文件的 `sort_sources_by_basename`（`basename_lower`）都是**大小写不敏感** ⇒ **同一项目两把尺**。
在 Windows 上落盘时两条会互相覆盖，而树上显示成两份 ⇒ 用户看到的与盘上的不一致。

**R34（低）**：`build_tree` 内部的重复条目**不记任何账**（后者为准，静默）⇒ 与 db 的 `R14` 同族。

## 三、重点 2：路径归一化（**与 `safe_out_path` 不是同一把尺**）

| 形态 | overlay 层 | `extract_tree::safe_out_path` |
|---|---|---|
| `..` | `split_path` **保留** `..` 成节点 | **拒** |
| 尾点/空格（`a. `） | **不剥**（节点名就是 `a. `） | **剥成 `a`** |
| 保留名（CON/NUL） | **不查** | 不查（只由 `write_file_atomic` 查叶子） |
| 空段/`.` | `split_path` 丢空段；`.` 保留成节点 | 丢空段与 `.` |
| 长路径 | 不处理（`recursive_directory_iterator` 失败会进 `report->errors`） | 不处理（>MAX_PATH 可见失败） |
| 冒号/ADS | 不查 | **拒** |

**R27（中）**：**判定"允许不允许"的两处用不同尺子** —— 树里能勾到的名字，落盘时可能被**改名**（尾点/空格）
或**拒绝**（`..`/ADS）。改名那条最阴：用户勾了 `a. `，盘上得到 `a`（覆盖率/清单对不上）。
★ 建议：把 `strip_trailing_dots_spaces` 与"保留名/冒号"的判定**提到一个共享函数**，overlay 只用它做**显示归一**，
extract 用它做**落盘判定** —— 一把尺，两处调用。

## 四、重点 3：权限失败 / 跳过目录 / 符号链接 / 不可读文件 —— 报错、跳过、还是静默

| 入口 | 根不可读 | 遍历中断 | 单个文件不可读 | 链接 |
|---|---|---|---|---|
| `collect_tree` | ✔ `report->errors`（零静默） | ✔ `errors` + `break` | 静默跳过（`is_regular_file(ec)` 假） | 报目录但不递归 ⇒ 树里出现"空目录"（**R31**，信息） |
| `loose_entries` | ✔ `error` 文案 | **`break` 静默**（**R29**） | 大小取 0（`ec ? 0`） | ✔ 报成条目、**绝不递归**，深度 32 / 20 万条上限 + **结构化 `truncated` 理由**（P17-110/120，做得好） |
| `collect_pack_files` | **完全静默**（`is_directory`/`is_regular_file` 都假 ⇒ 无 item、无报数）← **R28** | **`if (ec) break;` 静默**（R29） | **静默跳过**（R30） | 不跟随（`recursive_directory_iterator` 默认），链接本身也不是 regular file ⇒ **静默丢** |

**R28（中）**：`collect_pack_files` 对"根不存在/不可读"**零账** —— 同一文件里 `collect_tree` 与 `loose_entries`
都做了零静默，唯独封包收集这条没做。用户看到的是"0 个文件可封包"，而真实原因是路径/权限。

## 五、重点 4：总量 / 候选收集 —— **R8 的答案：上限就该在收集阶段**

- `loose_entries`：20 万条 + 深度 32 + 截断理由 ✔（唯一的"有上限"的收集）。
- `collect_pack_files`：**没有任何条数/字节上限**，把全树的 `rel` + 绝对路径都放进 `items`；
  而 2 GiB 的闸门在 `pack_db` / `pack_squashfs`（**之后**）才判 ⇒ **先吃满内存，再被拒**。
  **R32（中）**。
- `collect_tree`：无上限（编码页用它，条目数通常不大）。

⇒ **建议（并入 R8）**：收集阶段就累计 **条数 + 字节**，超限**立即失败并给出理由**；
把"总量预算"做成一张表（db 3× / sqfs 写 / sqfs 读累计 / **落盘累计** / **收集累计**）。

## 六、重点 5：overlay 的删除/替换顺序 —— 是否可见、是否记账

- 替换：后者为准 ✔；`conflicts_` 只在"同路径节点已存在"时 ++（类型变化整树替换也算 1 次）。
- **`conflict_count()` 有出口**：`overlay.h:180` → `engine_facade_impl.cpp:815` → py_host `fs_tree_conflict_count`
  ⇒ **不是死字段**（**R33 降为信息**：界面是否显示未核，CLI 是否打印未核）。
- `build_tree` 内部重复**不记账**（R34）。
- 勾选态：`copy.checked = cur->checked` ✔（6.1 口径：重建树不丢用户勾选）。

## 七、重点 6：`R10/R20` hotfix 的具体落点（已获准先修，**本页只列不改**）

| # | 文件 | 改什么 | 判据 |
|---|---|---|---|
| 1 | `src/engine/fs/squashfs.cpp` | 目录项名字校验：**拒** `\0`、控制字符（<0x20）、名字长度 >255（单段惯例；与写侧 65536 上限并存，取**更严**并写清理由） | `list` 遇坏名 ⇒ 明确报错（不是静默进 `out`） |
| 2 | `src/engine/fs/extract_tree.cpp` | `safe_out_path` 每段：拒 `\0`/控制字符；**保留名**（CON/PRN/AUX/NUL/COM1-9/LPT1-9，含带扩展名形态）；**目录段也查**（不只叶子） | 新增 grid：`CON/x.txt`、`a\0b`、`a\x01b` 都必须拒，理由点名 |
| 3 | `src/engine/fs/squashfs.cpp`（同批） | slink 类型：遇到 `kTypeSlink/kTypeSlinkX` ⇒ 报 **unsupported entry type**（不再当普通文件读） | `list` 或 `read_entry_file` 给出明确"不支持的类型"；现有 `R15` 收口 |

★ 三处都只做"拒绝 + 明确理由"，**不改任何合法路径的行为**；判据照"零静默"口径写（拒因点名）。
★ 建议把 1、2 的"名字合法性"抽成**一个函数**（`fs/name_check.h`），供 parser / extract / 未来 pack 共用（R27 的同一把尺）。

## 八、其余发现

| # | 位置 | 现象 | 严重度 |
|---|---|---|---|
| R31 | `collect_tree` | 指向目录的链接：报 `is_dir=true` 但**不递归** ⇒ 树上显示"空目录"（与 `loose_entries` 的"报成条目"口径不同） | 信息 |
| R35 | `depth_of_rel` | 只数 `/` —— **已核对不是 bug**：`core::path_to_utf8` 用 `generic_wstring()`，恒为 `/` | 核对无误 |
| R36 | `collect_tree` | 每条目一次 `stdfs::relative()`（O(N) syscall）；大目录慢 | 信息 |
| R37 | `is_package_file_name` | `basename_lower` 大小写不敏感 ✔（与 `MergeTree` 不一致，佐证 R26） | 信息 |
| R38 | `collect_tree` | 排序用 `path` 字节序 ⇒ 目录与文件混排（与 `MergeTree` 的"文件夹在前"不同） | 低（两处显示口径不同） |

## 九、队列增量（并入总表）

- **新增**：R26（大小写两把尺）、R27（overlay vs extract 归一化两把尺）、R28（封包收集零静默）、
  R29（遍历中断静默）、R30（不可读文件静默）、R32（收集无上限）、R38（排序口径）
- **降级/更正**：R33（conflict_count 有出口，信息级）、R35（核对无误，撤销候选）
- **hotfix 落点**：见 §七（R10/R20/R15-拒绝）

> 下一份：`src/engine/xml/dirdiff.cpp`（`dir_diff` 在这里；与本节 `collect_tree` 的"排序/去重/大小写"直接对照）。
