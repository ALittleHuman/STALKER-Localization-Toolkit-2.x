#include "xrkit/engine/patch/patch.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace xrkit::engine::patch {
namespace {

// ── 小工具 ──────────────────────────────────────────────────────────────
// ★ Python 切片语义：起点越界 ⇒ 空串；长度不够 ⇒ 截断（**不报错**，判态那层再看够不够）
std::string slice(const std::string& data, uint64_t off, size_t n) {
    if (off >= data.size()) return std::string();
    const size_t start = static_cast<size_t>(off);
    return data.substr(start, std::min(n, data.size() - start));
}

uint16_t read_u16(const std::string& d, size_t off) {
    if (off + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<unsigned char>(d[off]) |
                                 (static_cast<unsigned char>(d[off + 1]) << 8));
}

uint32_t read_u32(const std::string& d, size_t off) {
    if (off + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(d[off])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[off + 3])) << 24);
}

int32_t read_i32(const std::string& d, size_t off) {
    return static_cast<int32_t>(read_u32(d, off));
}

std::string helper_name(const PatchSite& site) {
    const size_t dot = site.id.rfind('.');
    return dot == std::string::npos ? site.id : site.id.substr(dot + 1);
}

std::vector<const PatchSite*> sites_of(const std::vector<PatchSite>& sites, const char* kind) {
    std::vector<const PatchSite*> out;
    for (const PatchSite& s : sites) {
        if (s.kind == kind) out.push_back(&s);
    }
    return out;
}

std::map<std::string, const PatchSite*> helper_map(const std::vector<PatchSite>& sites) {
    std::map<std::string, const PatchSite*> out;
    for (const PatchSite* s : sites_of(sites, "helper")) out[helper_name(*s)] = s;
    return out;
}

// helper 区整体跨度（跟着目录走：加一条 helper 项，跨度自动扩）
void cave_span(const std::vector<PatchSite>& sites, uint64_t& lo, uint64_t& hi) {
    lo = 0;
    hi = 0;
    bool first = true;
    for (const PatchSite* s : sites_of(sites, "helper")) {
        const uint64_t a = s->offset;
        const uint64_t b = s->offset + hex_to_bytes(s->patched_hex).size();
        if (first) {
            lo = a; hi = b; first = false;
        } else {
            lo = std::min(lo, a); hi = std::max(hi, b);
        }
    }
}

// 本目录会碰到的偏移范围；hi < lo 表示目录为空
void touch_span(const std::vector<PatchSite>& sites, uint64_t& lo, uint64_t& hi) {
    bool first = true;
    lo = 0; hi = 0;
    for (const PatchSite& s : sites) {
        const size_t need = std::max(hex_to_bytes(s.expect_hex).size(),
                                     hex_to_bytes(s.patched_hex).size());
        const uint64_t a = s.offset;
        const uint64_t b = s.offset + (need == 0 ? 0 : need - 1);
        if (first) { lo = a; hi = b; first = false; }
        else { lo = std::min(lo, a); hi = std::max(hi, b); }
    }
}

const PatchSite* first_of(const std::vector<PatchSite>& sites, const char* kind) {
    for (const PatchSite& s : sites) {
        if (s.kind == kind) return &s;
    }
    return nullptr;
}

// ── PE 解析（只要 layout_guard 需要的几项）──────────────────────────────
struct PeInfo {
    bool ok = false;
    std::string error;
    uint16_t machine = 0;
    uint16_t magic = 0;
    bool has_text = false;
    std::string text_name;
    uint64_t text_va = 0, text_rawptr = 0, text_rawsize = 0, text_delta = 0;
};

PeInfo pe_info(const std::string& d) {
    PeInfo info;
    if (d.size() < 0x40) {
        info.error = "文件太小，装不下 DOS 头";
        return info;
    }
    if (static_cast<unsigned char>(d[0]) != 'M' || static_cast<unsigned char>(d[1]) != 'Z') {
        info.error = "不是 MZ 开头（不是 PE 文件）";
        return info;
    }
    const uint32_t lfanew = read_u32(d, 0x3C);
    if (lfanew == 0 || static_cast<size_t>(lfanew) + 24 > d.size()) {
        info.error = "e_lfanew 越界";
        return info;
    }
    if (read_u32(d, lfanew) != 0x00004550u) {  // "PE\0\0"
        info.error = "PE 签名不符";
        return info;
    }
    info.machine = read_u16(d, static_cast<size_t>(lfanew) + 4);
    const uint16_t n_sections = read_u16(d, static_cast<size_t>(lfanew) + 6);
    const uint16_t opt_size = read_u16(d, static_cast<size_t>(lfanew) + 20);
    info.magic = read_u16(d, static_cast<size_t>(lfanew) + 24);
    const size_t sec_table = static_cast<size_t>(lfanew) + 24 + opt_size;
    for (uint16_t i = 0; i < n_sections; ++i) {
        const size_t off = sec_table + static_cast<size_t>(i) * 40;
        if (off + 40 > d.size()) break;
        std::string name = d.substr(off, 8);
        const size_t nul = name.find('\0');
        if (nul != std::string::npos) name = name.substr(0, nul);
        if (name != ".text") continue;
        info.has_text = true;
        info.text_name = name;
        info.text_va = read_u32(d, off + 12);
        info.text_rawsize = read_u32(d, off + 16);
        info.text_rawptr = read_u32(d, off + 20);
        info.text_delta = info.text_va - info.text_rawptr;
        break;
    }
    info.ok = true;
    return info;
}

}  // namespace

