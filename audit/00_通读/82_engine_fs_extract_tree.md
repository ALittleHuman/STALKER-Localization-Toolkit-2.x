# 通读台账 05：`src/engine/fs/extract_tree.cpp`（**826 行**，落盘层）

> 2026-10-24。只记录：问题 / 调用关系 / 风险 / 证据。**不重构**。BETA.2 保持可比。
> 本页回答两个悬着的问题：**R11（路径遍历）写侧该不该拒**、**R15（slink）是不是双缺**。
> 并**更正台账 02 的一条判断**（E2，见 §七）。

## 一、职责

把"镜像里的条目"落成磁盘上的树，**SquashFS 与 DB 六格式两条分支逐条同口径**：
安全路径判定（`safe_out_path`）→ 真实文件系统复核（`refuse_if_link_traversed`）→
`write_file_atomic` 落盘 → **产物口径记账**（`ArtifactTally`：同一落点只算一次）。
外加落点规则唯一一份（`unpack_target_dir` / `paths_carry_shell` / `preview_entries_in_layout`）。

## 二、检查点 1：路径遍历（这是 R11 的答案：**写侧已经拒了，而且拒得很细**）

`safe_out_path` 逐条拦（每条的**拒因字符串**都不同，便于判定"为什么被拒"）：

| 形态 | 判据 | 证据 |
|---|---|---|
| 空路径 | 直接拒 | `:245-248` |
| 绝对路径 / **UNC** | `\`→`/` 后首字符是 `/` ⇒ 拒（`\\server\share` 归一成 `//server/…`） | `:250-257` |
| **盘符**（`C:…`） | 第 2 字符是 `:` ⇒ 拒 | `:258-261` |
| **`..`** | 分段去尾点/空格后等于 `..` ⇒ 拒；**`.. `/`.. .`/`...` 同族**（剥完只剩点）也归这条拒因 | `:272-282` |
| 纯空格段 | 剥完为空且无点 ⇒ 单独拒因"Windows 建不出这种名字" | `:273-277` |
| 段里有**冒号**（NTFS ADS） | 拒（`:evil` 会把内容写进隐藏流） | `:296-299` |
| 段带**根名/根目录**（`D:` 这类盘符相对） | 拒（不依赖 `join` 悄悄换根） | `:288-292` |
| 兜底 | `is_within` **逐组件**比（不是字符串前缀），Windows 下大小写不敏感 | `:146-156`、`:316-319` |

★ 结论：**路径遍历字面形态（`..`/绝对/UNC/盘符/ADS/尾点空格变体）在落盘层已经拒掉了** ——
R11 的答案不是"parser 该拒"，而是"**parser 不拒也没关系，但 `list_entries()` 对外仍交出这些路径**"
（非 extract 的消费者要自己防）。这条建议可以**降级**：把 `list_entries` 的契约写清即可，不必然改 parser。

## 三、检查点 1 的剩余口子：**保留名**与**NUL/控制字符**

| 形态 | 现状 | 结论 |
|---|---|---|
| **Windows 保留名**（`CON`/`NUL`/`PRN`/`AUX`/`COM1..9`/`LPT1..9`） | **只在 `write_file_atomic` 的"叶子"上筛**（`atomic_write.cpp:117` 用 `is_windows_reserved_name(leaf)`）；**目录段没筛** | **R20（中）**：`CON/` 这类**目录段**会走到 `create_directories`，没有统一筛；叶子被拒是可见的（算 failure） |
| **NUL / 控制字符** | `safe_out_path` **完全不查**；parser 也不查 | **R10 确认（三层都缺）**：名字里的 `\0` 交给 Win32 路径 API 会**在 NUL 处截断** ⇒ 实际写到**另一条更短的路径**，而我们的判定仍按原串"在输出目录内" ⇒ **判定与落点不一致**（"写到意外路径"那一类）。这条**必须补在落盘层**（parser 补是更好，但不能只补 parser） |

## 四、检查点 2：长路径

