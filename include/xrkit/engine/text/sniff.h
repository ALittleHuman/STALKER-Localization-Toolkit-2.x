#pragma once
// xrkit::engine::text —— 编码**判定链**（B 链，编码转换页用）。
//
// 1.x `toolkit_textio.sniff_encoding` 的六步顺序，**不可改**：
//   1 BOM（物理事实）→ 2 多字节检查（能按 UTF-8 解且含 ≥0x80）→ 3 手动指定
//   → 4 XML 声明（**必须被字节证实**）→ 5 统计探测（＝"鲁棒"）→ 6 默认 windows-1251
//
// ★★ 2026-10-06 用户拍板的口径（四段原话，逐字遵守）：
//   ①「**单/多字节识别 > 手动 > 声明 > 鲁棒**。多字节编码**直接按 utf-8 读**，
//      单字节继续走剩下的。选择 auto 也只是**不包含手动**的那个环节而已。」
//   ②「鲁棒和声明实际上**地位差不多**，实际上更加准确的是**把声明读出来的也纳入置信度
//      的系统**。而且**我个人是非常不建议自动识别的**。**保留鲁棒**…毕竟还有不少文件
//      是没有声明的。」
//   ③「**字节判断和手动设置是死板的**，为了保证结果的稳定性。**后面的两个才是不确定
//      因素**，也就是一般意义上的**置信度系统**。」
//   ④「**声明的优先级，仍然是比鲁棒稍微高一点的**，因为大多数声明是正确的，鲁棒通常
//      来说更加不稳定。**前面两个死板的是锁**…**我仍然选择保留 auto 选项**。」
//
// ⇒ 落地形态（见 sniff.cpp 的 `single_byte_stat` + `sniff_encoding`）：
//   · 第 1/2/3 步是**锁**：物理事实 + 用户明示，语义一字不动（含 `auto` 仍保留）；
//   · 第 4/5 步合并成**一个置信度系统**：声明与"鲁棒"（单字节家族统计）**一起**裁决 ——
//     统计有决定性且与声明不一致时**否决声明**（`override_decl`，必须可见）；
//     统计无决定性时**声明照旧生效**（"大多数声明是正确的"）；两者都没有 ⇒ 第 6 步默认，
//     那是**纯猜**，必须打上 `low_confidence`（**零静默红线**）。
//
// ★ 这是**有意与 1.x 不同**：1.x 的第 4 步只要 `bytes_fit_encoding` 通过就 `return`，
//   而单字节编码下那道闸门**几乎是空的**（cp1251 只有 0x98 一个未定义字节）⇒
//   **谎报 `windows-1251` 的 cp1252 文件 100% 静默走错**。差异记在
//   `docs\2.0.0-行为口径对照表.md` ⑨ 组（附用户原话）与门禁 [16] 的台账里。
//
// ★ 第 5 步仍然**不是** chardet：探针表换成了"单字节家族的字节级语言统计剖面"
//   （`sniff_profiles.inc`，由 `tools/gen_sniff_profiles.py` 从**已发表字频**生成）
//   —— GBK/Big5/Shift-JIS/无 BOM 的 UTF-16 仍如实记账（规格文档 §5 差异台账）。

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

    // ── 置信度系统（2026-10-06 用户口径③④）──────────────────────────
    // "字节判断/手动"是锁 ⇒ 置信度恒 1；"声明/鲁棒"才是置信度系统的那两项。
    double confidence = 1.0;      // 0..1，见 sniff.cpp 的打分
    bool low_confidence = false;  // 纯猜（第 6 步默认）或无证据 ⇒ **必须可见**
    bool override_decl = false;   // 统计否决了 XML 声明 ⇒ **必须可见**
    uint64_t stat_evidence = 0;   // "鲁棒"步参与裁决的高位字节数（证据量）
    double stat_score = 0.0;      // 每个高位字节的平均对数似然比：正=像俄语(1251)，负=像西欧(1252)
};

// 9.7 的修正：1.x 那行"定案步骤"标签（`多字节(utf-8)`）是 convert_app **复算**的
// （它只查"有没有 XML 声明"、**不校验字节证实**），于是标签能与判定本身矛盾 ——
// 实测那句注释里就写着：字节是 UTF-8、声明写 windows-1251 的文件被标成 `声明(utf-8)`。
// 2.0 只发一个**步号**，标签由步号唯一决定，页面不许自己拼。
//
// 标签用词照 1.x：多字节 / 指定 / 声明 / 统计；`BOM` 与 `默认` 是 2.0 新加的两个
// 一等步骤（1.x 把 BOM 在进链之前就剥掉了、把默认并进了"统计"）—— 这是**有意分歧**，
// 因为 2.0 的链把这两步显式化了（见对照表 ⑨9.1 的顺序）。
const char* sniff_step_label(SniffStep step);

// 一行形态：`多字节(utf-8)` / `指定(windows-1251)` …（编码名为空时只给标签）
std::string sniff_result_label(const SniffResult& result);

// 判定结果里"**必须可见**"的那些标记（零静默红线），`、` 连接；没有标记返回空串。
// 现在是：`低置信`（纯猜/无证据）、`否决声明`（统计推翻了 XML 声明）、
//         `判定≠实解`（判定结论与实际使用编码分叉，由调用方补）。
// ★ 文案**只此一份**：CLI 与 GUI 都调它，页面不许自己拼（⑨9.7 的同一条纪律）。
std::string sniff_flags_label(const SniffResult& result);

// ★★ P17-87（待办总表 §四·9）：**问句原文**（唯一一份；GUI 弹窗与 CLI 报出都用它）。
//   内容＝"要不要按这个判定转 ＋ 文件 ＋ 判定标签 ＋ 理由 ＋ 标记 ＋ 置信度"（四件缺一不可）。
//   ★ 文案只此一份：`sniff.cpp` 之外不许自己拼（与 `sniff_flags_label` 同一条纪律）。
std::string sniff_confirm_question(const SniffResult& result, const std::string& path);

struct DecodedBytes {
    bool ok = false;
    std::string text;           // UTF-8（与 1.x decode_bytes 的 str 对应）
    std::string used_encoding;  // 实际解成功的编码
    bool forced = false;        // 是否落到 latin-1 保底
    uint64_t chars = 0;
    std::string error;
    // ★ 2026-10-06：判定结论（`SniffResult::encoding`）与实际解成功的编码**分叉** ——
    //   实测量到过 `enc=windows-1251` 但 `used=windows-1252` 的静默分叉。必须报出来。
    bool used_mismatch = false;
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
