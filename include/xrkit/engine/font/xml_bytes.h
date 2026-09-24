#pragma once
// xrkit::engine::font —— XML 字节的两个共用小工具（判定链 + 声明改写）。
//
// 1.x 里这两件事分别出现在 `font_pack._decode_xml_bytes` 与 `build_package` 的 XML 循环里；
// 字库模块有两处要用（字符集提取、出包时的 XML 转写），所以单独抽出来，避免两处各写一遍。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::font {

// 1.x `_decode_xml_bytes`：XML 字节 → (UTF-8 正文, 编码名, 可信)。
// 可信=false 表示编码只是猜的（调用方**必须告警**，不许静默）。
// 走与出包同一条链：`sniff_encoding`（B 链）→ 严格解码 → `decode_bytes` 兜底。
bool decode_xml_bytes(const std::vector<uint8_t>& raw, std::string& text, std::string& enc,
                      bool& trusted);

// 1.x `build_package` 里的声明改写：把**第一条** `<?xml ...?>` 的 `encoding="..."` 换成
// utf-8（大小写不敏感，其余字节原样）。找不到声明就原样返回。写盘时**不加 BOM**、
// **不做换行翻译**（1.x 用 `newline=""`）。
std::string rewrite_xml_declaration_encoding(const std::string& text);

}  // namespace xrkit::engine::font
