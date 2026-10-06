#include "xrkit/engine/font/render.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/resample.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

// 1.x `font_pack.FIT_SAMPLE`（定标样本：常用汉字 + 全角标点）
constexpr const char* kFitSample =
    "永你好汉化测试这说们书见风阳光束类型，。！";

constexpr int kFitExtra = 13;    // `range(size, size + 14)` ⇒ 候选 size..size+13
constexpr int kInkMargin = 4;    // 定标判据：墨迹高 ≤ cell − 4
constexpr int kTexWidths[2] = {2048, 4096};  // 1024 放不下时的两档候选

// Python 的 `//` 向**下**取整，C++ 的 `/` 向零截断 —— 负数会差 1（-3//2 = -2 而 -3/2 = -1）。
int floor_div(int a, int b) {
    const int q = a / b;
    return ((a % b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Pillow 的 `round()` 是**四舍六入五成双**；`std::nearbyint` 在默认舍入模式下同口径。
int py_round(double x) { return static_cast<int>(std::nearbyint(x)); }

// 把 UTF-8 样本串拆成码位（样本全是常规字符，不走代理对）。
std::vector<uint32_t> sample_cps() {
    std::vector<uint32_t> out;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(kFitSample);
    const size_t n = std::strlen(kFitSample);
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        uint32_t cp = 0;
        size_t len = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            len = 3;
        } else {
            cp = b & 0x07;
            len = 4;
        }
        for (size_t k = 1; k < len && i + k < n; ++k) {
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// 一个字形在 Pillow 那套"布局盒画布"上的结果。
//
// ★ 实测口径（2026-09-24，见 `docs/2.0.0-字库模块规格.md` §2.1 与 §2.1.1）：
//   Pillow 的 `getmask` 画布 = `font.getbbox` 给出的**布局盒**，不是"advance 宽 × 墨迹高"：
//     x0 = min(0, bitmap_left)                      y0 = ascent − max(0, bitmap_top)
//     x1 = max(advance 像素, bitmap_left + 位图宽)   y1 = max(ascent, ascent − bitmap_top + 位图行数)
//   墨迹贴在 `(bitmap_left − x0, (ascent − bitmap_top) − y0)`；`mb` = 该画布的非零紧致 bbox。
//   旧结论（"宽 = advance 像素、高 = max(行数, bitmap_top)、墨迹贴 (bitmap_left, 0)"）在
//   **槽位字号**上碰巧成立，换到**定标字号**（16/17/18/21…）就错：'&' 少一列、'j' 偏 2 列、
//   '_' 偏 2 行（y0 要夹在 ascent 线上）。
struct Raster {
    bool has_ink = false;   // = Pillow 的 `mb is not None`
    int32_t advance = 0;    // 26.6
    int bitmap_top = 0;
    int asc_px = 0;         // `font.getmetrics()[0]`
    int box_y0 = 0;         // = Pillow `getbbox(ch)[1]`（1.x 的 `top` 用它）
    int canvas_w = 0;
    int canvas_h = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 墨迹的紧致 bbox（画布坐标，右/下开区间）
    std::vector<uint8_t> px;
};

void measure_asc(FT_Face face, Raster& out) {
    // Pillow `getmetrics()`：实测本机所有字号下 ascender/descender 都是 64 的整数倍
    // （4 个字体 × 5 个字号，adv 也一样），所以 `>>6` 与"四舍五入"无差别；这里按舍入写。
    out.asc_px = static_cast<int>((face->size->metrics.ascender + 32) >> 6);
}

// 拉取并栅格化一个字形，按 Pillow 的布局盒搭画布、算紧致 bbox。
void raster_glyph(FT_Face face, FT_UInt index, Raster& out) {
    out = Raster{};
    measure_asc(face, out);
    if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0) return;
    out.advance = face->glyph->advance.x;
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return;

    const FT_Bitmap& bm = face->glyph->bitmap;
    const int rows = static_cast<int>(bm.rows);
    const int width = static_cast<int>(bm.width);
    const int left = face->glyph->bitmap_left;
    const int top = face->glyph->bitmap_top;
    out.bitmap_top = top;

    // 布局盒（Pillow `font.getbbox`）。advance 实测恒为整像素（4 字体 × 5 字号无小数），
    // 所以 `>>6` 与"四舍五入"在这里无差别。
    const int adv_px = static_cast<int>(out.advance >> 6);
    const int ink_left = std::min(0, left);
    const int ink_right = std::max(adv_px, left + width);
    const int ink_top = out.asc_px - top;                       // 以 ascent 线为 0、向下为正
    const int ink_top_y0 = std::min(out.asc_px, ink_top);
    const int ink_bottom = std::max(out.asc_px, ink_top + rows);

    out.box_y0 = ink_top_y0;
    out.canvas_w = ink_right - ink_left;
    out.canvas_h = ink_bottom - ink_top_y0;
    if (out.canvas_w <= 0 || out.canvas_h <= 0) return;
    out.px.assign(static_cast<size_t>(out.canvas_w) * static_cast<size_t>(out.canvas_h), 0);

    const int pen_x = left - ink_left;
    const int pen_y = ink_top - ink_top_y0;
    if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.buffer != nullptr) {
        for (int r = 0; r < rows; ++r) {
            const int y = pen_y + r;
            if (y < 0 || y >= out.canvas_h) continue;
            const uint8_t* src = bm.buffer + static_cast<size_t>(r) * static_cast<size_t>(bm.pitch);
            for (int c = 0; c < width; ++c) {
                const int x = pen_x + c;
                if (x < 0 || x >= out.canvas_w) continue;
                out.px[static_cast<size_t>(y) * static_cast<size_t>(out.canvas_w) +
                       static_cast<size_t>(x)] = src[c];
            }
        }
    }

    // mb = 画布的非零紧致 bbox（Pillow `Image.getbbox()` 的语义：首尾全零的行/列都裁掉）
    int min_x = out.canvas_w, max_x = -1, min_y = out.canvas_h, max_y = -1;
    for (int y = 0; y < out.canvas_h; ++y) {
        const uint8_t* row = out.px.data() + static_cast<size_t>(y) * out.canvas_w;
        for (int x = 0; x < out.canvas_w; ++x) {
            if (row[x] == 0) continue;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < 0) return;
    out.has_ink = true;
    out.x0 = min_x;
    out.y0 = min_y;
    out.x1 = max_x + 1;
    out.y1 = max_y + 1;
}

// 1.x `fit_render_size`：把渲染字号放大到"字填满格子、但不越界也不与邻字相碰"为止。
int fit_render_size(FT_Face face, int size, bool x2) {
    const int S = x2 ? 2 : 1;
    const int cell_h = cell_height_for(size);
    const std::vector<uint32_t> sample = sample_cps();
    int best = size;
    for (int cand = size; cand <= size + kFitExtra; ++cand) {
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(cand * S)) != 0) break;
        int measured = 0;
        bool too_big = false;
        for (uint32_t cp : sample) {
            Raster r;
            raster_glyph(face, FT_Get_Char_Index(face, cp), r);
            if (!r.has_ink) continue;  // 缺字形：跳过（不参与定标）
            ++measured;
            const int ih = (r.y1 - r.y0) / S;
            const int iw = (r.x1 - r.x0) / S;
            const int raw = rect_w_for(size, cp);
            const int rw = (raw >= 0) ? std::min(raw, cell_h) : -1;
            if (ih > cell_h - kInkMargin || (rw >= 0 && iw > rw)) {
                too_big = true;
                break;
            }
        }
        if (too_big || measured == 0) break;
        best = cand;
    }
    return best;
}

// DDS legacy 头（128 B，无 DX10 头）—— 与 1.x `write_dds` 的那串常量逐字段相同。
void append_dds_header(std::vector<uint8_t>& out, int w, int h, bool fmt_8888) {
    auto put32 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    const uint32_t pitch = fmt_8888 ? static_cast<uint32_t>(w) * 4u : static_cast<uint32_t>(w);
    const uint32_t flags = 0x1007;
    out.push_back('D');
    out.push_back('D');
    out.push_back('S');
    out.push_back(' ');
    put32(124);                 // size
    put32(flags);               // flags
    put32(static_cast<uint32_t>(h));
    put32(static_cast<uint32_t>(w));
    put32(pitch);
    put32(0);                   // depth
    put32(0);                   // mipmap count
    out.insert(out.end(), 44, 0);
    if (fmt_8888) {             // pf: size=32, flags=0x41, fourcc=0, bits=32, RGBA masks
        put32(32);
        put32(0x41);
        put32(0);
        put32(32);
        put32(0x00FF0000);
        put32(0x0000FF00);
        put32(0x000000FF);
        put32(0xFF000000);
    } else {                    // pf: size=32, flags=0x2, fourcc=0, bits=8, alpha=0xFF
        put32(32);
        put32(0x2);
        put32(0);
        put32(8);
        put32(0);
        put32(0);
        put32(0);
        put32(0xFF);
    }
    put32(0x1000);              // caps
    out.insert(out.end(), 16, 0);
}

std::string format_ini_line(uint32_t cp, int x, int y, int x2, int y2) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%05u= %d, %d, %d, %d", cp, x, y, x2, y2);
    return std::string(buf);
}

}  // namespace

