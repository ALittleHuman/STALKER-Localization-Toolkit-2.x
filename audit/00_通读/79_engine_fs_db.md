# 通读台账 02：`src/engine/fs/db.cpp`（**1154 行**，比清单里的 966 行多）

> 2026-10-24。只记录：问题 / 调用关系 / 风险 / 证据。**不重构**。BETA.2 保持可比。
> 本页按用户给的六个检查点组织；结尾是"其余发现"与"判据覆盖现状"。

## 一、职责与结构

六种 DB 格式（`xdb` / `2947ru` / `2947ww` / `2945` / `2215` / `11xx`）的**读、写、自洽判定、seek 式大包解析**。
分成两半：

| 半边 | 函数 | 备注 |
|---|---|---|
| **读**（内存） | `unpack_db` / `auto_detect` / `extract_entry` | 整包在内存里；`read_chunks` 逐字节回退找块头 |
| **读**（seek） | `list_db_seek` / `extract_entry_seek` | 只读块头、按 `sz` 跳 DATA 块；HEADER 块读进来（≤512 MiB 上限） |
| **写** | `pack_db` / `build_header` | 先建 header 再拼 DATA；4 GiB 窄化闸门 |
| **纯工具** | `Scrambler`（2947 系加扰）/ cp1251↔UTF-8 / lzhuf/lzo 解码 | 全内存、无临时文件 |

## 二、检查点 1：删掉 `entries_fit_file` 后"自洽"是否仍判全

调用点（四个，全部走 `entries_self_consistent*`）：
```
:426  read_chunks（内存版认 HEADER）
:609  auto_detect（六格式各试一次，唯一命中才认）
:756  list_db_seek（seek 版认 header）
:788  list_db_seek（**显式选定格式**时对 seek header 再试一次，P17-127）
```
`entries_fit_size`（:308）现在检查什么：

| 维度 | 判了吗 | 证据 |
|---|---|---|
| 目录项 | 检查但 **`is_dir` 一律跳过** | :310 `if (e.is_dir) continue;` |
| 路径非空 | ✔ | :311 |
| 控制字符 | ✔（`< 32`） | :312-314 |
| 偏移非 0 | ✔ | :316 |
| 大小不越文件尾 | ✔ `offset + size_comp <= file_size` | :316 |
| 压缩比 | ✔ `size_real <= size_comp * 1024` | :317 |
| 条目数上限 | ✘（隐含受 header 字节数限制） | — |
| **`offset` 是否落在 DATA 块内 / ≥8** | **✘ 完全不查** | 只与 `file_size` 比 |
| **`size_real` 是否超过解压上限** | **✘ 不在自洽阶段**（在 `decode_entry_bytes` :647 才查） | — |
| 零长度文件 | ✔ 合法（`size_comp == 0` 时跳过压缩比那条） | :317 |
| 截断 | ✔ `offset + sc > file_size ⇒ false` | :316 |

**结论**：删 `entries_fit_file` **没有损失**（它只是 `entries_fit_size(raw.size(), …)` 的别名）。
但"自洽"确实有三个缺口，**都不是删那一行造成的**：

- **D1（中）**：`offset` 只与文件总长比，不校验它落在 DATA 块里 ⇒ 一份把 `offset` 指到
  **HEADER 块内部**的伪造表也能"自洽"（解出来是垃圾，但 auto_detect 会认它）。
- **D2（中）**：`is_dir` 三格式**语义不统一** —— `parse_xdb` 是 `offset==0 && sr==0 && sc==0`；
  `fmt_null` 是 `fields[dir_i]==0`；`fmt_11xx` 是 `offset==0`。而自洽判定**无条件跳过** `is_dir`
  ⇒ 一旦某个解析路径把 `is_dir` 判错，一个巨大的目录条目就**绕过全部算术检查**。
- **D3（低）**：`size_real` 的解压上限（512 MiB）不在自洽阶段 ⇒ `auto_detect` 可能选中一个
  "每条都超过上限、必然解不出"的格式（解压阶段 fail-closed，不会崩，但会误导格式判定）。

## 三、检查点 2：`offset + size` 算术溢出 / 大文件与损坏 DB 的边界

