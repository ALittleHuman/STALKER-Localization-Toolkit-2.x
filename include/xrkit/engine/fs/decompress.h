#pragma once
// xrkit::engine::fs —— SquashFS 块解压。
//
// 块语义（**没有容器头**，别想当然）：
//   gzip = zlib 流、xz = xz 流、zstd = zstd 帧、lz4 = **块格式**（无帧头）、lzo = lzo1x 原始
// 实现全部进程内（不调外部 exe）—— 1.x 的血账：外部工具会被 360 按哈希拦。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

enum class Compression : uint16_t {
    Unknown = 0,
    Gzip = 1,
    Lzma = 2,  // 旧 lzma：见下方说明
    Lzo = 3,
    Xz = 4,
    Lz4 = 5,
    Zstd = 6,
};

const char* compression_name(Compression c);

// 解压一个块。out_capacity 是**输出上限**（元数据块恒 8192，数据块为 superblock.block_size）；
// 成功后 out 被缩放到实际长度。返回 false 时 err 带可读原因。
bool decompress_block(Compression comp, const uint8_t* src, size_t src_len, size_t out_capacity,
                      std::vector<uint8_t>& out, std::string& err);

}  // namespace xrkit::engine::fs
