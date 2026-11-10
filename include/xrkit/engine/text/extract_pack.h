#pragma once
// xrkit::engine::text —— 「提取文本」的**业务编排**唯一实现。
//
// CLI 与 GUI 都只做参数收集/展示；下面这些事不再各写一份：
//   · 语言包收集（可选）
//   · 源根 → configs/config 定位、输出相对前缀
//   · 根 scripts / configs/scripts / configs/gameplay 三个 job
//   · 语言目录选择、文本表合并（append）
//   · 无文本文件跳过、统计汇总
//
// 单目录的 `extract_dir` 仍保留（低层 writer）；本文件是**上层编排**。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xrkit::engine::text {

struct ExtractPackOptions {
    std::string source_root;   // 游戏根 / gamedata / 含 configs 的目录
    std::string out_root;      // 输出根（语言包/文本表/改写产物都落这里）
    bool copy_language_pack = true;  // 是否复制 configs/text、fonts、localization.ltx
    bool write_rewritten = true;     // 是否把改写后的 scripts/config 写入 out_root
    std::string lang;          // 空 = 从 localization.ltx / 目录名推断
    std::function<bool()> should_stop;
    std::function<void(int)> progress;              // 0..100
    std::function<void(const std::string&)> message;
};

struct ExtractPackResult {
    bool ok = false;
    bool cancelled = false;
    bool empty = false;        // 没有 scripts/gameplay，或 files/strings 全 0
    std::string error;
    std::string source;
    std::string out;
    std::string texts_out;
    std::string lang;
    std::vector<std::string> langs;
    int copied = 0;            // 语言包复制文件数
    int files = 0;             // 真正写出改写内容的文件数
    int strings = 0;           // 文本条数
    int no_text = 0;           // 无文本而跳过的文件数
    int jobs = 0;              // 实际执行的源分支数
};

ExtractPackResult extract_language_pack(const ExtractPackOptions& options);

}  // namespace xrkit::engine::text
