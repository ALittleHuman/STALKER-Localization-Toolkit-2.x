#include "xrkit/engine/fs/compress.h"

#include <cstring>
#include <string>

#include <lzma.h>

namespace xrkit::engine::fs {
namespace {

// 参照实现的参数（squashfs-tools-ng 1.3.2 / libsquashfs comp/xz.c）：
//   lzma_lzma_preset(&opt, level=6) → 再覆盖 lc/lp/pb/dict_size
//   lzma_stream_buffer_encode(filters={LZMA2}, LZMA_CHECK_CRC32, ...)
constexpr uint32_t kXzLevel = 6;
constexpr uint32_t kXzLc = 3;
constexpr uint32_t kXzLp = 0;
constexpr uint32_t kXzPb = 2;
constexpr uint32_t kXzDictSize = 131072;  // = block_size（xz 默认字典就是块大小）

}  // namespace

bool compress_block(Compression comp, const uint8_t* src, size_t src_len, size_t out_capacity,
                    std::vector<uint8_t>& out, bool& compressed, std::string& err) {
    compressed = false;
    out.clear();
    if (src == nullptr || src_len == 0 || out_capacity == 0) {
        err = "压缩参数非法（输入或容量为 0）";
        return false;
    }

    switch (comp) {
        case Compression::Xz: {
            lzma_options_lzma opt;
            if (lzma_lzma_preset(&opt, kXzLevel)) {
                err = "lzma_lzma_preset 失败";
                return false;
            }
            opt.dict_size = kXzDictSize;
            opt.lc = kXzLc;
            opt.lp = kXzLp;
            opt.pb = kXzPb;

            lzma_filter filters[2];
            filters[0].id = LZMA_FILTER_LZMA2;
            filters[0].options = &opt;
            filters[1].id = LZMA_VLI_UNKNOWN;
            filters[1].options = nullptr;

            std::vector<uint8_t> buf(out_capacity);
            size_t written = 0;
            const lzma_ret rc = lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC32, nullptr, src,
                                                          src_len, buf.data(), &written,
                                                          buf.size());
            // ★ 与参照实现同语义：LZMA_BUF_ERROR（输出放不下）也算"不值得压"，
            //   不当作错误 —— 上层会原样存储。真错误（内存等）才返回 false。
            if (rc == LZMA_BUF_ERROR) {
                return true;
            }
            if (rc != LZMA_OK) {
                err = "xz 压缩失败（code=" + std::to_string(static_cast<int>(rc)) + "）";
                return false;
            }
            if (written >= src_len) {
                return true;  // 压不小 → 调用方原样存
            }
            buf.resize(written);
            out = std::move(buf);
            compressed = true;
            return true;
        }
        case Compression::Gzip:
        case Compression::Lz4:
        case Compression::Zstd:
        case Compression::Lzo:
        case Compression::Lzma:
            // 口径：**不写没法验证的代码**。1.x 的封包只产出 xz（tar2sqfs 默认），
            // 因此这几种没有"逐字节一致"的参照基线，等有了基线再逐个补。
            err = std::string("封包暂只支持 xz（无参照基线）：") + compression_name(comp);
            return false;
        case Compression::Unknown:
            break;
    }
    err = "未知压缩 id=" + std::to_string(static_cast<unsigned>(comp));
    return false;
}

}  // namespace xrkit::engine::fs
