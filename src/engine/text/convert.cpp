#include "xrkit/engine/text/convert.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/text/encoding.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace xrkit::engine::text {
namespace {

// ★★ P17-87（待办总表 §四·9）：**确认钩子**的进程级存储（唯一一份；见 convert.h 的口径说明）。
//   · 宿主（GUI）注入"弹模态"那个；CLI/headless **不注入** ⇒ `has_encoding_confirm()` 为假 ⇒
//     引擎返回 `needs_confirm`，由调用方把问句与诊断**原样报出并非零退出**；
//   · 加锁：页面那条转换跑在**工作线程**（`task_run`），而注入发生在装配期（主线程）
//     ⇒ 读侧要拿锁（与 `task_shells_mutex` 同族的纪律）。
std::mutex& confirm_mutex() {
    static std::mutex* instance = new std::mutex();
    return *instance;
}

EncodingConfirmFn& confirm_storage() {
    static EncodingConfirmFn* instance = new EncodingConfirmFn();
    return *instance;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return text;
}

std::string strip(const std::string& text) {
    size_t b = 0;
    size_t e = text.size();
    while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r' || text[b] == '\n')) ++b;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r' ||
                     text[e - 1] == '\n')) {
        --e;
    }
    return text.substr(b, e - b);
}

bool ascii_iequal(const std::string& text, size_t pos, const char* word) {
    const size_t len = std::char_traits<char>::length(word);
    if (pos + len > text.size()) return false;
    for (size_t i = 0; i < len; ++i) {
        char a = text[pos + i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool ends_with_ci(const std::string& text, const char* suffix) {
    const size_t len = std::char_traits<char>::length(suffix);
    if (text.size() < len) return false;
    return ascii_iequal(text, text.size() - len, suffix);
}

// 找第一条 `<?xml ... ?>`：`[^>]*\?>` 的语义 = 从 `<?xml` 起到**第一个紧跟在 `?` 之后
// 的 `>`**；第一个 `>` 前面不是 `?` 就不算匹配（1.x 的正则就是这样）。
bool find_xml_decl(const std::string& text, size_t limit, size_t& start, size_t& end) {
    const size_t bound = text.size() < limit ? text.size() : limit;
    for (size_t i = 0; i + 5 <= bound; ++i) {
        if (!ascii_iequal(text, i, "<?xml")) continue;
        const size_t gt = text.find('>', i);
        if (gt == std::string::npos) return false;
        if (gt == 0 || text[gt - 1] != '?') return false;  // 声明必须以 `?>` 结束
        start = i;
        end = gt;
        return true;
    }
    return false;
}

// `re.sub(r"(encoding\s*=\s*\")[^\"]*(\")", ...)`：保留原样的空白与引号，只换值。
void replace_encoding_values(std::string& decl, const std::string& target_enc) {
    size_t i = 0;
    while (i + 8 <= decl.size()) {
        if (!ascii_iequal(decl, i, "encoding")) {
            ++i;
            continue;
        }
        size_t j = i + 8;
        while (j < decl.size() && (decl[j] == ' ' || decl[j] == '\t')) ++j;
        if (j >= decl.size() || decl[j] != '=') {
            ++i;
            continue;
        }
        ++j;
        while (j < decl.size() && (decl[j] == ' ' || decl[j] == '\t')) ++j;
        if (j >= decl.size() || decl[j] != '"') {
            ++i;
            continue;
        }
        const size_t value_start = j + 1;
        const size_t value_end = decl.find('"', value_start);
        if (value_end == std::string::npos) {
            ++i;
            continue;
        }
        decl = decl.substr(0, value_start) + target_enc + decl.substr(value_end);
        i = value_start + target_enc.size() + 1;
    }
}

std::string read_binary(const std::string& path, bool& ok) {
    std::ifstream in(xrkit::core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = true;
    return body;
}

}  // namespace

// ── ★★ P17-87：确认钩子的注入/查询（口径见 convert.h）───────────────────────────
void set_encoding_confirm(EncodingConfirmFn fn) {
    std::lock_guard<std::mutex> lock(confirm_mutex());
    confirm_storage() = std::move(fn);
}

bool has_encoding_confirm() {
    std::lock_guard<std::mutex> lock(confirm_mutex());
    return static_cast<bool>(confirm_storage());
}

std::string fix_xml_decl(const std::string& content, const std::string& target_enc) {
    size_t start = 0;
    size_t end = 0;
    if (!find_xml_decl(content, content.size(), start, end)) return content;
    std::string decl = content.substr(start, end - start + 1);
    for (char& c : decl) {
        if (c == '\'') c = '"';  // ① 统一双引号
    }
    replace_encoding_values(decl, target_enc);  // ② 只替换已存在的 encoding 值
    return content.substr(0, start) + decl + content.substr(end + 1);
}

bool needs_rewrite(const std::vector<uint8_t>& raw_after_bom, const std::string& target_enc,
                   bool had_utf8_bom) {
    if (had_utf8_bom) return true;  // ① 输出侧一律不带 BOM
    const std::string text(reinterpret_cast<const char*>(raw_after_bom.data()),
                           raw_after_bom.size());
    size_t start = 0;
    size_t end = 0;
    if (!find_xml_decl(text, 400, start, end)) return false;  // 窗口 raw[:400]
    const std::string decl = text.substr(start, end - start + 1);
    // 声明里的 encoding 属性（单/双引号都认）
    for (size_t i = 0; i + 8 <= decl.size(); ++i) {
        if (!ascii_iequal(decl, i, "encoding")) continue;
        size_t j = i + 8;
        while (j < decl.size() && (decl[j] == ' ' || decl[j] == '\t')) ++j;
        if (j >= decl.size() || decl[j] != '=') continue;
        ++j;
        while (j < decl.size() && (decl[j] == ' ' || decl[j] == '\t')) ++j;
        if (j >= decl.size() || (decl[j] != '"' && decl[j] != '\'')) continue;
        const char quote = decl[j];
        const size_t value_start = j + 1;
        const size_t value_end = decl.find(quote, value_start);
        if (value_end == std::string::npos) continue;
        const std::string current =
            lower_ascii(strip(decl.substr(value_start, value_end - value_start)));
        return current != lower_ascii(strip(target_enc));
    }
    return false;
}

ConvertOutcome convert_bytes(const std::vector<uint8_t>& raw_in, const std::string& rel_path,
                             const ConvertOptions& options) {
    ConvertOutcome out;
    out.in_bytes = raw_in.size();

    // 入口剥 BOM —— **剥掉不等于忘掉**：这个事实要参与后面的"要不要重写"判断
    bool had_utf8_bom = raw_in.size() >= 3 && raw_in[0] == 0xEF && raw_in[1] == 0xBB &&
                        raw_in[2] == 0xBF;
    std::vector<uint8_t> raw;
    if (had_utf8_bom) {
        raw.assign(raw_in.begin() + 3, raw_in.end());
    } else if (raw_in.size() >= 2 &&
               ((raw_in[0] == 0xFF && raw_in[1] == 0xFE) || (raw_in[0] == 0xFE && raw_in[1] == 0xFF))) {
        raw.assign(raw_in.begin() + 2, raw_in.end());
    } else {
        raw = raw_in;
    }

    const SniffResult sniffed = sniff_encoding(raw, options.source_encoding);
    const std::string target = lower_ascii(strip(options.target_encoding));
    out.step = sniffed.step;
    out.step_label = sniff_result_label(sniffed);  // ⑨9.7：标签来自判定，不是复算
    out.source_encoding = sniffed.encoding;
    // 零静默：判定的"不确定部分"（低置信 / 否决声明）必须原样带到界面与日志，
    // 文案只有 sniff.cpp 一份（页面不许自己拼）。
    out.flags_label = sniff_flags_label(sniffed);
    out.confidence = sniffed.confidence;
    out.low_confidence = sniffed.low_confidence;
    out.override_decl = sniffed.override_decl;
    if (target.empty()) {
        out.error = "目标编码为空";
        return out;
    }

    // ── ★★ P17-87（待办总表 §四·9）：**编码判定一律要由用户显式确认** ────────────────
    //   口径（用户 2026-10-10 拍板）：「全部上模态对话框」——
    //     · 只对 **auto**（判定链）触发：手动指定了源编码＝用户已经决定过，不再问；
    //     · 判定发生在**这里**（sniff 之后、任何解码/写盘之前）⇒ 取消时**一个字节都不会写**；
    //     · **没有注入确认器**（CLI/headless）⇒ 返回 `needs_confirm`（既非成功也非报错），
    //       由调用方把 `confirm_question` 与诊断**原样报出并非零退出** —— **不许猜、不许继续**。
    if (lower_ascii(strip(options.source_encoding)) == "auto" || options.source_encoding.empty()) {
        EncodingConfirmRequest request;
        request.path = rel_path;
        request.encoding = sniffed.encoding;
        request.step_label = out.step_label;
        request.flags_label = out.flags_label;
        request.confidence = sniffed.confidence;
        request.question = sniff_confirm_question(sniffed, rel_path);
        // 诊断原文（四件：判定 / 理由 / 标记 / 置信度）—— CLI 报出时原样打，页面弹窗也用它
        char conf_buf[32];
        std::snprintf(conf_buf, sizeof(conf_buf), "%.2f", sniffed.confidence);
        out.confirm_diagnostics = "step=" + out.step_label + " encoding=" + sniffed.encoding +
                                 " reason=" + sniffed.reason +
                                 " flags=" + (out.flags_label.empty() ? std::string("-")
                                                                     : out.flags_label) +
                                 " confidence=" + conf_buf;
        EncodingConfirmFn confirm;
        {
            std::lock_guard<std::mutex> lock(confirm_mutex());
            confirm = confirm_storage();
        }
        if (!confirm) {
            out.needs_confirm = true;
            out.confirm_question = request.question;
            return out;   // data 为空 ⇒ 调用方写不出任何东西
        }
        std::string chosen;
        const EncodingConfirmOutcome outcome = confirm(request, &chosen);
        if (outcome == EncodingConfirmOutcome::Cancelled) {
            out.confirm_question = request.question;
            out.error = "用户取消了编码确认（这一份未转换）";
            return out;
        }
        if (outcome == EncodingConfirmOutcome::Overridden && !chosen.empty()) {
            // 用户显式改成了别的源编码 ⇒ **重跑判定**（把用户的选择当 override 用），
            // 这样后面的解码/写盘与"手动指定源编码"那条路**逐字节同一条**（只有一份实现）。
            const SniffResult again = sniff_encoding(raw, chosen);
            out.step = again.step;
            out.step_label = sniff_result_label(again);
            out.source_encoding = again.encoding;
            out.flags_label = sniff_flags_label(again);
            out.confidence = again.confidence;
            out.low_confidence = again.low_confidence;
            out.override_decl = again.override_decl;
            return convert_bytes(raw_in, rel_path,
                                 [&options, &chosen] {
                                     ConvertOptions copy = options;
                                     copy.source_encoding = chosen;
                                     return copy;
                                 }());
        }
    }

    const bool is_xml = ends_with_ci(rel_path, ".xml");
    const auto decode_replace = [&raw](const std::string& encoding, std::string& text,
                                       std::string& err) {
        return decode_lossy(encoding, raw.data(), raw.size(), text, err);
    };
    const auto strip_bom_char = [](std::string& text) {
        if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    };

    if (sniffed.encoding == target) {
        // ── 源已是目标编码 ──
        out.skipped = true;
        out.used_encoding = sniffed.encoding;
        if (options.to_output_dir && target != kUtf8) {
            // 输出到新目录且目标不是 utf-8：**原样复制**（1.x 用 shutil.copy2）
            out.data = raw_in;
            out.wrote = true;
            out.out_bytes = out.data.size();
            out.ok = true;
            return out;
        }
        if (options.to_output_dir || needs_rewrite(raw, target, had_utf8_bom)) {
            std::string text;
            std::string err;
            if (!decode_replace(sniffed.encoding, text, err)) {
                out.error = err;
                return out;
            }
            strip_bom_char(text);
            if (is_xml) text = fix_xml_decl(text, target);
            if (!encode_lossy(target, text, out.data, err)) {
                out.error = err;
                return out;
            }
            out.wrote = true;
            out.out_bytes = out.data.size();
        }
        out.ok = true;
        return out;
    }

    // ── 真实转换：解码（replace）→（xml 时）对齐声明 → 按目标编码写回
    std::string text;
    std::string err;
    out.used_encoding = sniffed.encoding;
    if (!decode_replace(sniffed.encoding, text, err)) {
        // 1.x 在这里 `except Exception` 后**退回 windows-1251** 继续（只记一条警告），
        // 不是把整个文件判失败。
        // ★ 零静默：这条回退就是"**判定结论 ≠ 实际使用编码**"，必须报出来 + 计数。
        std::string fallback_err;
        if (!decode_lossy(kWindows1251, raw.data(), raw.size(), text, fallback_err)) {
            out.error = err;
            return out;
        }
        out.used_encoding = kWindows1251;
        out.used_mismatch = (sniffed.encoding != kWindows1251);
        if (out.used_mismatch) {
            if (!out.flags_label.empty()) out.flags_label += "、";
            out.flags_label += "判定≠实解";
        }
    }
    strip_bom_char(text);
    if (is_xml) text = fix_xml_decl(text, target);
    if (!encode_lossy(target, text, out.data, err)) {
        out.error = err;
        return out;
    }
    out.wrote = true;
    out.out_bytes = out.data.size();
    out.ok = true;
    return out;
}

ConvertOutcome convert_file(const std::string& path, const std::string& out_path,
                            const ConvertOptions& options) {
    ConvertOutcome out;
    bool read_ok = false;
    const std::string body = read_binary(path, read_ok);
    if (!read_ok) {
        out.error = "读不到源文件：" + path;
        return out;
    }
    const std::vector<uint8_t> raw(body.begin(), body.end());
    const std::string rel = out_path.empty() ? path : out_path;
    out = convert_bytes(raw, rel, options);
    // ★★ P17-87（§四·9）：`convert_bytes` 拿到的 `rel_path` 在"输出到新目录"时是**输出路径**
    //   （它只用它判 `.xml`）—— 而**问句必须指向用户看得见的那一份（源文件）**，
    //   否则用户会在对话框里看到 `out\a.xml` 而以为在问输出（本批实测踩到：CLI 报出的 question 里
    //   写的是输出路径）。问句原文由 `sniff.cpp` 拼，这里**只换路径**、不重拼文案。
    if (out.needs_confirm && !path.empty()) {
        out.confirm_question = sniff_confirm_question(sniff_encoding(raw, options.source_encoding), path);
    }
    out.in_bytes = raw.size();

    if (!out.ok || !out.wrote) {
        if (out.ok && !out.wrote) out.skipped = true;
        return out;
    }
    const std::string target_path = out_path.empty() ? path : out_path;
    std::string error;
    if (!xrkit::core::write_file_atomic(target_path, out.data, /*atomic=*/true, error)) {
        out.ok = false;
        out.wrote = false;
        out.error = error;
    }
    return out;
}

}  // namespace xrkit::engine::text
