#pragma once
// xrkit::engine::text —— 编码转换（编码转换页的业务层，⑨ 链的"下游"）。
//
// 逐条照 1.x `apps/convert_app.convert_file` 的**代码**（不是文档）：
//   · 入口先剥 BOM，但**剥掉不等于忘掉** —— "源带 UTF-8 BOM"这个事实要用标志传下去，
//     因为输出侧一律不带 BOM（BOM 只对 utf-8 合法，留在 cp1251 文件里是脏字节）；
//   · 解码用 `errors="replace"`（**不是严格解码**）：解不出来的字节变 U+FFFD，不抛错；
//   · 写盘用 `newline=""` ⇒ **不做任何换行翻译**（Python 文本模式默认会把 \n 变 \r\n，
//     这里刻意关掉）；
//   · 目标编码用 `errors="replace"`：目标编码表示不了的码位写成 `?`（与 Python 同语义）；
//   · `.xml` 走 `_fix_xml_decl`：统一双引号 + **只替换已存在的 encoding 属性**；
//   · 源编码 == 目标编码时走**对齐分支**（见 `needs_rewrite`），不是简单地不写。
//
// 判据纪律：这里的每个"照抄"都要能用 Python 同名行为对拍（见 `tests/golden_lossy_codec.py`），
// 对不上的进差异台账，不许"看着差不多"。

#include "xrkit/engine/text/sniff.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::text {

// ── 两个纯字符串口径（1.x 的同名函数）─────────────────────────────────

// `_fix_xml_decl`：把 XML 声明规范化 —— 统一双引号 + 把 encoding 值改写成目标编码，
// **保留其它属性**；只处理**第一条** `<?xml ... ?>`；**没有 encoding 属性就不补写**
// （转换分支与"已是目标编码"分支共用它，两处口径必须一致）。
std::string fix_xml_decl(const std::string& content, const std::string& target_enc);

// `_needs_rewrite`：源已是目标编码时，文件是否**仍然必须重写**。两种情况要写：
//   ① 源带 UTF-8 BOM（输出侧一律不带 BOM）；
//   ② XML 声明的 encoding 值与目标编码不符（否则"转成 utf-8 之后声明不变"）。
// 没有 encoding 属性 ⇒ 第 ② 条为 False（与 `fix_xml_decl` 不补写的口径一致）。
bool needs_rewrite(const std::vector<uint8_t>& raw_after_bom, const std::string& target_enc,
                   bool had_utf8_bom);

// ── 有损解码/编码在 `encoding.h` ─────────────────────────────────────
// （`errors="replace"` 是编解码层的语义，不属于"转换流程"；放在那里才能和
//  严格解码共用同一批表，也才方便被 golden 对拍单独驱动。）

// ── 转换 ─────────────────────────────────────────────────────────────

struct ConvertOptions {
    std::string target_encoding = "utf-8";
    std::string source_encoding = "auto";  // "auto" = 走判定链；否则是手动指定（第 3 步）
    bool to_output_dir = false;            // 输出到新目录 ⇒ **必须产出文件**
};

struct ConvertOutcome {
    bool ok = false;
    bool skipped = false;  // 源已是目标编码；可能仍写了盘（对齐声明/BOM）
    bool wrote = false;
    SniffStep step = SniffStep::Default;
    std::string step_label;       // 形如 `多字节(utf-8)` —— **来自判定结果**（⑨9.7）
    std::string source_encoding;  // 判定链/手动指定给出的编码名
    // ── 置信度系统的"必须可见"部分（2026-10-06 用户口径③"零静默"）──────────
    // 页面/批处理只**显示**它们，不许自己拼结论（文案只有 sniff.cpp 一份）。
    std::string flags_label;      // 引擎给的标记串：`低置信` / `否决声明`（可同时有）
    double confidence = 1.0;
    bool low_confidence = false;
    bool override_decl = false;
    // ★ 判定结论（`source_encoding`）与实际**真正用来解码**的编码分叉 —— 必须报出来。
    //   转换路径原来在这里有一处**静默回退**：判定编码 `decode_lossy` 失败就悄悄改用
    //   windows-1251（1.x 也这么做，但 1.x 至少记一条警告）。现在既记 flag 也计数。
    std::string used_encoding;
    bool used_mismatch = false;
    uint64_t in_bytes = 0;
    uint64_t out_bytes = 0;
    std::vector<uint8_t> data;  // 要写出的内容（wrote=false 时为空）
    std::string error;
};

// 转换一段字节。`rel_path` 只用来判断"是不是 .xml"（其余与内容无关）。
// `outcome.data` 是要写出的内容（`wrote=false` 时为空）。
ConvertOutcome convert_bytes(const std::vector<uint8_t>& raw, const std::string& rel_path,
                             const ConvertOptions& options);

// 读文件 → 转换 → 写文件（原子写：同目录 `.part` → 复核 → rename）。
// `out_path` 为空 ⇒ **原地**：此时只有 `needs_rewrite` 为真才写盘。
ConvertOutcome convert_file(const std::string& path, const std::string& out_path,
                            const ConvertOptions& options);

}  // namespace xrkit::engine::text
