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
            uint64_t memlimit = UINT64_MAX;
            size_t in_pos = 0;
            size_t out_pos = 0;
            const lzma_ret rc = lzma_stream_buffer_decode(&memlimit, 0, nullptr, src, &in_pos,
                                                          src_len, out.data(), &out_pos,
                                                          out_capacity);
            if (rc != LZMA_OK) {
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
