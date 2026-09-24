#pragma once
// xrkit::engine::font —— 字符集提取（M1-7 第⑦步）：1.x `extract_chars` 的等价实现。
//
// 语义（逐句照抄 `font_pack.extract_chars`）：
//   ① 起始集合 = `REQUIRED_CHARS` = `ASCII_CHARS` + 完整西里尔块 U+0410..U+04FF + `ANSI_BRIDGE`；
//   ② **递归**扫 `xml_dir` 下所有 `.xml`（原版 CharAdder 也是 `for /r` 递归）；
//   ③ 每个文件走**与出包同一条**编码判定链（`_decode_xml_bytes`：`sniff_encoding` → 严格解码
//      → `decode_bytes` 兜底），再解析出所有文本节点（`parse_xml_texts` 的三段式：单根 →
//      多根补根 → 去标签兜底）；
//   ④ 文本里剔除 `\r` `\n`、`cp < 0x20 且非 \t`、代理区，其余收进集合；
//   ⑤ 并入 `FONTGEN_EXTRA`；`full_cyrillic=false`（"复刻旧包"）再剔除 `LOST_CYRILLIC`；
//   ⑥ 返回**升序**去重的码位表（= 1.x 的 `"".join(sorted(chars))`），顺序即槽位顺序。
//
// ★ 为什么不能只扫顶层、也不能把编码写死：漏一个字符，引擎对缺失码位不是留空而是
//   `TCMap[i] = vFirstValid`（同一个重复字形）—— 满屏重复字形且日志里毫无痕迹。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::font {

struct CharsetResult {
    std::vector<uint32_t> chars;       // 升序去重（顺序即槽位顺序）
    int files_scanned = 0;             // 扫到的 .xml 文件数
    int files_undecodable = 0;         // 完全解不出来的（跳过）
    std::vector<std::string> guessed;  // 编码是"猜"的文件名（1.x 会汇总告警）
};

// `xml_dir` 不存在返回 false（1.x 返回 None）。
bool extract_chars(const std::string& xml_dir, bool full_cyrillic, CharsetResult& out);

// 供探针/复用：把码位表写成"每行一个十六进制码位"的文本（顺序即槽位顺序）。
std::string charset_to_lines(const std::vector<uint32_t>& chars);

}  // namespace xrkit::engine::font