| 位置 | 算法 | 是否会溢出 | 证据 |
|---|---|---|---|
| `entries_fit_size` | `e.offset + sc`（`uint32` 提升到 `uint64`） | **不会**（最大 2^33） | :316 |
| 同上 | `sc * kMaxLzoRatio`（sc 是 uint64，≤2^32） | **不会**（≤2^42） | :317 |
| `read_chunks` | `pos + 8ull + sz <= raw.size()` | **不会**（P17 修过；`8ull` 保证 uint64 域） | :412 |
| `read_chunks` | `pos > raw.size() - 8` | 当 `raw.size() < 8` 时 **size_t 下溢** ⇒ 条件为假 | :442（低：靠 while 条件兜住） |
| `walk_chain_from` | `pos += 8ull + sz` | 不会 | :499 |
| `chunk_head_at` | `8ull + sz <= remain` | 不会 | :476 |
| `parse_xdb` | `p + 12 + nl + 4 > d.size()` | 不会（nl ≤ 65519） | :211 |
| `build_header` | `off`（uint32）累计 + `it.data.size()` | **有闸门**（>4 GiB 报错、不产出） | :1013-1032 |

**上限常量**（`untrusted_limits.h`，全部具名、可判）：整包读 512 MiB、seek HEADER 512 MiB、
单条目解压 512 MiB、封包总量 **2 GiB**；两个环境变量（`XRKIT_DB_WHOLE_READ_LIMIT` /
`XRKIT_DB_PACK_LIMIT`）**只能往小改**（`:846-859`、`:1076-1083`，已判过）。

**大文件边界**：DB 格式的偏移是 `uint32` ⇒ 结构上只能表达前 4 GiB；>4 GiB 的包写不出去（有闸门）、
读侧也只能访问前 4 GiB。这是**格式限制**，不是缺陷，但值得写进文档（当前没写）。

**损坏 DB**：六格式各有一段"畸形就跳过/退出"的路径（`parse_xdb` 的 `nf<16 break`、
`parse_null` 的无 0 终止 `break`、`fmt_null` 的索引越界 `continue`）⇒ 都是"少解析"而不是"越界读" ✔。

## 四、检查点 3：重复路径 / 目录标记 / 排序稳定性 / 编码

