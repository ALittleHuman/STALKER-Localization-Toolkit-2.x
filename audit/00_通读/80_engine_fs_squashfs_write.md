# 通读台账 03：`src/engine/fs/squashfs_write.cpp`（**1209 行**）

> 2026-10-24。只记录：问题 / 调用关系 / 风险 / 证据。**不重构**。BETA.2 保持可比。
> 按用户给的五个检查点组织；最后与台账 02（`db.cpp`）互相参照。

## 一、职责

**自实现 SquashFS 封包**，产物要与 1.x 的 `tar2sqfs` 1.3.2 **逐字节一致**。文件头注释列了复刻清单 ①~⑨
（目录子节点字节序、inode 编号顺序、128 KiB 数据块、fragment 攒满/去重、整段块去重、8 KiB 元数据块、
超块 flags、表顺序、结尾补零）。

结构：
```
OutFile（可截断的随机写 + same_range 逐字节比）→ MetaWriter（8 KiB 分块 + 压不小时 bit15 原样）
Packer：build_tree → alloc_inode_numbers → process_file（数据块/fragment/整段去重）
        → serialize（inode 表 + 目录表）→ fragment 表 → id 表 → 超块回填 → 补零
pack_squashfs：`.part` 原子写（同目录 rename）
```

## 二、检查点 1：表字段 / 端序 / 块大小 / padding / 对齐

| 项 | 实现 | 结论 |
|---|---|---|
| 超块字段 | 96 字节，顺序：magic/inode_count/mkfs_time/block_size/frag_count/compression/block_log/flags/id_count/major(4)/minor(0) + 4×u64 表位置 + root_inode_ref + bytes_used | ✔ 与规范一致（`:1086-1109`） |
| 端序 | `put16/put32/put64` 全部小端；`rd32le`/`le32_at` 同 | ✔ |
| block_log | 由 block_size 逐位右移数出来（复刻 super.c） | ✔ `:130-134` |
| 块大小 | 4 KiB~1 MiB 的 2 的幂，否则明确报错 | ✔ `:1146-1150` |
| flags | `NO_XATTRS｜NO_DUPLICATES｜NO_FRAGMENTS` → 有碎片则换成 `ALWAYS_FRAGMENTS`（+ 按"是否全未压缩"置/清 `UNCOMPRESSED_FRAGMENTS`） | ✔ `:1048-1072` |
| padding | `dev_block_size`（默认 4096）补零；`bytes_used` **不含**补零、`file_size` 含 | ✔ `:1115-1125`、`res` 两字段分开 |
| 对齐 | 表与元数据块**不做额外对齐**（与参照一致：元数据是字节流、只补文件尾） | ✔（逐字节对拍覆盖） |
| inode 扩展判据 | 文件：`blocks_start > 4 GiB \|\| size > 4 GiB \|\| sparse > 0`；目录：`dir_size > 0xFFFC \|\| ent_count ≥ 256` | ✔ `:857-858`、`:892` |
| 目录索引记录字段序 | `index, start_block, size`（注释点明"先写 start_block 会被对拍抓住"） | ✔ `:909-914` |

## 三、检查点 2：gzip 写侧（块边界 / 空块 / 确定性 / 时间戳漂移）

- **块边界**：满 `block_size` 才写数据块；文件尾部（< 一块）**进 fragment**（`:767-852`）；
  fragment 攒到"再加就超一块"时先固化（`:836-838`）；结束时再 flush 一次（`:1037`）。
- **空块**：`all_zero` ⇒ 数据块 `block_sizes` 写 0（稀疏、不落盘）；fragment 全零 ⇒ 表项 `{0,0}`（`:661-666`、`:783-787`）。
- **压缩确定性**：`compress_block`（`compress.cpp`，本文件外）按 T-07 钉死的参数（gzip：level 9 / window 15 /
  memLevel 8 / 默认策略 / 不写 options 块）；块级探针曾实测 zlib 1.2.12 与 1.3.2 **逐字节一致**。
