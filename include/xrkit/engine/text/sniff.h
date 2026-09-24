#pragma once
// xrkit::engine::text —— 编码**判定链**（B 链，编码转换页用）。
//
// 1.x `toolkit_textio.sniff_encoding` 的六步顺序，**不可改**：
//   1 BOM（物理事实）→ 2 多字节检查（能按 UTF-8 解且含 ≥0x80）→ 3 手动指定
//   → 4 XML 声明（**必须被字节证实**）→ 5 统计探测 → 6 默认 windows-1251
//
// ★ 第 5 步（统计探测）我们只实现**确定性且已实测对齐**的那一部分：
//   `chardet` 在"纯 ASCII"上给 `ascii(1.000)`，而这条判定可以用字节规则逐项复刻
//   （见 sniff.cpp 的 ascii_like）。其余家族（GBK/Big5/Shift-JIS/无 BOM 的 UTF-16…）
//   需要移植 chardet 的探针才能对齐，**本轮如实记账**（规格文档 §5 + 门禁的差异台账）。
//   不许不声不响地"自己发明一个启发式"：那会让真实俄文文件被误判。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::text {

enum class SniffStep {
    Bom = 1,
    MultibyteUtf8 = 2,
    Override = 3,
    XmlDeclaration = 4,
    Statistic = 5,
    Default = 6,
};

struct SniffResult {
    SniffStep step = SniffStep::Default;
    std::string encoding;  // 胜出的编码名（与 Python 写法一致，日志原样用）
    std::string reason;    // 人类可读的胜出理由（日志直接用返回值，1.x 是 convert_app 复算）
};

struct DecodedBytes {
    bool ok = false;
    std::string text;           // UTF-8（与 1.x decode_bytes 的 str 对应）
    std::string used_encoding;  // 实际解成功的编码
    bool forced = false;        // 是否落到 latin-1 保底
    uint64_t chars = 0;
    std::string error;
};

// BOM → 编码名；无 BOM 返回空串。UTF-8 的 BOM 归一为 "utf-8"（1.x 口径：
// 返回 utf-8-sig 会让"判定出的编码"与"实际用的编码"对不上）
std::string bom_encoding(const std::vector<uint8_t>& raw);

// 六步判定链；override_enc 为空或 "auto" 表示不手动指定
SniffResult sniff_encoding(const std::vector<uint8_t>& raw,
                           const std::string& override_enc = "auto");

// 判定 + 解码（= 1.x `decode_bytes` 的完整语义，含宽字符 BOM 与 latin-1 保底）
DecodedBytes decode_bytes(const std::vector<uint8_t>& raw,
                          const std::string& override_enc = "auto");

// 读文件版
DecodedBytes read_text_bytes(const std::string& path,
                             const std::string& override_enc = "auto");

}  // namespace xrkit::engine::text
