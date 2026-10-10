#pragma once
// xrkit::core —— 原子写盘（两条写出路径共用的一份实现）。
//
// 为什么做成公共的：解包（`engine::fs::extract_tree`）与编码转换（`engine::text::convert`）
// 都需要同一条纪律 —— **同目录 `.part` → （自校验）→ 同盘 rename**，
// 任一环失败先 unlink。这条纪律写两份就一定会有一份先腐坏，而它保护的正是
// "宁可不写，也不留一个看起来存在的坏文件"。
//
// 与 1.x 的对应：`sqfs_extract` 的 `open(tmp,"wb")` → `os.replace(tmp, p)`；
// 2.0 多加了一步**写后校验**（写盘被截断只在读回时看得见，`ofstream` 的 `!out` 未必报）。
//
// ★★ 2026-10-05（用户裁决 ③）：写后校验**默认**改为"**边写边算哈希**"——
//   写出的同一遍字节同时喂进哈希并数出**实际写出字节数**，写完与**内存中那份数据**
//   的哈希/长度比对（零额外 I/O）；抓的仍是"我们写错长度/偏移/内容"这一类。
//   为什么改：旧的**读回逐字节复核**是"按文件个数付钱"——真机 `gamedata.sq_base_configs`
//   （14,861 文件）上实测读回复核 ≈ 17.9 s（31.1 s → 13.25 s 的差），与 rds 参照的
//   13.8 s 差出一个量级，而它换来的只是"确认写出的等于想写的字节"。
//   **读回逐字节复核没有删**：`WriteVerify::Readback`（调用方给开关）保留"绝对确定"的场合，
//   也留着它的注入式自证（见下面的探针）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

// 写后校验档位。
enum class WriteVerify {
    Streamed,  // 默认：边写边算哈希 + 实际字节数，与内存那份比对（零额外 I/O）
    Readback,  // 读回临时文件逐字节复核（绝对确定；按文件个数付读一遍 I/O 的钱）
};

// 写盘探针（**只为自检而存在**：没有探针时三条路逐字节等价，默认零注入）。
//
// 为什么要有它：`③` 的两条锁都要求"先红后绿"——
//   ① 流式哈希自校验必须**抓得住**"写出去的字节与内存里那份不同"（少 1 字节/偏移错 1）；
//   ② 读回开关必须**真在读回**（否则它就是个永远为真的开关）。
// 这两种注入都必须发生在**写的内部**（进程外的测试改不到"写到一半的数据"/"比对用的那份"），
// 所以把注入口做成显式探针，而不是在生产代码里留一段"故意写错"的死代码。
//
// ★ 2026-10-05 实测踩到的坑（写在这里免得再犯）：`tamper_temp_file` 对**两条档位都会触发**
//   （它在写完之后、校验之前）。拿它去证"流式档"会自相矛盾——写出去的文件都已经被改过了，
//   无论 rename 不 rename 都不再是"正确产物"。所以两条锁各用**自己的**注入口，且各自独立文件。
struct WriteProbe {
    // 流式校验阶段：把"内存里那份"的**期望哈希**换成调用方给的值（0 = 不注入）。
    // 写盘那侧仍用真 data ⇒ 哈希比对必不一致 ⇒ 抓的就是"写出去的不是想写的"这一类。
    // ★ 只改**比对用的期望值**，一个字节都不动真实输出 ⇒ "没写对就不 rename" 能被老实观测到。
    uint64_t tamper_expected_hash = 0;
    // 写完之后、校验之前：动一下临时文件（探针自己按 native 宽串拼 `.part`）。
    // 用来证明"读回复核真的在读回"。**只该在读回档打开**（见上面的坑）。
    bool (*tamper_temp_file)(const std::string& path_utf8) = nullptr;
    // ★★ 2026-10-05（本批小件 ②「性能锁换成**行为型**」）：**每次原子写都回调一次**，
    //   把"这一次实际走的是哪一档"如实报出来 —— 结构事实，与机器快慢无关。
    //
    // 为什么需要它：`③` 那条性能锁原来拿**耗时上界**当判据（"默认档不许比参照慢出量级"），
    // 而本机实测余量只有 8%，旧默认档 47 s 也照样能过 ⇒ **既假红又没用**。
    // 真正要钉的是结构事实：**默认档必须走 Streamed（零额外读盘）、`--verify-readback`
    // 必须走 Readback**。这个开关一旦失效（例如有人把 verify 写死成 Streamed），
    // 耗时上界**看不出来**，而本回调一眼能看出来。
    //   · 传的是**实际传给 `write_file_atomic` 的那一档**（不是调用方"打算"用的那一档）；
    //   · 只对 `atomic=true`（真正有 .part/校验的那条路）触发；
    //   · 默认 nullptr ⇒ 生产路径零注入、零额外开销。
    void (*record)(const std::string& path_utf8, WriteVerify mode) = nullptr;
};

// 安装/复位进程级探针（返回**旧**探针，便于调用方 finally 式还原）。
// 永远是"显式设置"才生效：默认（未设置）三条路都零注入。
WriteProbe set_write_probe(const WriteProbe& probe);

// ★★ 2026-10-10（P17-14，审计 01 F-15 Low）：**Windows 保留设备名不许当文件名**。
//   `CON` / `PRN` / `AUX` / `NUL` / `COM1..9` / `LPT1..9`（含带扩展名的 `CON.txt`、大小写任意）
//   在 Windows 上是**设备**：对它写入不是报错而是**被设备吃掉**（内容静默丢失，或写到控制台），
//   对它判存在/删除也都是"成功"（假象）。整棵 gamedata 里有玩家自制文件名时这就是一条
//   静默数据丢失路径。判据只认**主名**（`console.txt` 合法；`CON.txt` 非法）。
//   `utf8_name` 传**单个路径分量**（文件名或目录名）；返回 true ⇔ 该分量命中保留名。
bool is_windows_reserved_name(const std::string& utf8_name);

// 把 data 写到 path。`atomic=false` 时直接写目标（仅用于明确不要临时文件的场合）。
// `verify` 只在 `atomic=true` 时有意义（`atomic=false` 没有临时文件，也就没有可校验的中间态）。
// 成功返回 true；失败返回 false 并把原因写进 error（调用方负责上报，**不留全局变量**）。
// ★ P17-14：命中 Windows 保留设备名时**直接拒绝**并把理由写进 error（不再往设备里写）。
bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, bool atomic,
                       std::string& error, WriteVerify verify = WriteVerify::Streamed);

}  // namespace xrkit::core