- **时间戳/元数据**：超块 `mkfs_time` 与所有 inode 的 `mtime` 都来自 `opt_.mtime`，
  **默认 0**（`squashfs.h:118` 注释："1.x 的 tar 里 mtime 全 0"），且**全仓没有任何调用点改它**
  （`git grep '\.mtime ='` 只命中本文件内部的结构体字段）。
  ⇒ **当前产物是确定的**；但"确定性"依赖调用方不改 `mtime/uid/gid/mode` —— **写侧不自证**（记为 R6）。
- **哈希不进镜像**：`xxh32` 只用于去重**预筛**，真正判定是逐字节比（`:55-56`、`:217-232`）。
- **`\r` 无关本文件**（那是 `run.cpp` 的事，台账 01 已记）。

## 四、检查点 3：失败路径（半成品 / 临时件 / 原子写 / 清理）

| 情形 | 行为 | 证据 |
|---|---|---|
| 正常 | 先写 `<out>.part`，**作用域结束后**（句柄已关）`rename` 到目标 | `:1159-1205` |
| 封包中途失败 | 删 `.part`；**目标路径一个字节不动**；删不掉要把理由**追加**进 `error` | `:1188-1196` |
| **改名失败** | 返回错误，**但 `.part` 不删**（与上一条不对称） | `:1198-1204` ← **R2（低）** |
| 旧 `.part` 残留 | `open` 用 `trunc` ⇒ 覆盖 | `:142` |
| `truncate()` 失败 | 句柄已关、`pos_` 未改 ⇒ 调用方立即返回（当前唯一调用点如此）；若有人继续写会用陈旧偏移 | `:194-215` ← **R5（低，契约）** |

★ 与 `db.cpp` 的风格差异：db 的落盘靠**外层** `write_file_atomic`，squashfs 是**自己**管 `.part` + rename。
两条都原子，但"谁负责清理"的约定不同（读的人容易搞混，记一条）。

## 五、检查点 4：写侧 ↔ parser 往返（空目录 / 长路径 / 重名 / 非 ASCII / 零字节）

| 场景 | 写侧行为 | 结论 |
|---|---|---|
| **空目录** | 显式目录条目会在树里留节点、写 inode、目录项 0 条、`nlink=2` | ✔ 保留（与"收集侧 `skipped_empty_dirs`"是**两回事**，别混） |
| **长路径** | 组件名 `put16(name.size()-1)`，**无长度检查** ⇒ 名字 > 65536 字节会**静默窄化** | **R4（低）**：实际 NTFS/常见 tar 不会到，但写侧没拦 |
| **重名** | `build_tree` **显式拒绝**：`重复条目` / `路径冲突（同名文件已存在）` | ✔ 且与 db.cpp **相反**（见参照表） |
| **非 ASCII** | 名字**原样字节**写入（不转码）；`sort_children` 用 `std::string <`（字节序，等价 strcmp） | ✔ 与参照一致 |
| **零字节文件** | `full_blocks=0, tail=0` ⇒ 不进 fragment、`block_sizes` 空、非扩展 inode | ✔ |
| 目录/文件同名 | 显式报冲突（隐式目录被显式条目补属性除外） | ✔ `:519-553` |

## 六、检查点 5：压缩选项 / fallback / "写了但读不回"

- **压缩器一致性**：超块 id、所有 `MetaWriter`、fragment 块、**`load_frag_block` 的解压**全部跟 `opt_.compressor`
  （`:1096` 的注释 + T-07 判据）。这是"去重比对读到错数据"的唯一入口，已被判据盯住 ✔。
- **fallback**：`compress_block` 返回 `compressed=false` ⇒ 原样存（数据块 bit24、元数据块 bit15）；读侧按位认 ✔。
- **"写了但读不回"的可疑点（都核过，当前无**）：
  - `write_table` 的位置数组用 `file.size()` 做块起点 ✔；
  - `serialize` 里 inode 编号连续（`alloc_inode_numbers` 给所有子节点编号、root 单独 `++counter`）⇒ `inodes_[n-1]` 不会越界 ✔；
  - `dedup_file_blocks` 的截断点 `kept` 逻辑与 `blocks_`/`blocks_by_sum_` 重建一致 ✔（注释记录过"漏记 fragment 块会截掉 frag 表指向的块"的真机事故）；
  - `fragment_lookup` 的越界防护（`offset + size > 数据长度 ⇒ continue`）✔。
