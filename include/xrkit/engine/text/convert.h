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
#include <functional>
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

// ★★ P17-87（待办总表 §四·9，用户 2026-10-10 拍板）：**编码判定一律要问，不许自动模式**。
//
// 口径（用户原话「全部上模态对话框。谁让你这么喜欢用自动模式的？」）：
//   · **GUI（有窗口宿主）**：每一次编码判定都上**模态对话框**、由用户显式确认；
//     **不设**"页内建议／自动通过／静默按 auto 继续"；用户**取消或关掉＝不继续**；
//   · **CLI / headless**：没有窗口可弹 ⇒ **不许猜** —— 把问句与诊断**原样报出**并**非零退出**；
//   · **只对 `auto` 触发**：手动指定了源编码（`ConvertOptions::source_encoding != "auto"`）
//     ＝用户已经显式决定过，**不再问**（否则"选择"这件事本身就没意义了）。
//
// ★ 为什么钩子放在**引擎**而不是页面里（P17-86 先查件的结论）：引擎不许碰 Qt
//   （门禁 `no-qt` 步 + `check_no_qt.cmake` 锁着），而**页面那条路不覆盖批处理/CLI**
//   ⇒ 只有"宿主注入一个回调"这一种形状能同时满足两侧。
//   钩子**不注入**（默认）＝"没有窗口可弹"⇒ 引擎返回 `needs_confirm` 让调用方报出＋非零退出。
enum class EncodingConfirmOutcome {
    Confirmed,    // 用户确认按判定的编码继续
    Overridden,   // 用户显式改成了别的源编码（`chosen_encoding` 非空）
    Cancelled,    // 用户取消/关掉对话框 ⇒ **这一份不转**（不是"静默继续"）
};

struct EncodingConfirmRequest {
    std::string path;            // 这一份是哪来的（批量时逐文件；用户要看得见"在问哪一份"）
    std::string encoding;        // 判定胜出的编码名
    std::string step_label;      // 形如 `多字节(utf-8)`（文案来自 sniff.cpp，页面不许拼）
    std::string flags_label;     // `低置信` / `否决声明`（同上）
    double confidence = 1.0;
    std::string question;        // **问句原文** —— 由 `sniff.cpp` 拼（唯一一份文案）
};

// 返回 `Confirmed`/`Cancelled`；`Overridden` 时把用户选的编码写进 `chosen_encoding`。
using EncodingConfirmFn =
    std::function<EncodingConfirmOutcome(const EncodingConfirmRequest&, std::string* chosen_encoding)>;

// 宿主注入确认器（GUI 装配时注入"弹模态"那个；CLI/headless **不注入**）。
// 传 `nullptr` = 撤销（回到"没有窗口可弹"）。
void set_encoding_confirm(EncodingConfirmFn fn);
// 当前有没有注入（判据与 CLI 用它决定"能不能问"）
bool has_encoding_confirm();

struct ConvertOutcome {
    bool ok = false;
    bool skipped = false;  // 源已是目标编码；可能仍写了盘（对齐声明/BOM）
    bool wrote = false;
    // ★★ P17-87：**"需要用户确认才能继续"** —— 这一支既不是成功也不是报错：
    //   没有窗口可弹时（CLI/headless）调用方必须把 `confirm_question` 与诊断**原样报出**
    //   并**非零退出**；`data` 为空（**一个字节都没写**）。
    bool needs_confirm = false;
    std::string confirm_question;   // 问句原文（`needs_confirm` 时非空）
    std::string confirm_diagnostics;// 诊断原文（step / 理由 / 标记 / 置信度，逐行）
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
    // ★★ T-08（第三方只读审计 2026-10-18）：**"用户在编码确认里点了取消"是一等公民的事实**，
    //   不许靠 `error` 的文案去认（那是 C-02 那类"改文案＝改语义"的老坑）。
    //   批处理靠它**停下整批**（修前：取消只否决当前这一份，后面 499 个照样逐个弹窗）。
    bool user_cancelled = false;
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
