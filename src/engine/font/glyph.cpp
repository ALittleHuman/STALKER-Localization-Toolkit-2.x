#include "xrkit/engine/font/glyph.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/text/encoding.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

// 探针样本：1.x 的 FIT_SAMPLE（定标样本）+ 标点 + Latin + 西里尔
constexpr const char* kSample =
    "永你好汉化测试这说们书见风阳光束类型，。！"
    "（）【】“”…—№|、；：？/"
    "AaZzЯЁЖабвгд0123456789";

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

}  // namespace

bool dump_glyphs(const std::string& font_path, int pixel_size,
                 const std::vector<uint32_t>& cps_in, const std::string& out_path) {
    if (pixel_size <= 0) return false;

    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return false;
    struct LibGuard {
        FT_Library l;
        ~LibGuard() { FT_Done_FreeType(l); }
    } lib_guard{lib};

    // FreeType 要本地编码的路径（UTF-8 → 宽字符），否则含中文的路径打不开
    const std::string native = core::path_to_utf8(core::path_from_utf8(font_path));
    FT_Face face = nullptr;
    if (FT_New_Face(lib, native.c_str(), 0, &face) != 0) return false;
    struct FaceGuard {
        FT_Face f;
        ~FaceGuard() { FT_Done_Face(f); }
    } face_guard{face};

    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) return false;

    // 样本 → 码位表（给了 cps 就用给的，否则用内置样本）
    std::vector<uint32_t> cps = cps_in;
    if (cps.empty()) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(kSample);
        const size_t n = std::strlen(kSample);
        size_t i = 0;
        while (i < n) {
            uint32_t cp = 0;
            if (!text::utf8_next(p, n, i, cp)) break;
            cps.push_back(cp);
        }
    }

    std::vector<uint8_t> out;
    put32(out, 0x504C5947u);  // "GLYP"
    put32(out, static_cast<uint32_t>(pixel_size));
    put32(out, static_cast<uint32_t>(cps.size()));

    for (uint32_t cp : cps) {
        FT_UInt index = FT_Get_Char_Index(face, cp);
        int32_t left = 0;
        int32_t top = 0;
        int32_t advance = 0;
        int32_t height = 0;
        int32_t width = 0;
        int32_t pitch = 0;
        std::vector<uint8_t> tight;
        if (index != 0 && FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) == 0) {
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            const FT_Bitmap& bm = face->glyph->bitmap;
            left = face->glyph->bitmap_left;
            top = face->glyph->bitmap_top;
            advance = face->glyph->advance.x;
            height = static_cast<int32_t>(bm.rows);
            width = static_cast<int32_t>(bm.width);
            pitch = static_cast<int32_t>(bm.pitch);
            if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.buffer != nullptr) {
                tight.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
                for (int32_t r = 0; r < height; ++r) {
                    const uint8_t* row = bm.buffer + static_cast<size_t>(r) * pitch;
                    tight.insert(tight.end(), row, row + width);
                }
            }
        }
        const int32_t asc = static_cast<int32_t>(face->size->metrics.ascender);
        const int32_t desc = static_cast<int32_t>(face->size->metrics.descender);

        put32(out, cp);
        put32(out, static_cast<uint32_t>(left));
        put32(out, static_cast<uint32_t>(top));
        put32(out, static_cast<uint32_t>(advance));
        put32(out, static_cast<uint32_t>(asc));
        put32(out, static_cast<uint32_t>(desc));
        put32(out, static_cast<uint32_t>(height));
        put32(out, static_cast<uint32_t>(width));
        put32(out, static_cast<uint32_t>(pitch));
        put32(out, static_cast<uint32_t>(tight.size()));
        out.insert(out.end(), tight.begin(), tight.end());
    }

    std::ofstream f(core::path_from_utf8(out_path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

bool glyph_mask_info(const std::string& font_path, int pixel_size, uint32_t cp,
                     GlyphMaskInfo& out) {
    out = GlyphMaskInfo{};
    const std::string native = core::path_from_utf8(font_path).string();
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return false;
    struct LibGuard {
        FT_Library l;
        ~LibGuard() { FT_Done_FreeType(l); }
    } lib_guard{lib};
    FT_Face face = nullptr;
    if (FT_New_Face(lib, native.c_str(), 0, &face) != 0) return false;
    struct FaceGuard {
        FT_Face f;
        ~FaceGuard() { FT_Done_Face(f); }
    } face_guard{face};
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) return false;

    const FT_UInt index = FT_Get_Char_Index(face, cp);
    if (index == 0) {
        // 字体里没有这个码位 ⇒ 走 .notdef；**判据不是它**，而是渲染出来有没有墨迹
        // （有些字体的 .notdef 是空白，有些是方框；1.x 用 `getbbox() is None` 区分）。
        if (FT_Load_Glyph(face, 0, FT_LOAD_DEFAULT) != 0) return false;
    } else if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0) {
        return false;
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;
    const FT_Bitmap& bm = face->glyph->bitmap;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    out.ok = true;
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY || bm.buffer == nullptr) return true;

    int x0 = out.width;
    int y0 = out.height;
    int x1 = -1;
    int y1 = -1;
    for (int r = 0; r < out.height; ++r) {
        const uint8_t* row = bm.buffer + static_cast<size_t>(r) * bm.pitch;
        for (int c = 0; c < out.width; ++c) {
            if (row[c] == 0) continue;
            if (c < x0) x0 = c;
            if (c > x1) x1 = c;
            if (r < y0) y0 = r;
            if (r > y1) y1 = r;
        }
    }
    if (x1 >= 0) {
        out.has_ink = true;
        out.x0 = x0;
        out.y0 = y0;
        out.x1 = x1 + 1;  // 与 Pillow 的 bbox 一样是**半开区间**
        out.y1 = y1 + 1;
    }
    return true;
}

}  // namespace xrkit::engine::font