std::string to_windows_newlines(const std::string& text) {
    // Python 文本模式（`open(..., "w")`，newline=None）在 Windows 上把**每一个** `\n`
    // 翻成 `\r\n` —— 不是"已经是 CRLF 就跳过"。照同样的语义做。
    std::string out;
    out.reserve(text.size() + text.size() / 16 + 8);
    for (char c : text) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

// ── 字体自检（出包前的守卫）────────────────────────────────────────────
namespace {

// 一次打开字体，复用同一个 FT_Face（逐字自检要问 3000+ 个字符，逐次开字体太浪费）。
struct FontProbe {
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    std::string native;

    FontProbe(const std::string& font_path, int pixel_size) {
        if (FT_Init_FreeType(&lib) != 0) return;
        // 与 glyph.cpp 同口径：FreeType 要"本地编码的路径"（UTF-8 → 宽字符 → UTF-8）
        native = core::path_to_utf8(core::path_from_utf8(font_path));
        if (FT_New_Face(lib, native.c_str(), 0, &face) != 0) {
            face = nullptr;
            return;
        }
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
            FT_Done_Face(face);
            face = nullptr;
        }
    }
    ~FontProbe() {
        if (face != nullptr) FT_Done_Face(face);
        if (lib != nullptr) FT_Done_FreeType(lib);
    }
    bool ok() const { return face != nullptr; }
};

// = 1.x `bytes(f.getmask(ch, mode="L"))`：Pillow 掩码画布的字节（行距 = 画布宽）。
void mask_bytes(FT_Face face, uint32_t cp, std::vector<uint8_t>& out) {
    Raster r;
    raster_glyph(face, FT_Get_Char_Index(face, cp), r);
    out = std::move(r.px);
}

}  // namespace

