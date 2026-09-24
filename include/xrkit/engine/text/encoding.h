#pragma once
// xrkit::engine::text —— 编码解码/编码（进程内自实现，零外部依赖）。
//
// **1.x 有两条不同的链，本文件实现的是"六编码链"那条**（文本提取页用的）：
//   `toolkit_textio.read_text_file`：utf-8-sig → utf-8 → gb18030 → windows-1251
//   → windows-1252 → latin-1，前五种**严格**解码，末位 latin-1 兜底。
// （另一条是 `sniff_encoding` 的六步判定链，属编码转换页，见 docs/2.0.0-文本模块规格.md §5）
//
// 严格性不是细节：cp1251 的 0x98、cp1252 的 0x81/0x8D/0x8F/0x90/0x9D 在 Python 里
// 都会抛 UnicodeDecodeError。若把它们当 U+FFFD 放行，"windows-1251"这一步会**错误地
// 解成功**，整条链的落点就从 latin-1 变成 cp1251 —— 解码结果直接变。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::text {

// 六编码链里出现的编码名（与 1.x/Python 的写法一致，日志里要原样出现）
inline constexpr const char* kUtf8Sig = "utf-8-sig";
inline constexpr const char* kUtf8 = "utf-8";
inline constexpr const char* kGb18030 = "gb18030";
inline constexpr const char* kWindows1251 = "windows-1251";
inline constexpr const char* kWindows1252 = "windows-1252";
inline constexpr const char* kLatin1 = "latin-1";
// 判定链（B 链）还会用到的名字
inline constexpr const char* kAscii = "ascii";
inline constexpr const char* kUtf16 = "utf-16";
inline constexpr const char* kUtf16Le = "utf-16-le";
inline constexpr const char* kUtf16Be = "utf-16-be";
inline constexpr const char* kUtf32 = "utf-32";
inline constexpr const char* kUtf32Le = "utf-32-le";
inline constexpr const char* kUtf32Be = "utf-32-be";
inline constexpr const char* kGbk = "gbk";
inline constexpr const char* kGb2312 = "gb2312";

struct DecodeResult {
    bool ok = false;
    std::string text;      // UTF-8 正文（utf-8-sig 命中时 BOM 已被吃掉）
    std::string encoding;  // 实际使用的编码名（1.x 的 read_text_file 会把它当"成功编码"返回）
    std::string error;
    uint64_t chars = 0;    // Unicode 码位数（与 Python len(text) 对拍用）
};

// 单个编码的严格解码。失败返回 false（err 带原因），**不产生部分结果**。
bool decode_strict(const std::string& encoding, const uint8_t* data, size_t size,
                   std::string& text, std::string& err);

// 单个编码的严格编码（提取阶段要把改写后的正文按源编码写回）。
// 出现该编码表示不了的码位时返回 false —— 与 Python 的 UnicodeEncodeError 同语义，
// 上层照 1.x 的做法把整个文件记为 SKIP，**不许静默替换**。
bool encode_strict(const std::string& encoding, const std::string& text,
                   std::vector<uint8_t>& out, std::string& err);

// 六编码链（= 1.x read_text_file 的字节级语义；latin-1 兜底所以永不失败）
DecodeResult decode_text_chain(const std::vector<uint8_t>& raw);

// 读文件 → 六编码链 → **通用换行归一**。这就是 1.x `read_text_file(path)` 的完整语义：
// Python 文本模式默认 newline=None，会把 \r\n 与 \r 都翻译成 \n。
// ★ 漏了这一步，"编码名对了、正文也对了"的假象下，凡是用 \r\n 的源文件都会对不上。
DecodeResult read_text_file(const std::string& path);

// 通用换行归一（\r\n → \n，单独的 \r → \n）
void normalize_newlines(std::string& text);

// 读一个 UTF-8 码位（严格；失败返回 false 且 pos 不前进）。
// 文本层（提取）要按码位做 strip / 切分 / 判定，所以这里把码位迭代暴露出来。
bool utf8_next(const uint8_t* data, size_t size, size_t& pos, uint32_t& cp);

// 追加一个码位（UTF-8 编码）
void utf8_append(std::string& out, uint32_t cp);

// UTF-8 串的码位数（不合法返回 0）
uint64_t utf8_codepoint_count(const std::string& utf8);

}  // namespace xrkit::engine::text
