#pragma once
// xrkit::engine::fs —— LZHUF（(c)1989 Okumura）。
//
// 逐值照抄 1.x 的 `file_system/stalker_fs.py`：位流方向、Huffman 更新顺序
// （原版是 do-while，**不能**写成 while(c)）、以及四张常量表都必须一致 ——
// 错一格整包就废。常量表由 `lzhuf_tables.inc` 从 1.x 直接生成。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

std::vector<uint8_t> lzhuf_encode(const uint8_t* data, size_t size);

// 失败返回 false 并写 err。1.x 在这些情形返回 b""（输入 <4 字节、声明长度 0 或 >256MB、
// 码表越界）；调用方（DB 解析）与 1.x 一样把它们当成"解不出来"。
bool lzhuf_decode(const uint8_t* src, size_t size, std::vector<uint8_t>& out, std::string& err);

}  // namespace xrkit::engine::fs
