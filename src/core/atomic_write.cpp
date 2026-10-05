#include "xrkit/core/atomic_write.h"

#include "xrkit/core/path.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace xrkit::core {
namespace {

namespace stdfs = std::filesystem;

// ── 流式哈希（写一遍顺带算，零额外 I/O）────────────────────────────────
//
// ★★ 2026-10-05（用户裁决 ③）：为什么是 FNV-1a 64 位、而不引一个密码学哈希：
//   这条校验要拦的是**我们自己的写路径 bug**（长度截断、偏移错 1、缓冲区接错），
//   不是对抗性伪造。FNV-1a 的雪崩足够（改 1 位 ⇒ 输出面目全非），实现只有 5 行、
//   不引依赖、不拖慢写盘；长度**另有一条独立判据**（实际写出字节数 vs data.size()），
//   两者合起来覆盖"写错长度"与"写错内容"两大类。真要比"绝对确定"，那是
//   `WriteVerify::Readback` 那条路（逐字节比对，不靠哈希）。
//
// 为什么用"逐字节喂"而不是 8 字节一次：位序/端序在这里只是"我们自己两边一致"的问题，
// 逐字节累积最不容易写错，而且两个方向（内存那份、写出的那份）**共用同一个函数**
//   —— 不自洽的风险就此消掉。实测 100 MB 一个来回约 0.1 s，相对写盘可忽略。
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;   // FNV-1a 64 位 basis
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h = kFnvOffset) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

// 分片大小：与 `ofstream` 的缓冲无关（我们显式分片写，为了"边写边算"能落在同一遍循环里）。
// 1 MiB 是"每次 write 的调用次数"与"栈外缓冲区大小"之间的折中。
constexpr size_t kChunk = 1u << 20;

// 探针是**进程级**的，但只在显式设置时非空（默认零注入）。
WriteProbe g_write_probe;

// 清掉临时文件：**必须真的清掉**，而且失败要能说出来。
//
// ★ 2026-10-05 实测（③ 的注入自证里抓到的真缺陷）：Windows 上 `stdfs::remove` 会**因为句柄
//   还没落定而静默失败**（`error_code` 非零但没人看）——同一进程里刚动过那个文件的句柄（例如
//   探针的 `fstream`）会把它挡住一小会儿。原来那一行 `stdfs::remove(temp, ec);` 丢了返回值，
//   于是"解包失败还留一个 `.part`"这件事**谁也看不见**（真机 14,861 文件那批没暴露，是因为
//   失败路径从没被走到）。
//   ⇒ 短重试（50 × 2 ms）＋ 最终失败要写进 error（零静默：调用方拿得到逐条原因）。
bool remove_with_retry(const stdfs::path& p, std::string& error) {
    std::error_code ec;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (stdfs::remove(p, ec)) return true;      // 删掉了
        if (!stdfs::exists(p, ec)) return true;     // 本来就不在（幂等）
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::error_code last_ec;
    stdfs::remove(p, last_ec);
    if (stdfs::exists(p, last_ec)) {
        error += std::string(error.empty() ? "" : "；") + "清临时文件失败：" +
                 (last_ec ? last_ec.message() : std::string("仍存在"));
        return false;
    }
    return true;
}

}  // namespace

WriteProbe set_write_probe(const WriteProbe& probe) {
    const WriteProbe old = g_write_probe;
    g_write_probe = probe;
    return old;
}

bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, bool atomic,
                       std::string& error, WriteVerify verify) {
    error.clear();
    const stdfs::path target = path_from_utf8(path);
    std::error_code ec;
    const stdfs::path dir = target.parent_path();
    if (!dir.empty()) {
        stdfs::create_directories(dir, ec);
        if (ec) {
            error = "建目录失败：" + ec.message();
            return false;
        }
    }
    // ★ 临时文件路径**必须全程走宽串**：`Path::string()` 在 Windows 上按 **CP_ACP** 转窄串，
    // ★ 2026-09-27 更正（我自己的实测）：**俄文在 CP936 下能无损编码**（`файл` → `a7e6a7d1a7dba7dd`）；真正会退化成 `?` 的是 **CP_ACP 表示不了的字符**——韩文、日文假名、emoji、Ё 等。早先注释写"俄文会变 ?"是错的，别再照它复刻。
    //   ⇒ 临时文件打不开、原子写**必然失败**（2026-09-25 HanaAgent 审计查出；我们的逐字节语料
    //   路径全是 ASCII，所以一直没暴露）。用 `+= L".part"` 保住 native 宽串。
    stdfs::path temp = target;
    if (atomic) temp += L".part";

    // 内存里那份数据的哈希：**一次**算完，之后零额外 I/O。
    // `tamper_expected_hash` 只改**比对用的期望值**（自检注入；默认 0 = 不注入）——
    // 真实输出一个字节都不动，所以"没写对就不 rename / 清 `.part`"能被老实观测到。
    uint64_t expect_hash = fnv1a(data.data(), data.size());
    if (atomic && verify == WriteVerify::Streamed && g_write_probe.tamper_expected_hash != 0) {
        expect_hash = g_write_probe.tamper_expected_hash;
    }

    // ── 写（**边写边算**：写出的同一遍字节喂进哈希，同时数实际写出字节数）──
    uint64_t written = 0;
    uint64_t got_hash = kFnvOffset;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "打不开输出文件";
            return false;
        }
        bool io_bad = false;
        for (size_t off = 0; off < data.size();) {
            const size_t n = (std::min)(kChunk, data.size() - off);
            out.write(reinterpret_cast<const char*>(data.data() + off),
                      static_cast<std::streamsize>(n));
            if (!out) {
                io_bad = true;
                break;
            }
            got_hash = fnv1a(data.data() + off, n, got_hash);
            written += n;
            off += n;
        }
        out.close();
        // `!out` 未必报（旧注释的原话），所以既不忽略流状态、也**不信**它：
        // 真正的判据是下面的"实际写出字节数 + 写出的内容哈希"。
        if (io_bad || !out) {
            error = "写盘失败（磁盘满/被占用）：实写 " + std::to_string(written) + " / 应写 " +
                    std::to_string(data.size()) + " 字节";
            if (atomic) remove_with_retry(temp, error);
            return false;
        }
    }

    if (!atomic) return true;

    // ★★ 2026-10-05（本批小件 ②）：如实报出**这一次实际走的是哪一档**（结构事实）。
    //   位置在"写成功、校验之前"是有意的：它描述的是**这次调用用的档位**，
    //   与校验结果无关（失败也照样报 —— 那正是"开关到底有没有生效"要看的）。
    if (g_write_probe.record != nullptr) g_write_probe.record(path, verify);

    // 自检注入：写完之后、校验之前动一下临时文件（证明"读回复核真的在读回"）。
    // ★ **只在读回档触发**：流式档不读回临时文件，动了它只会让"写出去的产物"变成脏的，
    //   自检就自相矛盾了（2026-10-05 实测踩到）。默认（未设探针）这一段完全是死的。
    if (verify == WriteVerify::Readback && g_write_probe.tamper_temp_file != nullptr) {
        g_write_probe.tamper_temp_file(path_to_utf8(temp));
    }

    if (verify == WriteVerify::Readback) {
        // ── 读回逐字节复核（`WriteVerify::Readback`）──
        // 写盘被截断（磁盘满、被占用）只在**读回**时看得见；这条路的判据不靠哈希。
        std::vector<uint8_t> back;
        {
            std::ifstream in(temp, std::ios::binary);
            if (!in) {
                error = "复核读不回临时文件";
                remove_with_retry(temp, error);
                return false;
            }
            back.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }   // ★ 句柄必须先落定再谈删除（否则 Windows 上 remove 会静默失败，见 remove_with_retry）
        if (back.size() != data.size() ||
            (data.size() != 0 && std::memcmp(back.data(), data.data(), data.size()) != 0)) {
            // 逐条原因：长度对不上要给两个数，长度一样就是内容不同。
            if (back.size() != data.size()) {
                error = "复核不一致（临时文件 " + std::to_string(back.size()) + " 字节 ≠ 待写 " +
                        std::to_string(data.size()) + " 字节）";
            } else {
                error = "复核不一致（临时文件与待写内容不同）";
            }
            remove_with_retry(temp, error);
            return false;
        }
    } else {
        // ── 默认：流式哈希自校验（零额外 I/O）──
        if (written != data.size()) {
            error = "自校验不一致（实写 " + std::to_string(written) + " 字节 ≠ 待写 " +
                    std::to_string(data.size()) + " 字节）";
            remove_with_retry(temp, error);
            return false;
        }
        if (got_hash != expect_hash) {
            error = "自校验不一致（写出的字节哈希与待写内容不同）";
            remove_with_retry(temp, error);
            return false;
        }
    }

    // 同盘 rename（1.x 是 `os.replace`）：要么旧文件还在，要么新文件完整
    stdfs::rename(temp, target, ec);
    if (ec) {
        error = "替换目标文件失败：" + ec.message();
        remove_with_retry(temp, error);
        return false;
    }
    return true;
}

}  // namespace xrkit::core
