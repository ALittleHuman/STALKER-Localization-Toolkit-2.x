#pragma once
// xrkit::engine::patch —— 二进制补丁的**通用机器**（P16-1b 判定半；Qt-free）。
//
// 为什么是"通用机器 + 表由调用方给"（用户 2026-10-09 已过目的设计笔记 §二，方案 (b)）：
//   · 1.x `engine_utf8_patch.py` 的表本身就是"**收录一处就能补一处，引擎代码一行都不用改**"；
//   · 表是**单一构建的特化数据**（1.x 原话："换一个模组、换一次引擎编译，这些数字就全部作废"）；
//   · 本仓红线：不许拿代理指标/特化数据当语义判据 ⇒ 构建相关数据不焊进内核。
//   ⇒ 引擎负责"**怎么做**"（PE 布局守卫 / 逐项判态 / 指纹状态机 / 意图写入 / 字节 diff），
//     插件负责"**做什么**"（给表、给路径、要结论）。
//
// ★★ 判定纪律（1.x 血账，用户红线）：
//   结论**只**来自逐项字节内容（expect / patched / 周边锚 / thunk 完整性 / 空闲区是否全零），
//   **绝不看"差了多少字节"** —— 实测 v3 与 v4 的差异字节数完全相同（都是 88），
//   字节数既分不出正确/缺陷形态，也证明不了打对了位置。`reference_diff_bytes` 只作报告参考值。
//
// ★ 二进制字段一律走 **hex 文本**（ASCII 安全）：pybind11 的 `std::string` 走 UTF-8，
//   直接传裸字节会被改坏。解析时大小写不敏感、允许空格分隔。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::patch {

// 1.x 的常量（照抄；`kRvaDelta` 是"文件偏移 → RVA"的换算基准，本构建为 0xC00）
inline constexpr uint64_t kRvaDelta = 0xC00;
// 缺陷版（v1/v2/v3）把 helper 区整体前移的字节数 —— **只用于识别**，绝不是生成目标
inline constexpr int kDefectShift = -2;

// ── 目录项（一条 = 一处要补的地方）───────────────────────────────────────
//   kind: "overwrite"（整段换）| "call"（等长改写成 `call helper`）|
//         "helper"（helper 机码落位，落点是空闲填充）| "preserve"（必须逐字节不变）
struct PatchSite {
    std::string id;
    std::string kind;
    uint64_t offset = 0;
    std::string expect_hex;
    std::string patched_hex;
    std::string before_hex;   // 周边锚（可空）：offset 之前的真实邻域
    std::string after_hex;    // 周边锚（可空）：expect 之后的真实邻域
    std::string helper;       // kind=call：指向哪个 helper（`helper` 项 id 的点后段，如 "A"）
    std::string tail_hex;     // kind=call：等长改写后原样保留的原有指令字节
    std::string note;         // 报告里的人类可读说明
    // 目录项**显式**声明的布局要求（0/空 = 不检查；声明了就变成硬条件）
    uint64_t layout_delta = 0;
    std::string layout_section;
};

// ── 逐项判态 ────────────────────────────────────────────────────────────
//   state: "orig"（原版未打）| "patched"（终态已打）| "kept"（preserve 项保持未变）|
//          "broken"（preserve 项被动过）| "other"（既非原版也非终态）|
//          "context-mismatch"（周边锚不符）| "out-of-range"（超出文件范围）
struct PatchItemState {
    std::string id;
    std::string kind;
    uint64_t offset = 0;
    std::string state;
    std::string expect_hex;
    std::string patched_hex;
    std::string actual_hex;
    std::string context_problem;
    int64_t call_target = -1;   // kind=call 且该处是 `E8 rel32` 时：指向的**文件偏移**
    int64_t call_target_defect = -1;  // 同上，但按缺陷落位（helper 前移）算 —— 只用于识别
    std::string note;
};

// ── PE 布局守卫 ─────────────────────────────────────────────────────────
struct PatchLayout {
    bool parsed = false;          // PE 能不能解析
    std::string error;
    uint16_t machine = 0;
    uint16_t magic = 0;
    std::string text_name;
    uint64_t text_va = 0;
    uint64_t text_rawptr = 0;
    uint64_t text_rawsize = 0;
    uint64_t text_delta = 0;      // VA - RawPtr
    bool delta_is_expected = false;      // == kRvaDelta（**只是提示**）
    bool targets_inside_text = false;    // 目标偏移是否都落在 .text 原始数据内
    uint64_t touch_lo = 0, touch_hi = 0; // 本目录会碰到的偏移范围（hi<lo 表示目录为空）
    std::string note;             // 人类可读的一行（照 1.x 的措辞）
    bool hard_ok = false;         // 硬条件（解析成功 + x64 + PE32+ + 显式 layout 声明）
};

