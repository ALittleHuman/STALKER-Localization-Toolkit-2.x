#pragma once
// xrkit::engine::fs —— **路径分量（段）的名字合法性**，唯一一份。
//
// 为什么要有它（2026-10-24，通读台账 04/05/06 的 R10/R20/R27）：
//   · parser（`squashfs.cpp`）把目录项名字当**原始字节**收下，不查 NUL/控制字符；
//   · 落盘层（`extract_tree.cpp`）的 `safe_out_path` 只看 `..`/盘符/ADS/尾点空格，**也不查**；
//   · 保留设备名（`CON`/`NUL`/`COM1`…）只在 `core::write_file_atomic` 的**叶子**上筛，
//     **目录段**没人筛（`CON/` 会直接进 `create_directories`）；
//   · 而 `overlay.cpp`（树/收集）连"尾点空格归一"都不做 ⇒ **同一项目两把尺**。
//   ⇒ 这三处必须调**同一个判定**。本头文件就是那把尺。
//
// 口径（只做"拒绝 + 明确理由"，不改任何合法名字的行为）：
//   · **控制字符**（含 NUL）：Win32 路径 API 会在 NUL 处**截断** ⇒ 实际写到另一条路径，
//     而判定仍按原串认为"在输出目录内" ⇒ 判定与落点不一致（写到意外路径那一类）；
//   · **Windows 保留设备名**：对 `CON`/`NUL` 写入**不是报错而是被设备吃掉**（静默丢数据）；
//     判定复用 `core::is_windows_reserved_name`（它已处理 `CON.txt`/`CON ` 与大小写）。
//
// ★ 名字长度上限（写侧 65536 / 读侧 4096）**不在本文件**：那是 R4/R9 的批次（两侧一起拦）。

#include <string>

#include "xrkit/core/atomic_write.h"   // core::is_windows_reserved_name

namespace xrkit::engine::fs::name_check {

// 段里有没有控制字符（< 0x20 或 0x7f）。UTF-8 多字节（≥0x80）**不算**控制字符。
inline bool has_control_bytes(const std::string& seg) {
    for (char ch : seg) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

inline bool is_reserved_device_name(const std::string& seg) {
    return xrkit::core::is_windows_reserved_name(seg);
}

// 返回**空串**表示这一段合法；否则返回可直接给用户的拒因（点名是哪条规则、哪个名字）。
// ★ 拒因文案要稳定：判据按关键字认（"控制字符" / "保留设备名"）。
inline std::string segment_reject_reason(const std::string& seg) {
    if (seg.empty()) return std::string();   // 空段由调用方按各自口径处理（丢 or 拒）
    if (has_control_bytes(seg)) {
        return "分段含控制字符（含 NUL）：" + seg +
               " —— Win32 会在 NUL 处截断路径，落点会跑到别处";
    }
    if (is_reserved_device_name(seg)) {
        return "分段是 Windows 保留设备名：" + seg +
               " —— 对它写入会被设备吃掉（数据静默丢失）";
    }
    return std::string();
}

}  // namespace xrkit::engine::fs::name_check
