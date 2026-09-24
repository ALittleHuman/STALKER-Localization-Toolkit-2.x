#pragma once
// xrkit::engine::fs —— SquashFS 块压缩（自实现，零外部 exe）。
//
// 判据（M1-5，用户拍板"产物逐字节一致"）：产出必须与 1.x 的封包工具
// tar2sqfs 1.3.2 **逐字节相同**。所以参数不是"挑一组合理的"，而是从参照实现里
// 逐项校准出来的（证据见 docs/2.0.0-SquashFS-writer规格.md）：
//
//   · xz：lzma_lzma_preset(6) 之后**覆盖** dict_size=131072、lc=3、lp=0、pb=2；
//     check = CRC32；过滤器链只有 LZMA2。
//   · **必须走 lzma_stream_buffer_encode（单次、整块）**：它会在块头里写
//     compressed_size / uncompressed_size 两个字段；流式 API（lzma_stream_encoder）
//     不写，产物会少 4 字节 —— 实测参照镜像就是前者（664 vs 660）。
//   · 输出上限 = 调用方给的上限（数据块/尾块 = block_size，元数据块 = 8192）。

#include "xrkit/engine/fs/decompress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

// 尝试压缩一块。
//   · 返回 true + compressed=true  → out 是压缩结果（长度一定 < src_len）
//   · 返回 true + compressed=false → 压缩结果不比原数据小（或输出放不下），
//                                    调用方应**原样存储**（SquashFS 的 bit24 语义）
//   · 返回 false                   → 真出错，err 带可读原因
bool compress_block(Compression comp, const uint8_t* src, size_t src_len, size_t out_capacity,
                    std::vector<uint8_t>& out, bool& compressed, std::string& err);

}  // namespace xrkit::engine::fs