std::vector<uint32_t> font_missing_chars(const std::string& font_path,
                                         const std::vector<uint32_t>& chars) {
    FontProbe probe(font_path, 24);  // 1.x 固定 24px 取样
    std::vector<uint32_t> miss;
    if (!probe.ok()) return miss;
    std::vector<uint8_t> ref;
    mask_bytes(probe.face, 0xE000, ref);

    auto is_notdef = [&](uint32_t cp) {
        std::vector<uint8_t> got;
        mask_bytes(probe.face, cp, got);
        if (got != ref) return false;
        const uint32_t alt = st_fallback(cp);
        if (alt != 0) {
            std::vector<uint8_t> alt_bytes;
            mask_bytes(probe.face, alt, alt_bytes);
            return alt_bytes == ref;
        }
        return true;
    };

    // 按类别成组抽样：**整组都是 .notdef** 才判定该类别缺失
    std::vector<std::vector<uint32_t>> groups;
    auto take = [&](auto pred, int want) {
        std::vector<uint32_t> group;
        for (uint32_t cp : chars) {
            if (!pred(cp)) continue;
            group.push_back(cp);
            if (static_cast<int>(group.size()) >= want) break;
        }
        return group;
    };
    std::vector<uint32_t> cjk = take([](uint32_t c) { return c >= 0x4E00 && c <= 0x9FFF; }, 3);
    if (cjk.empty()) cjk = take([](uint32_t c) { return c >= 0x3400 && c <= 0x4DBF; }, 3);
    if (cjk.empty()) cjk = take([](uint32_t c) { return c >= 0x3000 && c <= 0x30FF; }, 3);
    if (cjk.empty()) cjk = take([](uint32_t c) { return is_block_char(c); }, 3);
    const std::vector<uint32_t> cyr =
        take([](uint32_t c) { return c >= 0x0400 && c <= 0x04FF; }, 3);
    if (!cjk.empty()) groups.push_back(cjk);
    if (!cyr.empty()) groups.push_back(cyr);
    for (uint32_t cp : chars) {
        if (cp == 'A') {
            groups.push_back({cp});
            break;
        }
    }
    if (groups.empty()) {
        for (size_t i = 0; i < chars.size() && i < 3; ++i) groups.push_back({chars[i]});
    }
    for (const std::vector<uint32_t>& group : groups) {
        bool all_missing = true;
        for (uint32_t cp : group) {
            if (!is_notdef(cp)) {
                all_missing = false;
                break;
            }
        }
        if (all_missing) miss.insert(miss.end(), group.begin(), group.end());
    }
    return miss;
}