// ── 十六进制工具 ────────────────────────────────────────────────────────
std::string bytes_to_hex(const std::string& raw) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i != 0) out.push_back(' ');
        const unsigned char b = static_cast<unsigned char>(raw[i]);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string hex_to_bytes(const std::string& hex, bool* ok) {
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-') continue;
        clean.push_back(c);
    }
    if (ok != nullptr) *ok = true;
    if (clean.size() % 2 != 0) {
        if (ok != nullptr) *ok = false;
        return std::string();
    }
    std::string out;
    out.reserve(clean.size() / 2);
    auto nibble = [&](char c, bool& good) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        good = false;
        return 0;
    };
    for (size_t i = 0; i + 1 < clean.size(); i += 2) {
        bool good = true;
        const int hi = nibble(clean[i], good);
        const int lo = nibble(clean[i + 1], good);
        if (!good) {
            if (ok != nullptr) *ok = false;
            return std::string();
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string hex_short(const std::string& raw, size_t n) {
    if (raw.size() <= n) return bytes_to_hex(raw);
    char buf[64];
    std::snprintf(buf, sizeof(buf), " …(共 %zu 字节)", raw.size());
    return bytes_to_hex(raw.substr(0, n)) + buf;
}

// ── 布局守卫 ────────────────────────────────────────────────────────────
PatchLayout parse_layout_buffer(const std::string& data, const std::vector<PatchSite>& sites) {
    PatchLayout lay;
    const PeInfo pe = pe_info(data);
    lay.parsed = pe.ok;
    lay.error = pe.error;
    lay.machine = pe.machine;
    lay.magic = pe.magic;
    lay.text_name = pe.text_name;
    lay.text_va = pe.text_va;
    lay.text_rawptr = pe.text_rawptr;
    lay.text_rawsize = pe.text_rawsize;
    lay.text_delta = pe.text_delta;
    lay.delta_is_expected = pe.has_text && pe.text_delta == kRvaDelta;
    uint64_t lo = 0, hi = 0;
    touch_span(sites, lo, hi);
    lay.touch_lo = lo;
    lay.touch_hi = hi;
    lay.targets_inside_text = pe.has_text && hi >= lo &&
                              pe.text_rawptr <= lo &&
                              hi < pe.text_rawptr + pe.text_rawsize;

    // 硬条件：能解析 + x64 + PE32+（本补丁的机器码是 x64/PE32+ 专用）
    std::string hard;
    if (!pe.ok) {
        hard = pe.error.empty() ? "PE 解析失败" : pe.error;
    } else {
        if (pe.machine != 0x8664) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "machine=0x%X 不是 x64（本补丁的机器码是 x64 专用）", pe.machine);
            hard += buf;
        }
        if (pe.magic != 0x20B) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "%smagic=0x%X 不是 PE32+（本补丁的机器码是 PE32+ 专用）",
                          hard.empty() ? "" : "；", pe.magic);
            hard += buf;
        }
    }
    // 目录项**显式**声明了 layout 的，对应项变硬查（声明了就要求实测吻合）
    for (const PatchSite& s : sites) {
        if (s.layout_delta != 0 &&
            (!pe.has_text || pe.text_delta != s.layout_delta)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s目录项 %s 声明了 layout.delta=0x%llX，实测 0x%llX",
                          hard.empty() ? "" : "；", s.id.c_str(),
                          static_cast<unsigned long long>(s.layout_delta),
                          static_cast<unsigned long long>(pe.has_text ? pe.text_delta : 0));
            hard += buf;
        }
        if (!s.layout_section.empty() &&
            (!pe.has_text || pe.text_name != s.layout_section)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s目录项 %s 声明了 layout.section=%s，实测 %s",
                          hard.empty() ? "" : "；", s.id.c_str(), s.layout_section.c_str(),
                          pe.has_text ? pe.text_name.c_str() : "(无 .text)");
            hard += buf;
        }
    }
    lay.hard_ok = hard.empty();

    // 软条件（**只出现在报告里，不拦**）：这两条是本构建特征，判定交给逐项内容 + 锚 + 写后自检
    std::string note;
    if (!pe.has_text) {
        note = "没有 .text 节（本构建特征无从核对）";
    } else {
        char buf[256];
        if (pe.text_delta != kRvaDelta) {
            std::snprintf(buf, sizeof(buf),
                          ".text 的 VA-RawPtr=0x%llX ≠ 硬编码的 0x%llX —— **只是提示**："
                          "这不是当初那一份构建；判定仍走逐项内容 + 锚 + 写后自检",
                          static_cast<unsigned long long>(pe.text_delta),
                          static_cast<unsigned long long>(kRvaDelta));
            note = buf;
        } else if (hi >= lo && !lay.targets_inside_text) {
            std::snprintf(buf, sizeof(buf),
                          "要改的偏移 0x%08llX..0x%08llX 不在 .text 原始数据"
                          "（0x%08llX..0x%08llX）内 —— **只是提示**",
                          static_cast<unsigned long long>(lo),
                          static_cast<unsigned long long>(hi),
                          static_cast<unsigned long long>(pe.text_rawptr),
                          static_cast<unsigned long long>(pe.text_rawptr + pe.text_rawsize - 1));
            note = buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          ".text VA=0x%llX RawPtr=0x%llX → VA-RawPtr=0x%llX（与硬编码假设一致），"
                          "全部目标偏移都落在 .text 原始数据内",
                          static_cast<unsigned long long>(pe.text_va),
                          static_cast<unsigned long long>(pe.text_rawptr),
                          static_cast<unsigned long long>(pe.text_delta));
            note = buf;
        }
    }
    lay.note = hard.empty() ? note : hard;
    return lay;
}