- **没有任何 `\\?\` 前缀**（`core/path.cpp`、`core/atomic_write.cpp` 都查过，零命中）。
- 后果：>MAX_PATH（260）的路径在 Win32 API 上**直接失败**（除非进程清单开了长路径），
  ⇒ R9 的 69 KB 路径**不会静默截断，而是可见失败**（记进 `failures`）✔。
- 但**失败前已经建的父目录不会回滚**（`create_directories` 先跑）⇒ **R22（低）**：半成品目录树残留（只记 failure）。

## 五、检查点 3：类型（R15 的答案：**parser 与落盘层双缺**）

- `Entry` 没有类型字段（台账 04 已证）；本文件**只分 `is_dir` 与非 `is_dir`**，非目录一律
  `read_file()` → `read_file_data()` ⇒ **slink 会被当普通文件去读数据块**。
- 全仓 `git grep 'create_symlink|SYMBOLIC_LINK'` ⇒ **零命中**：**本产品根本不落 symlink**。
- ⇒ **R15 确认是双缺**（parser 没有类型 + 落盘层没有分派 + 从来没有创建链接的能力）。
  对 STALKER 语料无影响（真机包里有 slink 的概率极低，且 `list` 会报尺寸不符），
  但**对"别人给的镜像"是可见失败而非静默**（`read_file_data` 长度不符会报错）—— 属"能报错"的缺，不是"悄悄写错"。

## 六、检查点 4：重名 / 覆盖

| 情形 | 行为 | 是否可见 |
|---|---|---|
| 同名文件两条 | **照写**（后写覆盖），`had_file` ⇒ 进 `overwritten`；计数走产物口径 ⇒ 第二条记 `duplicate_records` | ✔ **两个通道都可见** |
| **大小写不同**（`A.txt` / `a.txt`） | Windows 上同一文件 ⇒ 同上（覆盖 + `overwritten` + `duplicate_records`） | ✔ 可见（`artifact_key` 在 Windows 折小写） |
| **文件与目录同名** | 谁先谁后都会**报错**：先目录后文件 ⇒ `write_file_atomic` 目标不是文件；先文件后目录 ⇒ `create_directories` 报 ec | ✔ 可见 |
| 与台账 03/04 对照 | 写侧**拒绝**重名、读侧**全输出+索引取首个**、落盘层**照写并可见覆盖** | ⇒ 同一项目**三种**口径 → 队列第 2 条 |

## 七、检查点 5/6：原子写、失败清理、总量

- **原子写**：两条分支都用 `core::write_file_atomic`（同目录 `.part` → rename；默认流式哈希自校验，
  开关打开才读回逐字节）✔；失败 ⇒ 记 failure，**目标不被改写** ✔（`overwritten` 记在写成功之后，避免谎报）。
- **失败清理**：文件不留半成品 ✔；**已建的目录不回滚**（R22）。
- **总量**：`bytes_written` 只是计数器，**没有任何累计上限**（parser 只有"单文件 ≤512 MiB"）⇒
  **R8 在落盘侧也成立**（第三处）。磁盘满 ⇒ `write_file_atomic` 失败 ⇒ 可见 ✔。
- 内存：`ArtifactTally` 的 `std::set` 与 `result.written` 随条目数线性增长（4 Mi 条目上限来自 parser）⇒ 信息。

## 八、★ 更正台账 02 的一条判断（E2）

台账 02 说"`extract_entry_seek` 传 `registry=nullptr` 是**不二次解密**的陷阱"。
读 `registry.h` 的活动钩子注释后**更正**：`gate_active_read(path, registry)` 的 `registry == nullptr`
语义是"**用活动注册表**"（前端通过 `set_active_registry()` 注入；没有注入时原样放行）。
⇒ `extract_tree.cpp` 里 `list_entries(image_path)` / `read_file(image_path)` 传 nullptr **是正确的**，
不是缺陷。**真正的坑**只剩一条：**没人注入活动注册表时，加密包会被当明文读**（那是前端契约，不是本文件的 bug）。
E2 从"中"降为"**文档级**"：把"`nullptr` = 用活动注册表"写进 `registry.h` 已经有的注释里即可（它已写了，只是别处会误读）。

## 九、其余发现

| # | 位置 | 现象 | 严重度 |
|---|---|---|---|
| R20 | `safe_out_path` | 保留名只由 `write_file_atomic` 在**叶子**上拦；**目录段不筛** | 中 |
| R21 | `ArtifactTally` | 大小写碰撞的覆盖**可见**（overwritten + duplicate_records）——不是静默 bug，记录备查 | 信息 |
| R22 | `create_directories` | 失败不回滚已建目录 | 低 |
| **R23** | `extract_tree.cpp:187` | `const stdfs::file_status root_status = ...` **赋值后从未使用**（全文件唯一一处）⇒ **第一类死代码候选**（编译器没报 C4189，因为它是"有副作用的初始化"形态） | 低（清理批次收） |
| R24 | `refuse_if_link_traversed` | 每个文件都要走一遍"逐段 stat"，深目录 + 大包时是 O(条目×深度) 次 syscall（性能，非正确性） | 低 |
| R25 | `extract_tree` | `result.ok = true` 在**列完条目之后**就置位 ⇒ 单条失败不影响整体 ok（设计如此，但调用方必须看 `failures`） | 信息 |

## 十、对队列的更新（按你的调整）

1. **R10（NUL/控制字符）＋ R20（保留名，含目录段）** —— **提到最前**（"写到意外路径"的安全类）
2. **D2+E3**（db：is_dir 收敛 + 自洽检查目录）
3. **重名口径统一**（写侧拒 / 读侧报重复条数 / 落盘照写但可见 —— 三处对齐 + 判据）
4. **D1**（db：offset 落在 DATA 块内）
5. **R4+R9 一起**（写侧拒 >65536；读侧文件名与目录名**同一把 4096 尺**）
6. **总量/峰值**：db 3× 折算、sqfs 读侧累计、**落盘侧累计**（R8 三处一起）
7. **R2**（`.part` 清理）
8. **R12/R14/R15**（fragment 对账、重名报数、slink 类型化 —— R15 若要支持链接就是**新功能**，不是修 bug）
9. R21~R25、D3/E1/E5/E6 最后收

★ R11 降级：落盘层已经拒得很细（`..`/绝对/UNC/盘符/ADS/尾点空格变体全有独立拒因）；
剩下的是"`list_entries` 把原始路径交给消费者"的**契约问题**，不是漏洞。

> 下一份：`src/engine` 其余按 ROI —— 建议 `fs/overlay.cpp`（`collect_tree`/`dir_diff` 是 `walk-one` 判据的核心）
> 或 `text/extract.cpp`（台账 01 提到过它删了一个零引用函数）。