std::vector<uint32_t> font_unrenderable(const std::string& font_path,
                                        const std::vector<uint32_t>& chars) {
    FontProbe probe(font_path, 24);
    std::vector<uint32_t> out;
    if (!probe.ok()) return out;
    std::vector<uint8_t> ref;
    mask_bytes(probe.face, 0xE000, ref);
    for (uint32_t cp : chars) {
        std::vector<uint8_t> got;
        mask_bytes(probe.face, cp, got);
        if (got != ref) continue;  // 画得出，不缺
        const uint32_t alt = st_fallback(cp);
        if (alt != 0) {
            std::vector<uint8_t> alt_bytes;
            mask_bytes(probe.face, alt, alt_bytes);
            if (alt_bytes != ref) continue;  // 繁体顶得住，不算缺
        }
        out.push_back(cp);
    }
    return out;
}

bool render_font(const std::string& font_path, const std::vector<uint32_t>& chars,
                 int render_size, bool x2, const std::string& fmt, FontProduct& out) {
    out = FontProduct{};
    if (chars.empty()) {
        out.error = "字符集为空";
        return false;
    }
    if (fmt != "8888" && fmt != "A8") {
        out.error = "未知 DDS 格式：" + fmt;
        return false;
    }
    // ★★ 2026-10-06（审计处理批·2-3 / A1-3 / A4）：`render_size` 的**入参域**此前没有定义、
    //   也没有自校验 —— 除零就发生在下面 `slots = tex_w / cell_h`：
    //     · `render_size == 0` ⇒ `cell_height_for(0)` 走表外兜底 `ceil(0*1.40) == 0`
    //       ⇒ `1024 / 0` = **整数除零**（x64 上 SIGFPE / 0xC0000094）；
    //     · `render_size >= 732` ⇒ `cell_h >= 1025 > 1024` ⇒ `slots == 0`
    //       ⇒ `(chars.size() + slots - 1) % slots` 再次除零。
    //   可达性：`xrkit_cli --font-render <字体> 0 0 A8 …`（GUI 够不到，CLI/脚本够得到）。
    //   ★ 按 A4 的口径收口在**引擎自己**（而不是只改调用方）：下一个调用点不会再踩。
    //   为什么放在打开字体**之前**：这样"参数非法 ⇒ 可见错误"这条判据不需要一枚真字体
    //   （判据只判参数域，不该依赖外部前提）。
    const int cell_h_guard = cell_height_for(render_size);
    if (render_size <= 0 || cell_h_guard <= 0 || cell_h_guard > 1024) {
        out.error = "渲染字号超出可渲染域：render_size=" + std::to_string(render_size) +
                    " ⇒ cell_h=" + std::to_string(cell_h_guard) +
                    "（要求 1 <= cell_h <= 1024，否则行带网格会除零）";
        return false;
    }

    const std::string native = core::path_to_utf8(core::path_from_utf8(font_path));
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
        out.error = "FreeType 初始化失败";
        return false;
    }
    struct LibGuard {
        FT_Library l;
        ~LibGuard() { FT_Done_FreeType(l); }
    } lib_guard{lib};

    FT_Face face = nullptr;
    if (FT_New_Face(lib, native.c_str(), 0, &face) != 0) {
        out.error = "字体打不开：" + font_path;
        return false;
    }
    struct FaceGuard {
        FT_Face f;
        ~FaceGuard() { FT_Done_Face(f); }
    } face_guard{face};

    const int S = x2 ? 2 : 1;  // 1.x `S = 2 if x2 else 1`（x2 档渲染字号翻倍、最后再降采样回去）
    const int cell_h = cell_height_for(render_size);
    const int fitted = fit_render_size(face, render_size, x2);
    out.fitted_size = fitted;
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(fitted * S)) != 0) {
        out.error = "设置渲染字号失败";
        return false;
    }
    Raster probe;
    measure_asc(face, probe);
    const int asc = probe.asc_px;
    const int desc = static_cast<int>((-face->size->metrics.descender + 32) >> 6);
    const int vshift = std::max(0, cell_h - floor_div(asc + desc, S));

    // 画布宽：1024 → 2048 → 4096（判据是 `rows*cell − 1 <= tex_w`，照抄 1.x）
    int tex_w = 1024;
    int slots = tex_w / cell_h;
    int rows_n = (static_cast<int>(chars.size()) + slots - 1) / slots;
    for (int candidate : kTexWidths) {
        if (rows_n * cell_h - 1 <= tex_w) break;
        tex_w = candidate;
        slots = tex_w / cell_h;
        rows_n = (static_cast<int>(chars.size()) + slots - 1) / slots;
    }
    if (rows_n * cell_h - 1 > tex_w) {
        out.error = "字库放不下这么多字符（宽 " + std::to_string(tex_w) + "）";
        return false;
    }
    const int max_y = rows_n * cell_h;
    int tex_h = 16;
    while (tex_h < std::max(1, max_y)) tex_h *= 2;

    std::vector<uint8_t> canvas(static_cast<size_t>(tex_w) * static_cast<size_t>(tex_h), 0);
    std::vector<std::string> ini_lines;
    ini_lines.reserve(chars.size());

    for (size_t i = 0; i < chars.size(); ++i) {
        const uint32_t cp = chars[i];
        const int col = static_cast<int>(i) % slots;
        const int row = static_cast<int>(i) / slots;
        const int y = row * cell_h;

        Raster r;
        raster_glyph(face, FT_Get_Char_Index(face, cp), r);

        // 推进宽（1.x `widths_of`）：表/方块兜底 → 表外按字体 advance 现算 → 封顶 cell
        int raw = rect_w_for(render_size, cp);
        if (raw < 0) {
            const double adv_px = static_cast<double>(r.advance) / 64.0;
            raw = std::max(1, py_round(adv_px / S) + 1);
        }
        const int rw0 = std::min(raw, cell_h);

        // 缺墨迹 → 用**繁体**字形顶（码位不变）
        if (!r.has_ink) {
            const uint32_t alt = st_fallback(cp);
            if (alt != 0) {
                Raster alt_r;
                raster_glyph(face, FT_Get_Char_Index(face, alt), alt_r);
                if (alt_r.has_ink) {
                    r = alt_r;
                    out.substituted.emplace_back(cp, alt);
                }
            }
        }

        int gw = 0;
        int gh = 0;
        int left0 = 0;
        if (r.has_ink) {
            gw = (r.x1 - r.x0) / S;
            gh = (r.y1 - r.y0) / S;
            left0 = r.x0 / S;
        }
        const BoxResult box = adjudicate_box(cp, rw0, left0, gw, cell_h);
        const int x = col * cell_h + (cell_h - rw0) / 2 + left0 - box.pl;
        ini_lines.push_back(format_ini_line(cp, x, y, x + box.rw, y + cell_h));
        if (std::getenv("XRKIT_FONT_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[dbg] cp=%u x2=%d rw0=%d canvas=%dx%d mb=(%d,%d,%d,%d) "
                         "gw=%d gh=%d left0=%d pl=%d rw=%d gwd=%d\n",
                         cp, S, rw0, r.canvas_w, r.canvas_h, r.x0, r.y0, r.x1, r.y1,
                         gw, gh, left0, box.pl, box.rw, box.gw_draw);
        }

        if (!r.has_ink || box.gw_draw < 1 || gh < 1) continue;

        // 裁出紧致墨迹（= 1.x 的 `Image.frombytes(...).crop(mb)`）；S>1 再按 LANCZOS 压到
        // `(box.gw_draw, gh)`（= 1.x 的 `glyph.resize((gw, gh), Image.LANCZOS)`）。
        const int ink_w = r.x1 - r.x0;
        const int ink_h = r.y1 - r.y0;
        std::vector<uint8_t> glyph(static_cast<size_t>(ink_w) * static_cast<size_t>(ink_h));
        for (int rr = 0; rr < ink_h; ++rr) {
            std::memcpy(&glyph[static_cast<size_t>(rr) * ink_w],
                        &r.px[static_cast<size_t>(r.y0 + rr) * r.canvas_w + r.x0],
                        static_cast<size_t>(ink_w));
        }
        int glyph_w = ink_w;
        int glyph_h = ink_h;
        if (S > 1 && box.gw_draw != ink_w) {
            std::vector<uint8_t> resized(static_cast<size_t>(box.gw_draw) *
                                         static_cast<size_t>(gh));
            resample_lanczos_8bpc(glyph.data(), ink_w, ink_h, resized.data(), box.gw_draw, gh);
            glyph.swap(resized);
            glyph_w = box.gw_draw;
            glyph_h = gh;
        }

        // 纵向：em 盒底贴行带底；装不下先整体上移，再装不下才裁进行带（裁的是带外不可见像素）
        int top = floor_div(r.box_y0, S) + vshift;
        if (top + gh > cell_h) top -= (top + gh - cell_h);
        int src_y0 = 0;
        int keep = glyph_h;
        const int clip_top = std::max(0, -top);
        const int clip_bot = std::max(0, top + glyph_h - cell_h);
        if (clip_top != 0 || clip_bot != 0) {
            ++out.clipped;
            keep = glyph_h - clip_top - clip_bot;
            if (keep < 1) continue;
            src_y0 += clip_top;
            top += clip_top;
        }
        // S=1 且没触发裁剪时贴的是**原始**墨迹宽（`ink_draw_w` 只影响 S>1 的压宽与守卫）；
        // 触发裁剪时 1.x 会 `crop((0, clip_top, gw, clip_top+keep))`，宽度随之变成 gw。
        const int draw_x = x + box.pl;
        const int draw_y = y + top;
        const int paste_w = (clip_top != 0 || clip_bot != 0)
                                ? std::min(box.gw_draw, glyph_w)
                                : glyph_w;
        for (int rr = 0; rr < keep; ++rr) {
            const int dy = draw_y + rr;
            if (dy < 0 || dy >= tex_h) continue;
            const uint8_t* src = glyph.data() + static_cast<size_t>(src_y0 + rr) * glyph_w;
            uint8_t* dst = canvas.data() + static_cast<size_t>(dy) * tex_w;
            for (int cc = 0; cc < paste_w; ++cc) {
                const int dx = draw_x + cc;
                if (dx < 0 || dx >= tex_w) continue;
                dst[dx] = src[cc];
            }
        }
    }

    // ini 正文（LF；落盘时按 1.x 的文本模式翻成 CRLF，见 to_windows_newlines）
    out.ini = "[mb_symbol_coords]\nheight=" + std::to_string(cell_h) + "\n";
    for (const std::string& line : ini_lines) {
        out.ini += line;
        out.ini += "\n";
    }

    out.tex_w = tex_w;
    out.tex_h = tex_h;
    out.max_y = max_y;
    append_dds_header(out.dds, tex_w, tex_h, fmt == "8888");
    if (fmt == "8888") {
        out.dds.reserve(out.dds.size() + canvas.size() * 4);
        for (uint8_t v : canvas) {
            out.dds.push_back(v);
            out.dds.push_back(v);
            out.dds.push_back(v);
            out.dds.push_back(v);
        }
    } else {
        out.dds.insert(out.dds.end(), canvas.begin(), canvas.end());
    }
    return true;
}

}  // namespace xrkit::engine::font