// ── 逐项判态 ────────────────────────────────────────────────────────────
namespace {

// 该偏移处若是 `E8 rel32`，返回它指向的**文件偏移**；否则 -1
int64_t call_target_at(const std::string& data, uint64_t offset, int64_t delta) {
    if (offset + 5 > data.size()) return -1;
    if (static_cast<unsigned char>(data[static_cast<size_t>(offset)]) != 0xE8) return -1;
    const int32_t rel = read_i32(data, static_cast<size_t>(offset) + 1);
    const int64_t call_rva = static_cast<int64_t>(offset) + delta;
    return (call_rva + 5 + rel) - delta;
}

std::string context_problem(const PatchSite& site, const std::string& data) {
    struct Side { const char* label; const std::string* want; uint64_t off; };
    const std::string before = hex_to_bytes(site.before_hex);
    const std::string after = hex_to_bytes(site.after_hex);
    const std::string expect = hex_to_bytes(site.expect_hex);
    Side sides[2] = {
        {"前", &before, site.offset >= before.size() ? site.offset - before.size() : 0},
        {"后", &after, site.offset + expect.size()},
    };
    for (const Side& side : sides) {
        if (side.want->empty()) continue;
        const std::string got = slice(data, side.off, side.want->size());
        if (got != *side.want) {
            return std::string(side.label) + "锚不符：0x" + [&] {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08llX",
                              static_cast<unsigned long long>(side.off));
                return std::string(buf);
            }() + " 处期望 " + bytes_to_hex(*side.want) + "，实为 " + hex_short(got);
        }
    }
    return std::string();
}

PatchItemState item_state_of(const PatchSite& site, const std::string& data, int64_t delta) {
    PatchItemState d;
    d.id = site.id;
    d.kind = site.kind;
    d.offset = site.offset;
    d.expect_hex = bytes_to_hex(hex_to_bytes(site.expect_hex));
    d.patched_hex = bytes_to_hex(hex_to_bytes(site.patched_hex));
    d.note = site.note;
    const std::string expect = hex_to_bytes(site.expect_hex);
    const std::string patched = hex_to_bytes(site.patched_hex);
    const size_t need = std::max(expect.size(), patched.size());
    const std::string got = slice(data, site.offset, need);
    d.actual_hex = bytes_to_hex(got);
    if (got.size() < need) {
        d.state = "out-of-range";
        return d;
    }
    if (site.kind == "preserve") {
        d.state = (got == patched) ? "kept" : "broken";
    } else if (got == patched) {
        d.state = "patched";
    } else if (got == expect) {
        d.state = "orig";
    } else {
        d.state = "other";
    }
    if (site.kind == "call") {
        d.call_target = call_target_at(data, site.offset, delta);
        // `call_target_defect`（按**缺陷落位**算的目标）在 `identify_buffer` 里填 ——
        // 那里才有 helper 表；这里保持 -1。
    }
    const std::string problem = context_problem(site, data);
    if (!problem.empty() &&
        (d.state == "orig" || d.state == "patched" || d.state == "kept")) {
        d.context_problem = problem;
        d.state = "context-mismatch";
    }
    return d;
}

}  // namespace