- **无总量闸门**：`pack_squashfs` 不限总量（db 那边有 2 GiB 闸门）。>4 GiB 时 inode 自动升级 EXT、
  `bytes_used` 是 u64 ⇒ **格式层面安全**；但**内存峰值**（目录表 keep_in_memory + 块账本 + fragment 缓存）
  没有任何上限 ⇒ **R3（低）**。

## 七、其余发现（按严重度）

| # | 位置 | 现象 | 严重度 |
|---|---|---|---|
| R1 | `write_dir_inode` :888 | `nlink = 条目数 + 2`（不是 POSIX 的"子目录数+2"）——**与参照一致**，但照 POSIX 读会误判 | 信息 |
| R2 | `pack_squashfs` :1198-1204 | 改名失败不删 `.part`（失败路径删了） | 低 |
| R3 | `MetaWriter(keep=true)` :277-279 | 目录表**全内存**（每 8 KiB 一块的压缩产物），无上限 | 低 |
| R4 | `serialize` :987 | 名字长度写 `uint16`，无 >65536 检查 | 低 |
| R5 | `OutFile::truncate` :194-215 | 失败后 `pos_` 陈旧；当前调用方安全，属**契约** | 低 |
| R6 | `PackOptions` | 确定性依赖 `mtime/uid/gid/mode`；默认全钉死、当前无人改 | 信息 |
| R7 | `write_fragment_block` :690 | fragment 块**必须**记进块账本（漏了会截掉 frag 表指向的块）——已修、有真机证据 | 已修（记档） |
| R8 | `pack_squashfs` | 无总量闸门（与 db 的 2 GiB 闸门不对称） | 中（建议对齐） |

## 八、与台账 02（`db.cpp`）互相参照

| 主题 | `squashfs_write.cpp` | `db.cpp` | 该统一吗 |
|---|---|---|---|
| **重名条目** | **显式拒绝**（"重复条目"） | **不去重、静默覆盖**，且**无判据** | **该**：定"拒绝还是台账"（用户已排进队列） |
| 原子落盘 | 自己 `.part` + rename | 靠外层 `write_file_atomic` | 不必统一，但要写清"谁清理" |
| 失败清理 | 失败删 `.part`；改名失败**不删** | 外层负责 | 小修（R2） |
| 确定性来源 | `mtime/uid/gid/mode` 默认钉死 | pack 顺序依赖调用方给的 `items` | 两处都该在文档里点明 |
| 总量上限 | **无** | 2 GiB 闸门（且峰值 ~3×） | **该**（R8） |
| 自洽/校验 | 写侧自算偏移，无"自洽"概念 | 读侧 `entries_self_consistent*`（三个缺口 D1/D2/D3） | 不必统一（不同层次） |
| 目录 is_dir | 写侧不存在歧义（树节点自己带 `is_dir`） | **三格式三种判法**（D2） | db 侧收敛即可 |

## 九、给修复队列（与 db 的队列合并后的建议顺序）

1. **D2+E3**（db：is_dir 收敛 + 自洽不跳过目录）—— 用户已定第一；
2. **重名**：先定口径（建议 db 侧"拒绝 + 台账"，与 squashfs 对齐），再加判据；
3. **D1**（db：offset 落在 DATA 块内）；
4. **E2**（db：`extract_entry_seek` 的 `registry` 改必填）；
5. **峰值内存**：db 按 3× 折算；squashfs 的目录表 keep_in_memory 也纳入同一张"峰值预算"表；
6. **R8**（squashfs 无总量闸门）与 **R2**（改名失败不删 `.part`）—— 一并做，都是小改；
7. D3/E1/E5/E6、R1/R3/R4/R5 最后收。

> 下一份：`src/engine` 其余按 ROI —— 建议 `fs/squashfs.cpp`（读侧 parser，与本草稿的"写↔读往返"直接对照）。
