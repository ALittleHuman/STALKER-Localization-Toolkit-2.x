// ★★ 2026-10-10（P17-13，审计 01 F-08 Medium）：**不可信声明尺寸上限的唯一一处定义**。
//
// 背景（修前事实）：
//   · `src/engine/fs/lzhuf.cpp` 里写的是**内联字面量** `declared > 256 * 1024 * 1024`；
//   · `src/engine/fs/db.cpp` 里是 `constexpr uint64_t kMaxDecompressBytes = 512ull * 1024 * 1024;`；
//   · 审计另点名的 `src/engine/fs/lzhuf_ref.c` 那处硬编码 **先查后确认已不存在**
//     （当前文件里只有 `MAX_FREQ` 等 LZHUF 内部常量，没有再写 256 MiB —— 审计给的行号是旧版本）。
//   ⇒ 同一语义（"来自包的、不可信的声明解压尺寸"）的常量分散在两个 TU、取值不同、
//     且注释自称"值必须能对照"，谁改一处另一处不会跟着变。
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

// 两者的大小关系是**有意的**（见上）：DB 那条不许被改到比 LZHUF 那条还小，
// 否则 DB 侧会出现"能声明、却过不了 LZHUF 闸"的条目（读得进却解不开）。
#if defined(__cplusplus)
static_assert(XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES >= XRKIT_LZHUF_MAX_DECLARED_BYTES,
              "DB 单条目上限不许小于 LZHUF 声明长度上限（否则合法条目读得进却解不开）");
#endif

#endif  // XRKIT_ENGINE_FS_UNTRUSTED_LIMITS_H