// ── 识别（指纹状态机）──────────────────────────────────────────────────
PatchIdentifyResult identify_buffer(const std::string& data, const std::vector<PatchSite>& sites) {
    PatchIdentifyResult r;
    r.file_size = data.size();
    // ★ 布局守卫的结果**必须填进结果**（修前漏了：CLI 打的是默认构造值 ⇒ parsed 恒 0）
    r.layout = parse_layout_buffer(data, sites);
    uint64_t tlo = 0, thi = 0;
    touch_span(sites, tlo, thi);
    if (sites.empty()) {
        r.error = "目录为空：没有收录任何一项（收录一处才能补一处）";
        return r;
    }
    const uint64_t need = thi + 1;
    if (data.size() < need) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "文件只有 %zu 字节，装不下补丁涉及的范围（需要 ≥ %llu）",
                      data.size(), static_cast<unsigned long long>(need));
        r.error = buf;
        return r;
    }

    const PeInfo pe = pe_info(data);
    if (pe.ok && pe.has_text) r.delta = static_cast<int64_t>(pe.text_delta);
    const int64_t delta = r.delta;

    for (const PatchSite& s : sites) {
        PatchItemState item = item_state_of(s, data, delta);
        // 缺陷落位目标：helper 项偏移前移 kDefectShift 后算出的目标
        if (s.kind == "call" && item.call_target >= 0) {
            const std::map<std::string, const PatchSite*> hm = helper_map(sites);
            const auto it = hm.find(s.helper);
            if (it != hm.end()) {
                item.call_target_defect = static_cast<int64_t>(it->second->offset) + kDefectShift;
            }
        }
        r.items.push_back(item);
    }

    // 汇总计数（判态直方图）
    for (const PatchItemState& item : r.items) {
        if (item.state == "orig") ++r.n_orig;
        else if (item.state == "patched") ++r.n_patched;
        else if (item.state == "kept") ++r.n_kept;
        else if (item.state == "broken") ++r.n_broken;
        else if (item.state == "context-mismatch") ++r.n_context;
        else if (item.state == "out-of-range") ++r.n_range;
        else ++r.n_other;
    }

    // 空闲区是否全零 + thunk 是否逐字节未变
    uint64_t clo = 0, chi = 0;
    cave_span(sites, clo, chi);
    r.cave_lo = clo;
    r.cave_hi = chi;
    r.cave_all_zero = true;
    for (uint64_t i = clo; i < chi; ++i) {
        if (slice(data, i, 1) != std::string(1, '\0')) { r.cave_all_zero = false; break; }
    }
    const PatchSite* thunk = first_of(sites, "preserve");
    if (thunk != nullptr) {
        const std::string want = hex_to_bytes(thunk->expect_hex);
        const std::string got = slice(data, thunk->offset, want.size());
        r.thunk_actual_hex = bytes_to_hex(got);
        r.thunk_intact = (got == want);
    }

    // 参考差异数（★ 判定不看它；实测 v3 与 v4 该数完全相同 = 88）
    uint64_t changed = 0;
    for (const PatchSite& s : sites) {
        const std::string expect = hex_to_bytes(s.expect_hex);
        for (size_t i = 0; i < expect.size(); ++i) {
            const uint64_t off = s.offset + i;
            const std::string got = slice(data, off, 1);
            if (got.size() != 1 || got[0] != expect[i]) ++changed;
        }
    }
    r.reference_diff_bytes = changed;

    // ── 指纹状态机：结论只从逐项判态 + thunk 完整性推出来 ──
    std::map<std::string, std::string> state_of;
    for (const PatchItemState& item : r.items) state_of[item.id] = item.state;

    const std::map<std::string, const PatchSite*> hm = helper_map(sites);
    std::map<std::string, bool> defect_helpers;
    for (const auto& kv : hm) {
        const std::string patched = hex_to_bytes(kv.second->patched_hex);
        const int64_t off = static_cast<int64_t>(kv.second->offset) + kDefectShift;
        defect_helpers[kv.first] = off >= 0 &&
            slice(data, static_cast<uint64_t>(off), patched.size()) == patched;
    }
    const PatchSite* imm = first_of(sites, "overwrite");
    const std::string imm_id = imm != nullptr ? imm->id : std::string();
    const std::vector<const PatchSite*> calls = sites_of(sites, "call");
    auto n_of = [&](const std::vector<const PatchSite*>& list, const char* want) {
        int n = 0;
        for (const PatchSite* s : list) {
            const auto it = state_of.find(s->id);
            if (it != state_of.end() && it->second == want) ++n;
        }
        return n;
    };
    const int n_call_orig = n_of(calls, "orig");
    int n_call_defect = 0;
    for (const PatchSite* s : calls) {
        int64_t target = -1;
        for (const PatchItemState& item : r.items) {
            if (item.id == s->id) { target = item.call_target; break; }
        }
        const auto hit = hm.find(s->helper);
        if (hit != hm.end() && target >= 0) {
            const int64_t defect_off = static_cast<int64_t>(hit->second->offset) + kDefectShift;
            // ★ 两边都是**文件偏移**（`call_target_at` 已经把 delta 减回去了，见它的注释）——
            //   修前这里多减了一次 delta ⇒ n_call_defect 恒 0 ⇒ v2 被误判成 v1v3。
            //   （金标准一眼抓出：逐项判态与计数全对，只有 verdict 差一档。）
            if (target == defect_off) ++n_call_defect;
        }
    }
    std::string missing, broken;
    for (const PatchItemState& item : r.items) {
        if (item.state == "orig" || item.state == "other" ||
            item.state == "out-of-range" || item.state == "context-mismatch") {
            if (!missing.empty()) missing += "、";
            missing += item.id;
        }
        if (item.state == "broken") {
            if (!broken.empty()) broken += "、";
            broken += item.id;
        }
    }
    bool any_defect_helper = false;
    for (const auto& kv : defect_helpers) any_defect_helper = any_defect_helper || kv.second;

    auto all_in = [&](const std::vector<std::string>& wants) {
        for (const PatchSite& s : sites) {
            const auto it = state_of.find(s.id);
            const std::string st = it == state_of.end() ? std::string("out-of-range") : it->second;
            if (std::find(wants.begin(), wants.end(), st) == wants.end()) return false;
        }
        return true;
    };

    char buf[512];
    // ① 原版：每一项都还是原样，且没有任何补丁痕迹
    if (sites.size() > 0 &&
        all_in({"orig", "kept"}) && !any_defect_helper &&
        r.thunk_intact && r.cave_all_zero) {
        r.ok = true;
        r.verdict_key = "orig";
        r.verdict_text = "原版";
        r.detail = "未发现任何补丁痕迹：每一项都还是原版字节，helper 区仍是全零填充，thunk 完好";
        return r;
    }
    // ② v4：每一项都到了终态，且 preserve 项逐字节未变
    if (all_in({"patched", "kept"}) && r.thunk_intact) {
        r.ok = true;
        r.verdict_key = "v4";
        r.verdict_text = "v4(正确)";
        std::snprintf(buf, sizeof(buf), "目录共 %zu 项全部到位，helper 落位正确，thunk 逐字节未受影响",
                      sites.size());
        r.detail = buf;
        return r;
    }
    // ③ v2：thunk 被覆盖 + 只有 helper A/B（且在缺陷落位）+ 只改了前 3 处调用点
    const auto imm_it = state_of.find(imm_id);
    const bool imm_patched = !imm_id.empty() && imm_it != state_of.end() &&
                             imm_it->second == "patched";
    if (!r.thunk_intact && imm_patched &&
        defect_helpers.count("A") != 0 && defect_helpers["A"] &&
        defect_helpers.count("B") != 0 && defect_helpers["B"] &&
        (defect_helpers.count("C") == 0 || !defect_helpers["C"]) &&
        n_call_defect == 3 && n_call_orig == static_cast<int>(calls.size()) - 3) {
        r.ok = true;
        r.verdict_key = "v2";
        r.verdict_text = "v2(有缺陷)";
        const auto a = hm.find("A");
        std::snprintf(buf, sizeof(buf),
                      "退出崩溃版：helper A/B 落在 0x%08llX（覆盖 thunk），helper C 与其余 "
                      "调用点缺失",
                      static_cast<unsigned long long>(
                          (a != hm.end() ? a->second->offset : 0) + kDefectShift));
        r.detail = buf;
        return r;
    }
    // ④ v1 / v3：thunk 被覆盖，但落位/覆盖范围与 v2 不同
    if (!r.thunk_intact && imm_patched &&
        defect_helpers.count("A") != 0 && defect_helpers["A"] &&
        defect_helpers.count("B") != 0 && defect_helpers["B"]) {
        r.ok = true;
        r.verdict_key = "v1v3";
        r.verdict_text = "v1 或 v3(有缺陷)";
        const auto a = hm.find("A");
        const unsigned long long aoff = static_cast<unsigned long long>(
            (a != hm.end() ? a->second->offset : 0) + kDefectShift);
        std::string detail;
        if (n_call_defect == static_cast<int>(calls.size()) &&
            defect_helpers.count("C") != 0 && defect_helpers["C"]) {
            std::snprintf(buf, sizeof(buf),
                          "退出崩溃版（疑似 v3）：调用点与 A/B/C 都在，但 helper 区整体"
                          "前移 %d 字节，A 落在 0x%08llX 覆盖了 thunk", -kDefectShift, aoff);
            detail = buf;
        } else if (n_call_defect == 2 && n_call_orig == static_cast<int>(calls.size()) - 2) {
            std::snprintf(buf, sizeof(buf),
                          "退出崩溃版（疑似 v1）：只改了前 2 处调用点，helper 区前移 %d 字节，"
                          "A 落在 0x%08llX 覆盖了 thunk", -kDefectShift, aoff);
            detail = buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "退出崩溃版：helper 区前移 %d 字节覆盖了 thunk；覆盖率与已知的 "
                          "v1/v3 都不完全相同", -kDefectShift);
            detail = buf;
        }
        detail += "；缺失项: " + (missing.empty() ? std::string("无") : missing);
        r.detail = detail;
        return r;
    }
    r.ok = true;
    r.verdict_key = "unknown";
    r.verdict_text = "未知";
    std::string detail = "既不是原版、也不是已知的 v4/v2/v1/v3 形态；缺失项: " +
                         (missing.empty() ? std::string("无") : missing);
    if (!broken.empty()) detail += "；异常项: " + broken;
    r.detail = detail;
    return r;
}

