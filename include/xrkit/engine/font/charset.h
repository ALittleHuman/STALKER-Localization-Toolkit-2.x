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
#include <set>
#include <string>
#include <vector>

namespace xrkit::engine::font {

struct CharsetResult {
    std::vector<uint32_t> chars;       // 升序去重（顺序即槽位顺序）
    int files_scanned = 0;             // 扫到的 .xml 文件数
    int files_undecodable = 0;         // 完全解不出来的（跳过）
    // ★★ 2026-10-07（审计补充 A1-4）：**打不开**的文件数 —— 与"空文件"必须分开。
    //   修前读文件助手打不开就 `return {}`，而空字节解码**必然成功** ⇒ 整份语料的字符
    //   被静默丢弃而 `files_scanned` 照涨。开不了的文件**必须计入失败**（调用方据此拒绝
    //   出包/明确报错），不许静默当成空文件。
    int files_unreadable = 0;
    // 枚举目录失败这类**硬失败**的第一手理由（配合 extract_chars 返回 false）。
    std::string error;
    std::vector<std::string> guessed;  // 编码是"猜"的文件名（1.x 会汇总告警）
};

// `xml_dir` 不存在返回 false（1.x 返回 None）。
// 用户 2026-09-26 决策：**日文层砍掉** ⇒ 假名不进字符集。
// 剔的码位：平假名/片假名 U+3040–U+30FF、片假名扩展 U+31F0–U+31FF、半角片假名 U+FF66–U+FF9D。
// ★ "日文汉字不进"**不能**靠剔 CJK 码位实现 —— 日文汉字与中文汉字码位相同，剔了就把译文
//   的汉字一起剔光。那条只能靠"不加日语专用字形层、译文里出现的汉字照收"来落实。
void strip_japanese_kana(std::set<uint32_t>& chars);

bool extract_chars(const std::string& xml_dir, bool full_cyrillic, CharsetResult& out);

// 1.x `REQUIRED_CHARS` = ASCII/俄文基础 + 完整西里尔块 U+0410..U+04FF + ANSI 桥接（升序去重）。
// 出包时的"字体缺必需字形"告警用它（不是全字符集）。
std::vector<uint32_t> required_chars();

// 供探针/复用：把码位表写成"每行一个十六进制码位"的文本（顺序即槽位顺序）。
std::string charset_to_lines(const std::vector<uint32_t>& chars);

}  // namespace xrkit::engine::font