| 维度 | 现状 | 风险 |
|---|---|---|
| **重复路径** | **完全不去重**：解析原样收；`pack_db` 也不查重 ⇒ 可以产出"两条同名条目"的包；解包时后者覆盖前者 | **中**：数据被静默覆盖，界面/日志都看不出来（见"判据覆盖现状"——没有判据） |
| **目录标记** | 见 D2（三格式三种判法）；写侧一律 `path + "\\"`，读侧 `normalize_path` 把 `\`→`/` 并去掉尾斜杠 ⇒ 往返一致 ✔ | 读侧一致；**D2 的 is_dir 语义**才是问题 |
| **排序稳定性** | 不排序 ⇒ 顺序 = 文件里的顺序（稳定）；`pack_db` 的顺序 = 调用方给的 `items` 顺序 | 低：产物顺序依赖调用方；`pack` 产物已有逐字节对拍 |
| **编码** | cp1251→UTF-8 读、UTF-8→cp1251 写；不可表示码点写 `?` 并记 `DbNameRewrite` 台账（有判据） | **低但真**：cp1251 的**未定义字节**（表里 `0xFFFD`）读出来是 U+FFFD，写回时 `rev[U+FFFD] < 0` ⇒ 变 `?` ⇒ **往返不保真**，且读侧不报损 |

## 五、检查点 4：加密 DB 的解密路径

- **2947 系加扰**：`Scrambler` 是**纯内存**变换（`encrypt` / `decrypt` 各返回新 vector），
  **没有任何临时文件** ✔（:168-186）。
- **插件解密**（`gate_active_read`）：产物落在**插件自己**的临时目录；db.cpp 只读 `gate.path`，
  **不拥有**它的生命周期。注释明确：`list_db_seek` **故意不建作用域**（:697-699），清理点在外层
  （`extract_package` / CLI dump）。⇒ **不是 RAII，靠调用方契约**。
  - 风险：中。若调用方在异常路径提前返回，解密产物会残留（会不会被下一次复用？取决于 gate 的命名）。
  - 判据侧：`extract-symlink`、`unpack-only` 等步骤覆盖了部分清理语义，但**没有一条**判"解密产物在失败路径上也被清掉"。

## 六、检查点 5：异常 / 分配失败是否留半状态

- **读侧**：全是局部 `vector` + 返回值，失败写 `error` 返回 ⇒ **无半状态** ✔。
- **`std::bad_alloc`**：没有 `try/catch` ⇒ 异常向上抛。读侧无副作用（安全）；
  **写侧** `pack_db` 的 `data` / `db` 都在内存里拼，落盘由外层 `write_file_atomic` 做 ⇒
  **不会产出半个文件** ✔。
- **峰值内存**（真实风险，低-中）：`pack_db` 里 `items[i].data` → `data`（2×）→ `db`（3×），
  上限 2 GiB ⇒ 峰值可达 **~6 GiB**。注释已承认"峰值 2–3 倍"（:1100-1103），但**没有按峰值设上限**。

## 七、其余发现（本文件内，按严重度）

| # | 位置 | 现象 | 严重度 |
|---|---|---|---|
| E1 | `validate_header` :365-377 | **自动嗅探（strict）要求路径是 ASCII 且前 8 字节含字母** ⇒ 一个"条目名全是俄文"的合法包在 `auto_detect` 里会被否掉（显式选格式才救得回）。与 D1/D2 叠加时更容易误判 | 中 |
| E2 | `extract_entry_seek` :936-937 | 传 `registry=nullptr` 调 `gate_active_read` ⇒ **不二次解密**。契约是"`entry` 必须来自同一 `path` 的 listing"；第三方直接拿原始密文路径 + 条目来调，会读出垃圾（T-02 只在调用方修过） | 中（接口契约隐患） |
| E3 | `parse_xdb` :221 | `is_dir` 判据是 `offset==0 && sr==0 && sc==0`；一个**合法的零长度文件**（sr=0, sc=0, offset≠0）不会被误判 ✔，但一个 `offset==0` 而 size>0 的条目会被判成目录并**跳过所有检查** | 低（与 D2 同源） |
| E4 | `read_chunks` :442 | `raw.size() - 8` 的下溢写法 | 低（靠 while 兜住） |
| E5 | `walk_chain_from` :491 | HEADER 块 >512 MiB ⇒ 返回 false ⇒ 大包退回整包读 ⇒ 又被 512 MiB 闸门拒绝 ⇒ **用户只看到"拒绝"**，不知道是 HEADER 太大 | 低 |
| E6 | `cp1251_to_utf8` :59 | 未定义字节 → U+FFFD，写回变 `?`（见四.编码） | 低 |

## 八、判据覆盖现状（我核过的）

| 主题 | 有判据吗 |
|---|---|
| 六格式 pack 产物 / 条目表 / 提取内容 vs 1.x | ✔ `golden_db.py`（GOLDEN-DB: PASS，denied=0） |
| 文件名 cp1251 顶替台账（`?` + `DbNameRewrite`） | ✔ `check_db_name_cp1251_lock.py` |
| 11xx 目录锁 / 名称锁 | ✔ `check_db_11xx_dirlock.py` / `db-name1251-lock` 步 |
| 整包读闸门（尺寸/环境变量只能往小改） | ✔ 有专门判据（P17-111/115/118 那几批） |
| 4 GiB 窄化（写侧） | ✔ 有判据（P17-65/68） |
| **重复路径** | **✘ 没有** |
| **`offset` 必须落在 DATA 块内** | **✘ 没有** |
| **`is_dir` 三格式语义一致性** | **✘ 没有** |
| **解密产物在失败路径上的清理** | **✘ 没有** |

## 九、给修复队列的建议（**不在通读时改**）

1. **D1**：自洽判定加"`offset >= 8` 且**落在某个 DATA 块范围内**"（需要把块区间传进去）。
2. **D2**：把 `is_dir` 的判定收敛成一个函数（三格式各自给参数），并在自洽判定里**不跳过目录条目**
   （至少校验 `offset==0` 与 size 全零）。
3. **D3**：自洽阶段就把 `size_real > 512 MiB` 拒掉（与 `decode_entry_bytes` 同口径）。
4. **重复路径**：`pack_db` 拒绝重名（或至少记台账）；读侧 `list_*` 报"重复条目 N 条"。
5. **E2**：`extract_entry_seek` 把 `registry` 作为**必填参数**（现在默认 nullptr 是个陷阱）。
6. **峰值内存**：`pack_db` 的上限按"峰值 ≈ 3×总量"折算，或分批写。

> 下一份：`src/engine` 其余（先按 ROI：`fs/squashfs_write.cpp`、`fs/db.cpp` 之外的大文件）。