PatchIdentifyResult identify_file(const std::string& path, const std::vector<PatchSite>& sites) {
    // ★ 必须走 `path_from_utf8`：MSVC 的 `std::ifstream(std::string)` 按 **ANSI** 解路径，
    //   含中文/西里尔的路径会**直接打不开**（本批实测：全 ASCII 的 orig 样本正常，
    //   路径里带中文的 v2/v4 样本报"读不到文件"）。给 `std::filesystem::path` 就对。
    std::ifstream in(xrkit::core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        PatchIdentifyResult r;
        r.error = "读不到文件：" + path;
        return r;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return identify_buffer(data, sites);
}

// ══════════════════════════════════════════════════════════════════════════
// P16-1b-2：写盘半（守卫链 → 生成 → 后置自检 → 原子写）
// ══════════════════════════════════════════════════════════════════════════
namespace {

std::string hex_byte(int value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", value & 0xFF);
    return buf;
}

std::string hex_off(uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08llX", static_cast<unsigned long long>(value));
    return buf;
}

// 空闲区守卫的第二步：区间起点是否落在前面某条指令的跨度内。
// ★ 只看"全零"会把 `48 FF 25 F2 10 00 00` 这条 jmp 的 disp32 尾巴（最后两字节就是 00）
//   当成空闲填充 —— v1/v2/v3 正是据此认定 0xED575C 空闲，退出时跳进未映射地址。
struct SpanConflict {
    bool found = false;
    uint64_t scan_off = 0;
    int64_t ins_start = 0;
    int64_t ins_end = 0;
    std::string desc;
    std::string seq_hex;
};

SpanConflict cave_span_conflict(const std::string& data, uint64_t start, uint64_t end) {
    SpanConflict out;
    if (end <= start) return out;
    static const unsigned char kOp1[] = {0xFF, 0x25};
    static const unsigned char kOp2[] = {0x48, 0x8D, 0x0D};
    static const unsigned char kOp3[] = {0xE8};
    static const unsigned char kOp4[] = {0xE9};
    struct Op {
        const unsigned char* seq;
        size_t seq_len;
        size_t ins_len;
        const char* desc;
    };
    static const Op kOps[] = {
        {kOp1, 2, 6, "jmp qword ptr [rip+disp32]"},
        {kOp2, 3, 7, "lea rcx,[rip+disp32]"},
        {kOp3, 1, 5, "call rel32"},
        {kOp4, 1, 5, "jmp rel32"},
    };
    const uint64_t kBackwardScan = 8;
    const uint64_t from = start > kBackwardScan ? start - kBackwardScan : 0;
    for (uint64_t off = from; off < start; ++off) {
        for (const Op& op : kOps) {
            const std::string want(reinterpret_cast<const char*>(op.seq), op.seq_len);
            if (slice(data, off, op.seq_len) != want) continue;
            int64_t ins_start = static_cast<int64_t>(off);
            int64_t ins_end = static_cast<int64_t>(off + op.ins_len - 1);
            // REX.W 前缀是指令本体的一部分：真正的凶手是 `48 FF 25 …` 这条 7 字节 jmp，
            // 只认 `FF 25` 会把起点算晚 1 字节，跨度判定就会漏掉边界情况。
            if (op.seq == kOp1 && off > 0 &&
                static_cast<unsigned char>(data[static_cast<size_t>(off - 1)]) == 0x48) {
                ins_start = static_cast<int64_t>(off) - 1;
            }
            if (ins_start <= static_cast<int64_t>(start) &&
                static_cast<int64_t>(start) <= ins_end) {
                out.found = true;
                out.scan_off = off;
                out.ins_start = ins_start;
                out.ins_end = ins_end;
                out.desc = op.desc;
                out.seq_hex = bytes_to_hex(want);
                return out;
            }
        }
    }
    return out;
}

// 首个差异字节（给守卫失败的报告用；返回 base+i、期望字节、实得字节；越界用 -1 表示）
struct FirstDiff {
    uint64_t off = 0;
    int want = -1;
    int got = -1;
};

FirstDiff first_diff_byte(const std::string& want, const std::string& got, uint64_t base) {
    FirstDiff out;
    const size_t n = std::min(want.size(), got.size());
    for (size_t i = 0; i < n; ++i) {
        if (want[i] != got[i]) {
            out.off = base + i;
            out.want = static_cast<unsigned char>(want[i]);
            out.got = static_cast<unsigned char>(got[i]);
            return out;
        }
    }
    out.off = base + n;
    if (got.size() < want.size()) {
        out.want = static_cast<unsigned char>(want[n]);
        out.got = -1;
    } else if (want.size() < got.size()) {
        out.want = -1;
        out.got = static_cast<unsigned char>(got[n]);
    }
    return out;
}

// 按 `E8 rel32` 算出该 call 项的等长替换字节（rel32 在 RVA 空间里算）
std::string computed_call_bytes(const PatchSite& site,
                                const std::map<std::string, const PatchSite*>& helpers,
                                int64_t delta, bool& ok, std::string& err) {
    ok = false;
    const auto it = helpers.find(site.helper);
    if (it == helpers.end()) {
        err = "call 项 " + site.id + " 指向的 helper「" + site.helper + "」不在目录里";
        return std::string();
    }
    const int64_t call_rva = static_cast<int64_t>(site.offset) + delta;
    const int64_t target_rva = static_cast<int64_t>(it->second->offset) + delta;
    const int64_t rel = target_rva - (call_rva + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "rel32 越界: 0x%llX", static_cast<unsigned long long>(rel));
        err = buf;
        return std::string();
    }
    const std::string expect = hex_to_bytes(site.expect_hex);
    const std::string tail = hex_to_bytes(site.tail_hex);
    std::string body;
    // ★ 必须 push 一个**字节** 0xE8；写成 `std::string body = "E8"` 会得到两个 ASCII 字符
    //   `0x45 0x38`（自洽校验当场报"算得 45 38 28 8E…，目录记录的是 E8 28 8E…"）。
    body.push_back(static_cast<char>(0xE8));
    body.push_back(static_cast<char>(rel & 0xFF));
    body.push_back(static_cast<char>((rel >> 8) & 0xFF));
    body.push_back(static_cast<char>((rel >> 16) & 0xFF));
    body.push_back(static_cast<char>((rel >> 24) & 0xFF));
    if (expect.size() < body.size() + tail.size()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "调用点 0x%08llX 放不下 call+tail",
                      static_cast<unsigned long long>(site.offset));
        err = buf;
        return std::string();
    }
    const size_t pad = expect.size() - body.size() - tail.size();
    ok = true;   // ★ 修前漏了这一句：成功路径不进任何 err 分支，`ok` 一直留在开头的 false
    return body + tail + std::string(pad, '\x90');
}

}  // namespace

