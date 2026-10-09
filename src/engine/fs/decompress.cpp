#include "xrkit/engine/fs/decompress.h"

#include <mutex>
#include <string>

#include <lz4.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include "minilzo.h"

namespace xrkit::engine::fs {
namespace {

std::once_flag g_lzo_once;
bool g_lzo_ok = false;

void ensure_lzo() {
    std::call_once(g_lzo_once, []() { g_lzo_ok = (lzo_init() == LZO_E_OK); });
}

// ── ★★ 2026-10-10（P17-2，审计 01 F-02 High）：解码器的**内存闸**（唯一一份口径）──────
// 事实（修前）：xz 分支把 `memlimit` 置成 `UINT64_MAX` —— 等于把解码器自身这道闸**关掉**。
//   `.xz` 块头里声明的 `dict_size` 最大可到约 1.5 GiB，攻击者据此即可让 liblzma 分配 GB 级
//   内存；输出缓冲虽由 `out_capacity` 约束，但**管不住解码器内存**。
// 口径：给一个与块语义匹配的上限 —— 输出容量的 64 倍，且**封顶 256 MiB**、**下限 8 MiB**
//   （解码器自身还有固定开销，给小了会把合法小块也拒掉）。
//   · 元数据块（8 KiB）⇒ 512 KiB → 抬到下限 8 MiB；
//   · 数据块（≤ 1 MiB）⇒ 64 MiB ✓。
//   超限时 liblzma 返回 `LZMA_MEMLIMIT_ERROR` ⇒ 走"明确报错"（零静默，不许只给 bad_alloc）。
constexpr uint64_t kDecoderMemLimitFloor = 8ull * 1024 * 1024;     // 8 MiB
constexpr uint64_t kDecoderMemLimitCap = 256ull * 1024 * 1024;     // 256 MiB

uint64_t decode_mem_limit(size_t out_capacity) {
    const uint64_t wanted = static_cast<uint64_t>(out_capacity) * 64ull;
    return wanted < kDecoderMemLimitFloor
               ? kDecoderMemLimitFloor
               : (wanted > kDecoderMemLimitCap ? kDecoderMemLimitCap : wanted);
}

}  // namespace

const char* compression_name(Compression c) {
    switch (c) {
        case Compression::Gzip: return "gzip";
        case Compression::Lzma: return "lzma";
        case Compression::Lzo: return "lzo";
        case Compression::Xz: return "xz";
        case Compression::Lz4: return "lz4";
        case Compression::Zstd: return "zstd";
        case Compression::Unknown: break;
    }
    return "unknown";
}

bool decompress_block(Compression comp, const uint8_t* src, size_t src_len, size_t out_capacity,
                      std::vector<uint8_t>& out, std::string& err) {
    if (out_capacity == 0 || src_len == 0) {
        err = "解压参数非法（容量或输入为 0）";
        return false;
    }
    out.assign(out_capacity, 0);

    switch (comp) {
        case Compression::Gzip: {
            uLongf dst_len = static_cast<uLongf>(out_capacity);
            const int rc = uncompress(out.data(), &dst_len, src, static_cast<uLong>(src_len));
            if (rc != Z_OK) {
                err = "zlib 解压失败（rc=" + std::to_string(rc) + "）";
                return false;
            }
            out.resize(static_cast<size_t>(dst_len));
            return true;
        }
        case Compression::Xz: {
            // ★★ P17-2（审计 01 F-02）：memlimit 由 `UINT64_MAX` 改成与块语义匹配的上限
            //   （见文件头 `decode_mem_limit`）。超限返回 `LZMA_MEMLIMIT_ERROR` ⇒ 明确报错。
            uint64_t memlimit = decode_mem_limit(out_capacity);
            size_t in_pos = 0;
            size_t out_pos = 0;
            const lzma_ret rc = lzma_stream_buffer_decode(&memlimit, 0, nullptr, src, &in_pos,
                                                          src_len, out.data(), &out_pos,
                                                          out_capacity);
            if (rc != LZMA_OK) {
                if (rc == LZMA_MEMLIMIT_ERROR) {
                    err = "xz 解码内存超过上限（块头声明的字典过大）：块声明的字典需要 " +
                          std::to_string(memlimit) + " 字节以上，上限=" +
                          std::to_string(decode_mem_limit(out_capacity)) + " 字节";
                    return false;
                }
                err = "xz 解压失败（code=" + std::to_string(static_cast<int>(rc)) + "）";
                return false;
            }
            out.resize(out_pos);
            return true;
        }
        case Compression::Lz4: {
            const int rc = LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                                               reinterpret_cast<char*>(out.data()),
                                               static_cast<int>(src_len),
                                               static_cast<int>(out_capacity));
            if (rc < 0) {
                err = "lz4 块解压失败（rc=" + std::to_string(rc) + "）";
                return false;
            }
            out.resize(static_cast<size_t>(rc));
            return true;
        }
        case Compression::Zstd: {
            const size_t rc = ZSTD_decompress(out.data(), out_capacity, src, src_len);
            if (ZSTD_isError(rc)) {
                err = std::string("zstd 解压失败：") + ZSTD_getErrorName(rc);
                return false;
            }
            out.resize(rc);
            return true;
        }
        case Compression::Lzo: {
            ensure_lzo();
            if (!g_lzo_ok) {
                err = "lzo_init 失败";
                return false;
            }
            lzo_uint dst_len = static_cast<lzo_uint>(out_capacity);
            const int rc = lzo1x_decompress_safe(src, static_cast<lzo_uint>(src_len), out.data(),
                                                 &dst_len, nullptr);
            if (rc != LZO_E_OK) {
                err = "lzo 解压失败（code=" + std::to_string(rc) + "）";
                return false;
            }
            out.resize(static_cast<size_t>(dst_len));
            return true;
        }
        case Compression::Lzma:
            // 口径：**不写没法验证的代码**。旧 lzma(id=2) 在现有黄金语料里一个样本都没有，
            // 1.x 也未实现；等真出现这种镜像再补，否则就是凭猜写解码器。
            err = "SquashFS 旧 lzma（id=2）未实现：无样本可验证";
            return false;
        case Compression::Unknown:
            break;
    }
    err = "未知压缩 id=" + std::to_string(static_cast<unsigned>(comp));
    return false;
}

}  // namespace xrkit::engine::fs
