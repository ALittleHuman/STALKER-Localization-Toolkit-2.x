#include "xrkit/engine/fs/compress.h"

#include <cstring>
#include <string>

#include <lzma.h>

// ★★ T-07（第三方只读审计 2026-10-18；参数按用户 2026-10-19 给的参照实现）：
//   gzip 块 = `deflateInit2(level=9, Z_DEFLATED, windowBits=15, memLevel=8, Z_DEFAULT_STRATEGY)`
//   ＋ `deflate(Z_FINISH)`；**不写** options 块。zlib 1.2.12（参照）与 1.3.2 在本机
//   实测逐字节一致（`tests/probe_zlib_deflate_bytes.py`，3/3）。
#include <zlib.h>

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
        case Compression::Gzip: {
            // ★ T-07：参照 squashfs-tools-ng 1.3.2 的 gzip 参数（用户钉死）。
            //   ★ 契约与 xz 相同：**压完不小于原块就不算压**（gzip/zstd/lzo 都是这个口径；
            //     lz4 不是 —— 那是另一件事，见审计文档）。
            z_stream zs{};
            if (deflateInit2(&zs, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                err = "deflateInit2 失败";
                return false;
            }
            std::vector<uint8_t> buf(out_capacity);
            zs.next_in = const_cast<Bytef*>(src);
            zs.avail_in = static_cast<uInt>(src_len);
            zs.next_out = buf.data();
            zs.avail_out = static_cast<uInt>(buf.size());
            const int rc = deflate(&zs, Z_FINISH);
            const size_t produced = buf.size() - zs.avail_out;
            deflateEnd(&zs);
            if (rc != Z_STREAM_END) {
                err = "deflate(Z_FINISH) 未收尾";
                return false;
            }
            if (produced < src_len) {
                buf.resize(produced);
                out = std::move(buf);
                compressed = true;
            } else {
                compressed = false;   // 不小于原块 ⇒ 原样存（与参照一致）
            }
            return true;
        }
        case Compression::Lz4:
        case Compression::Zstd:
        case Compression::Lzo:
        case Compression::Lzma:
            // 口径：**不写没法验证的代码**。
            //   · lzo：仓库 vendored 的是 minilzo，只有 `lzo1x_1_compress`，而参照默认是
            //     `lzo1x_999` ⇒ 要换完整 lzo2（依赖替换，不是接线）；
            //   · lz4：参照**无条件**写 8 字节 options 块并置 `COMPRESSOR_OPTIONS` 标志
            //     （数据区起点/bytes_used/所有表偏移都要动），且"不可压"语义与我们契约相反；
            //   · zstd / lzma：参照基线还没做进探针。
            err = std::string("封包暂只支持 xz / gzip（其余无参照基线或需改布局）：") +
                  compression_name(comp);
            return false;
        case Compression::Unknown:
            break;
    }
    err = "未知压缩 id=" + std::to_string(static_cast<unsigned>(comp));
    return false;
}

}  // namespace xrkit::engine::fs