// ── 识别结果 ────────────────────────────────────────────────────────────
struct PatchIdentifyResult {
    bool ok = false;
    std::string error;
    std::string verdict_key;    // "orig" | "v4" | "v2" | "v1v3" | "unknown"
    std::string verdict_text;   // "原版" / "v4(正确)" / "v2(有缺陷)" / "v1 或 v3(有缺陷)" / "未知"
    std::string detail;         // 说明（照 1.x 的措辞）
    std::vector<PatchItemState> items;
    int n_orig = 0;
    int n_patched = 0;
    int n_kept = 0;
    int n_broken = 0;
    int n_other = 0;
    int n_context = 0;
    int n_range = 0;
    bool thunk_intact = false;
    std::string thunk_actual_hex;
    bool cave_all_zero = false;
    uint64_t cave_lo = 0, cave_hi = 0;
    uint64_t file_size = 0;
    uint64_t reference_diff_bytes = 0;   // ★ 只作报告参考值，**判定不看它**
    int64_t delta = static_cast<int64_t>(kRvaDelta);
    PatchLayout layout;
};

// ── 十六进制工具（对外暴露，判据/插件都用同一把尺子）──────────────────────
std::string bytes_to_hex(const std::string& raw);                 // "41 8B C6"
std::string hex_to_bytes(const std::string& hex, bool* ok = nullptr);  // 空格/大小写不敏感
std::string hex_short(const std::string& raw, size_t n = 16);     // 报告用（超长截断并标长度）

// ── 主体 ────────────────────────────────────────────────────────────────
// 解析 PE 布局并跑硬/软条件（`sites` 用来算 touch_span 与显式 layout 声明）
PatchLayout parse_layout_buffer(const std::string& data, const std::vector<PatchSite>& sites);
// 逐项判态 + 指纹状态机（**不写盘**）
PatchIdentifyResult identify_buffer(const std::string& data, const std::vector<PatchSite>& sites);
// 从文件读入再识别（读不到/太短 ⇒ ok=false + 可见理由）
PatchIdentifyResult identify_file(const std::string& path, const std::vector<PatchSite>& sites);

// ── P16-1b-2 写盘半 ─────────────────────────────────────────────────────
// 「生成」＝ 纯内存跑完**全部写前守卫** → 生成补丁镜像 → 做后置自检。
// ★ 任何一条守卫不满足都只返回 ok=false + 逐条 reason/notes，**不产生任何副作用**。
//
// 守卫链（全部沿目录逐项走 ⇒ 收录一处就自动多守一处）：
//   ① PE 布局（节表 + 目标偏移都落在 .text 原始数据内 + 目录项显式 layout 声明）
//   ② preserve 项（不许动的地方）在输入里本来就完好
//   ③ 每一项的 expect 逐字节命中（"按需打补丁"的前提；已是终态要单独说明）
//   ④ 每一项声明的周边锚命中（防单字节碰撞 / 防打错位置）
//   ⑤ helper 占的空闲区全零，**且起点不落在前一条指令的跨度内**（v1/v2/v3 的翻车点）
//   ⑥ call 项自洽：按 RVA 算出的 `E8 rel32` 必须逐字节等于目录记录的 patched
// 后置自检（写出之前，**独立于定位方式**，是防"打错位置"的最终兜底）：
//   ⑦ 逐项终态 == patched 且实际改动集合 == 目录声明的写入集合
struct PatchBuildResult {
    bool ok = false;
    std::string reason;
    std::vector<std::string> notes;   // 逐条守卫结论（零静默：过了哪几条要看得见）
    std::string patched;              // 内存产物（只有 ok=true 才有）
    uint64_t written_bytes = 0;       // 参考值：这次实际改动的字节数（**判定不看它**）
};

struct PatchApplyResult {
    bool ok = false;
    std::string reason;
    std::string out_path;
    std::vector<std::string> notes;
    uint64_t written_bytes = 0;
    bool refused_input_is_output = false;  // 输出 == 输入 ⇒ 拒绝（本工具永不覆盖输入）
    bool refused_output_exists = false;    // 输出已存在且未 force ⇒ 拒绝（前端据此问"要覆盖吗"）
};

PatchBuildResult build_patched(const std::string& data, const std::vector<PatchSite>& sites);
PatchBuildResult build_patched_file(const std::string& src, const std::vector<PatchSite>& sites);
// 从**原版**生成补丁副本：绝不覆盖输入；输出已存在且 !force ⇒ 拒绝；
// 写出走 `core::write_file_atomic`（同目录 .part → 自校验 → 同盘 rename）⇒ 中途失败**不留半成品**。
PatchApplyResult apply_patch(const std::string& src, const std::string& out,
                             const std::vector<PatchSite>& sites, bool force);

}  // namespace xrkit::engine::patch
