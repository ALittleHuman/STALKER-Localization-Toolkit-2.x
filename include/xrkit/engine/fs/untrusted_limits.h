// ★★ 2026-10-10（P17-13，审计 01 F-08 Medium）：**不可信声明尺寸上限的唯一一处定义**。
//
// 背景（修前事实）：
//   · `src/engine/fs/lzhuf.cpp` 里写的是**内联字面量** `declared > 256 * 1024 * 1024`；
//   · `src/engine/fs/db.cpp` 里是 `constexpr uint64_t kMaxDecompressBytes = 512ull * 1024 * 1024;`；
//   · 审计另点名的 `src/engine/fs/lzhuf_ref.c` 那处硬编码 —— ★★ **2026-10-13（P17-115）更正**：
//     我上一版在这里写"**先查后确认已不存在**"，那是**假的**。复审计（`N-01` P0）用
//     `git ls-files` 证伪：`src/engine/fs/lzhuf_ref.c` **被跟踪、确实存在**，其中
//     `textsize > 256*1024*1024` 一直在（我上一轮轻信了旧注释、**没去 `git grep` 核**）。
//     ⇒ 已改为走下面的共享宏。
//   · 另有 `src/engine/fs/squashfs.cpp` 也**又写了一遍**裸的 `512ull*1024*1024`
//     （同值、未 include 本头、**门禁不查**）—— 同批改为 include 本头派生。
//   ⇒ 同一语义（"来自包的、不可信的声明尺寸"）的常量曾经分散在**三处** TU、取值不同、
//     且注释自称"值必须能对照"，谁改一处另一处不会跟着变。
//   ★ 教训（写在这里当路标）：**"注释说某处已不存在"不等于"真的不存在"** ——
//     凡是"某处还有没有裸字面量"这类断言，必须**两向可判**（门禁查"共享宏真的被用了"，
//     而不是查"那句注释还在不在"）。本批把判据也照这个改了。
//
// 本头文件的处置（**刻意不强行把两个值改成一样**）：
//   两个值服务的是两种不同容器，**改小 db 那一条会改真实大包的行为**（1.9 GB / 8 GB 级 DB 的
//   单条目解压），那属于"口径变更"，要先取证再动 —— 不在本批。这里做的是：
//   ① 定义收拢到一处（改这里两处一起变，编译器保证同步）；
//   ② 写清各自为什么是这个数、以及两者的大小关系（`static_assert` 钉住）；
//   ③ 判据（`run_ci` 的 shell-smoke 步）禁止这两处再出现裸字面量。
#ifndef XRKIT_ENGINE_FS_UNTRUSTED_LIMITS_H
#define XRKIT_ENGINE_FS_UNTRUSTED_LIMITS_H

// LZHUF（DB 头/流）里"声明解压长度"的上限。
// 256 MiB：LZHUF 是逐字节哈夫曼，解码缓冲按声明长度**一次分配** ⇒ 这个数直接决定单次分配峰值。
#define XRKIT_LZHUF_MAX_DECLARED_BYTES (256ull * 1024ull * 1024ull)

// DB 单条目解压上限（`db.cpp` 的 `kMaxDecompressBytes` 由它派生）。
// 512 MiB：比 LZHUF 那条宽一倍 —— DB 条目的"声明长度"要覆盖真实语料里最大的单条目，
// 实测 1.9 GB 级包里最大的单条目也在 512 MiB 以内；放宽它要连带复核
// `01 F-18` 那条 `block_sizes` 自证（squashfs 侧的块数上限）。
#define XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES (512ull * 1024ull * 1024ull)

// xdb 快路径的"块链头"读取上限（`db.cpp` 的 `kMaxSeekHeaderBytes` 由它派生）。
// 512 MiB：与 DB 单条目上限同值但**语义不同**（这是"顺序读多少头字节去试算 offset"），
// 收拢在这里是为了不再出现第二个裸的 512 MiB 字面量。
#define XRKIT_DB_MAX_SEEK_HEADER_BYTES (512ull * 1024ull * 1024ull)

// ★★ 2026-10-13（P17-111，复审计 `01 F-09`）：**"退回整包读"的文件尺寸上限**。
//
// 修前事实：`db.cpp` 退回整文件读那一段是**无上限**的
//   `std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), …)` ——
// 而它发生在"**还认不出这是不是 DB**"之前（seek 式判定失败才退到这里）
//   ⇒ 用户在文件对话框里随手指一个几 GB 的文件（镜像/视频/安装包）就会被**整个读进内存**：
//     白读一遍，还可能把进程直接撑爆（32 位构建必爆，64 位看物理内存）。
//
// 512 MiB：**与 DB 单条目解压上限同值**，理由是"能整包进的 DB 才有意义" ——
//   真实语料里最大的 DB 是 1.9 GB 级，那种包**必须走 seek 式**（块头 ＋ 尾部表）；
//   走到"退回整包读"这条路的包本来就该是小的/畸形的。放宽它要连带复核：
//   `01 F-09` 的分册证据（哪些真实包会退到这条路）与内存峰值实测。
#define XRKIT_DB_MAX_WHOLE_READ_BYTES (512ull * 1024ull * 1024ull)

// ★★ 2026-10-18（T-05，第三方只读审计）：**DB 封包"待封入字节总量"的上限**。
//
// 修前事实（审计原话）：`fs_page.cpp::run_pack_job` 把每个文件读进 `DbPackItem.data`，
//   `pack_db()` 再拼一份连续的 `data`，最后 `write_file_atomic` 又拷一份 `vector`
//   ⇒ **峰值约 2–3 倍总大小，且没有任何内存闸**；1.9 GB 级包会直接 OOM。
//   SquashFS 分支是**流式**的（逐块读、逐块写），两条路不对称。
//
// 2 GiB：口径是"**覆盖真实语料里最大的 DB**"—— 真机最大的是 1.9 GB 级，2 GiB 留了余量；
//   按 2–3 倍峰值，封一个 2 GiB 的包要 4–6 GiB 内存，这已经是"能跑就别同时开别的"的量级。
//   **放宽它必须连带复核内存峰值**（那是口径变更，要单独裁定），所以环境变量
//   `XRKIT_DB_PACK_LIMIT` **只能往小改**（判据用它造小夹具，不必真造 GB 文件）。
//   ★ 真正的解法是"先写数据区、再写头"的**流式封包**（那时这条上限才能放开）—— 未做。
#define XRKIT_DB_MAX_PACK_BYTES (2ull * 1024ull * 1024ull * 1024ull)   // 2 GiB（别看漏那个 1024）

// 两者的大小关系是**有意的**（见上）：DB 那条不许被改到比 LZHUF 那条还小，
// 否则 DB 侧会出现"能声明、却过不了 LZHUF 闸"的条目（读得进却解不开）。
#if defined(__cplusplus)
static_assert(XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES >= XRKIT_LZHUF_MAX_DECLARED_BYTES,
              "DB 单条目上限不许小于 LZHUF 声明长度上限（否则合法条目读得进却解不开）");
// T-05：单条目能压进包，整包上限自然要大于它（否则"能解开的条目压不回去"）。
static_assert(XRKIT_DB_MAX_PACK_BYTES >= XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES,
              "DB 封包总量上限不许小于单条目解压上限（否则合法条目压不进包）");
#endif

#endif  // XRKIT_ENGINE_FS_UNTRUSTED_LIMITS_H