PatchBuildResult build_patched(const std::string& data, const std::vector<PatchSite>& sites) {
    PatchBuildResult r;
    if (data.empty()) {
        r.reason = "输入为空";
        return r;
    }
    if (sites.empty()) {
        r.reason = "补丁目录为空：没有任何收录项，拒绝生成（fail-closed）";
        return r;
    }
    uint64_t tlo = 0, thi = 0;
    touch_span(sites, tlo, thi);
    if (data.size() <= thi) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "输入太短：只有 %zu 字节，而补丁最后一个目标偏移是 0x%08llX",
                      data.size(), static_cast<unsigned long long>(thi));
        r.reason = buf;
        return r;
    }

    // 守卫 ①：PE 布局（含"目标偏移都落在 .text 内"）
    const PatchLayout layout = parse_layout_buffer(data, sites);
    if (!layout.hard_ok) {
        r.reason = "PE 布局守卫不通过：" + layout.note;
        return r;
    }
    r.notes.push_back("PE 布局守卫: 通过（" + layout.note + "）");
    const int64_t delta = static_cast<int64_t>(layout.text_delta);

    // 守卫 ②：preserve 项 —— 不许动的地方，输入里本来就该是完好的
    const std::vector<const PatchSite*> preserves = sites_of(sites, "preserve");
    for (const PatchSite* s : preserves) {
        const std::string expect = hex_to_bytes(s->expect_hex);
        const std::string got = slice(data, s->offset, expect.size());
        if (got != expect) {
            r.reason = "preserve 守卫不通过（" + s->id + " " + hex_off(s->offset) +
                       "）：这里要求逐字节保持 " + bytes_to_hex(expect) + "，输入的实为 " +
                       hex_short(got) +
                       " —— 该输入不是本构建原版（或已被缺陷版 v1/v2/v3 破坏）";
            return r;
        }
    }
    if (!preserves.empty()) {
        r.notes.push_back("preserve 守卫: 通过（" + std::to_string(preserves.size()) +
                          " 项在输入里本来就完好）");
    }

    // 守卫 ③④：逐项 expect + 周边锚
    for (const PatchSite& s : sites) {
        const std::string expect = hex_to_bytes(s.expect_hex);
        const std::string patched = hex_to_bytes(s.patched_hex);
        const std::string got = slice(data, s.offset, expect.size());
        if (got != expect) {
            const FirstDiff fd = first_diff_byte(expect, got, s.offset);
            const bool already = (got == patched);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "逐项守卫不通过（%s %s，%s）：期望原字节 %s，实为 %s；"
                          "首个差异字节在 %s（期望 %s，实为 %s）",
                          s.id.c_str(), hex_off(s.offset).c_str(), s.kind.c_str(),
                          hex_short(expect).c_str(), hex_short(got).c_str(),
                          hex_off(fd.off).c_str(),
                          fd.want < 0 ? "越界" : hex_byte(fd.want).c_str(),
                          fd.got < 0 ? "越界" : hex_byte(fd.got).c_str());
            r.reason = buf;
            if (already) r.reason += "（该处已是终态：输入不是原版，或已经打过补丁）";
            return r;
        }
        const std::string problem = context_problem(s, data);
        if (!problem.empty()) {
            r.reason = "逐项守卫不通过（" + s.id + " " + hex_off(s.offset) + "）：" + problem +
                       " —— 锚不符说明这份输入在该处不是我们收录的那一段（换了构建，或位置漂了），"
                       "拒绝生成";
            return r;
        }
    }
    r.notes.push_back("逐项守卫: 通过（" + std::to_string(sites.size()) +
                      " 项 expect + context 全部命中）");

    // 守卫 ⑤：helper 占用的空闲区 —— 全零 + 起点不落在前一条指令的跨度内
    if (!sites_of(sites, "helper").empty()) {
        uint64_t cs = 0, ce = 0;
        cave_span(sites, cs, ce);
        const std::string cave = slice(data, cs, static_cast<size_t>(ce - cs));
        bool all_zero = (cave.size() == static_cast<size_t>(ce - cs));
        uint64_t bad = cs;
        if (all_zero) {
            for (size_t i = 0; i < cave.size(); ++i) {
                if (cave[i] != '\0') {
                    all_zero = false;
                    bad = cs + i;
                    break;
                }
            }
        }
        if (!all_zero) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "空闲区守卫不通过：0x%08llX..0x%08llX 不是全零（首个非零字节 "
                          "0x%08llX = 0x%02X）——这里已经被别的东西占用，不能把 helper 放进去",
                          static_cast<unsigned long long>(cs),
                          static_cast<unsigned long long>(ce - 1),
                          static_cast<unsigned long long>(bad),
                          static_cast<unsigned char>(slice(data, bad, 1)[0]));
            r.reason = buf;
            return r;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "空闲区守卫(全零): 通过（0x%08llX..0x%08llX 全为 0x00）",
                      static_cast<unsigned long long>(cs),
                      static_cast<unsigned long long>(ce - 1));
        r.notes.push_back(buf);
        const SpanConflict conflict = cave_span_conflict(data, cs, ce);
        if (conflict.found) {
            char buf2[320];
            std::snprintf(buf2, sizeof(buf2),
                          "空闲区守卫不通过：目标区间起点 0x%08llX 落在前面一条指令的跨度内 —— "
                          "0x%08llX 处匹配 `%s`（%s），该指令覆盖 0x%08llX..0x%08llX。"
                          "区间起点必须恰好落在某条指令之后，否则等长改写会改坏这条指令"
                          "（v2/v3 退出崩溃即由此而来）",
                          static_cast<unsigned long long>(cs),
                          static_cast<unsigned long long>(conflict.scan_off),
                          conflict.seq_hex.c_str(), conflict.desc.c_str(),
                          static_cast<unsigned long long>(conflict.ins_start),
                          static_cast<unsigned long long>(conflict.ins_end));
            r.reason = buf2;
            return r;
        }
        r.notes.push_back("空闲区守卫(不落在前一条指令跨度内): 通过");
    }

    // 守卫 ⑥：call 项自洽 —— 按 RVA 现算的字节必须逐字节等于目录记录的 patched
    const std::map<std::string, const PatchSite*> helpers = helper_map(sites);
    const std::vector<const PatchSite*> calls = sites_of(sites, "call");
    for (const PatchSite* s : calls) {
        bool ok = false;
        std::string err;
        const std::string want = computed_call_bytes(*s, helpers, delta, ok, err);
        if (!ok) {
            r.reason = "自洽校验失败（" + s->id + "）：" + err;
            return r;
        }
        const std::string recorded = hex_to_bytes(s->patched_hex);
        if (want != recorded) {
            r.reason = "自洽校验失败（" + s->id + " " + hex_off(s->offset) + "）：按 RVA 算得 " +
                       bytes_to_hex(want) + "，目录记录的是 " + bytes_to_hex(recorded) +
                       " —— 目录或 RVA 假设不可信，拒绝生成";
            return r;
        }
    }
    r.notes.push_back("自洽校验: 通过（" + std::to_string(calls.size()) +
                      " 个 call 项按 RVA 算出的字节 == 目录记录）");

    // ── 生成（逐目录项）──
    std::string out = data;
    for (const PatchSite& s : sites) {
        std::string want = hex_to_bytes(s.patched_hex);
        if (s.kind == "call") {
            bool ok = false;
            std::string err;
            want = computed_call_bytes(s, helpers, delta, ok, err);
            if (!ok) {
                r.reason = "生成失败（" + s.id + "）：" + err;
                return r;
            }
        }
        if (s.offset + want.size() > out.size()) {
            r.reason = "生成失败（" + s.id + "）：写入范围越界";
            return r;
        }
        out.replace(static_cast<size_t>(s.offset), want.size(), want);
    }

    // ── 后置自检（写出之前）──
    for (const PatchSite& s : sites) {
        const std::string want = hex_to_bytes(s.patched_hex);
        const std::string got = slice(out, s.offset, want.size());
        if (got != want) {
            r.reason = "后置自检失败：" + s.id + "（" + hex_off(s.offset) +
                       "）的终态不是 patched（期望 " + hex_short(want) + "，实为 " +
                       hex_short(got) + "）";
            return r;
        }
    }
    // 实际改动集合 == 目录声明的写入集合（**与字节数无关**的比较）
    //   · 目录声明的范围内：允许变（变了才计数）；
    //   · 范围之外：**一个字节都不许变**（有变就是"打错位置/多改"）。
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(sites.size());
    for (const PatchSite& s : sites) {
        const uint64_t len = hex_to_bytes(s.patched_hex).size();
        ranges.emplace_back(s.offset, s.offset + len);
    }
    std::sort(ranges.begin(), ranges.end());
    uint64_t changed = 0;
    size_t ri = 0;
    for (uint64_t i = 0; i < data.size(); ++i) {
        while (ri < ranges.size() && ranges[ri].second <= i) ++ri;
        const bool inside = (ri < ranges.size() && ranges[ri].first <= i &&
                             i < ranges[ri].second);
        if (inside) {
            if (data[static_cast<size_t>(i)] != out[static_cast<size_t>(i)]) ++changed;
        } else if (data[static_cast<size_t>(i)] != out[static_cast<size_t>(i)]) {
            r.reason = "后置自检失败：目录声明之外的第 " + hex_off(i) +
                       " 字节被改动了（实际改动集合 != 目录声明的写入集合）";
            return r;
        }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "后置自检: 通过（%zu/%zu 项终态逐字节吻合、preserve 项未变、"
                  "改动集合 == 目录声明；本次写入 %llu 个字节，这个数只是参考，判定不看它）",
                  sites.size(), sites.size(), static_cast<unsigned long long>(changed));
    r.notes.push_back(buf);
    r.ok = true;
    r.patched = out;
    r.written_bytes = changed;
    return r;
}

PatchBuildResult build_patched_file(const std::string& src, const std::vector<PatchSite>& sites) {
    std::ifstream in(xrkit::core::path_from_utf8(src), std::ios::binary);
    if (!in) {
        PatchBuildResult r;
        r.reason = "读不到文件：" + src;
        return r;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return build_patched(data, sites);
}

PatchApplyResult apply_patch(const std::string& src, const std::string& out,
                             const std::vector<PatchSite>& sites, bool force) {
    PatchApplyResult r;
    r.out_path = out;
    const PatchBuildResult built = build_patched_file(src, sites);
    r.notes = built.notes;
    r.written_bytes = built.written_bytes;
    if (!built.ok) {
        r.reason = built.reason;
        return r;
    }
    std::error_code ec;
    const std::filesystem::path src_abs = std::filesystem::absolute(xrkit::core::path_from_utf8(src), ec);
    const std::filesystem::path out_abs = std::filesystem::absolute(xrkit::core::path_from_utf8(out), ec);
    r.out_path = xrkit::core::path_to_utf8(out_abs);
    // 绝不覆盖输入（哪怕调用方显式要求）
    if (src_abs == out_abs) {
        r.refused_input_is_output = true;
        r.reason = "拒绝：输出与输入是同一个文件 —— 本工具永不覆盖输入";
        return r;
    }
    if (std::filesystem::exists(out_abs) && !force) {
        r.refused_output_exists = true;
        r.reason = "拒绝：输出文件已存在 " + r.out_path + " —— 需要明确要求覆盖";
        return r;
    }
    // 原子写：复用 `core::write_file_atomic`（同目录 .part → 自校验 → 同盘 rename）
    // ★ 不另写一份：本仓的纪律是"这条纪律写两份就一定会有一份先腐坏"。
    std::vector<uint8_t> payload(built.patched.begin(), built.patched.end());
    std::string err;
    if (!xrkit::core::write_file_atomic(xrkit::core::path_to_utf8(out_abs), payload, true, err)) {
        r.reason = "写出失败：" + err;
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace xrkit::engine::patch
