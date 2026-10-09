#pragma once
// xrkit::engine::font —— **字形位图 → 8bpp 的唯读法**（P17-3；审计 02 F-01 High / 02 F-06 Medium）。
//
// 为什么要有它（修前事实）
// ----------------------
// 同一件事在仓库里写了**三份拷贝**，而只有 `glyph.cpp::dump_glyphs` 那一份认
// `FT_PIXEL_MODE_MONO`：
//
//   · `render.cpp::raster_glyph` 的拷贝分支只认 `FT_PIXEL_MODE_GRAY`；
//   · `glyph.cpp::glyph_mask_info` 干脆 `if (pixel_mode != GRAY) return true;`（早退成
//     ok=true / has_ink=false）。
//
// 而 FreeType 的 `FT_LOAD_DEFAULT` 对**带内嵌位图 strike** 的字体会先试
// `FT_LOAD_SBITS_ONLY`（`ftobjs.c`「try to load embedded bitmaps if available」），命中后
// `slot->format == FT_GLYPH_FORMAT_BITMAP`、`FT_Render_Glyph` 直接 no-op 返回 0（它只对轮廓
// 有效）⇒ 上面两处就把 **1bpp MONO 位图**当成"什么都没有"：
//
//   · **出包那条路**：整字判"缺墨迹" ⇒ ini 里槽位框照写、DDS 上空白
//     （实测 simsun 在 S=1 的 13/15 档：97.2% 槽位全空；msyh/arial 0.0~0.1%）；
//   · **定标那条路**：`fit_render_size` 用同一份栅格量墨迹 ⇒ MONO 候选被当成缺字形
//     **提前 break** ⇒ 渲染字号也小一档（simsun@11：1.x fitted=17 / 我们=11）；
//   · **四态判定**（`classify_cjk_font` → `glyph_mask_info`）同一个洞，只是当前取样字号
//     固定 40px、落在 strike 之外 ⇒ 暂不可达（本批一并堵上，免得改字号时变成活雷）。
//
// 口径（照 1.x/Pillow 与 `glyph.cpp` 原有的正确那一份）
// -----------------------------------------------------
//   · MONO 是 1bpp：每行 `(width + 7) / 8` 字节、**最高位是最左像素**，行步进用 `pitch`
//     （**不是** width）；展开成 0/255；
//   · GRAY 逐行原样拷贝（步进同样用 `pitch`）；
//   · 别的 pixel_mode（LCD / BGRA 等）**不认** ⇒ 调用方照旧按"没有可读像素"处理，
//     绝不假装读到了（零静默）。
//
// ★ 修法**不许**改成 `FT_LOAD_NO_BITMAP`：1.x/Pillow 读的就是位图（Pillow 在 ppem 12 给
//   12×12 = strike 高度；强推轮廓只有 12×11）——强推轮廓会让"刚修好的定标"又对不上 1.x。

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xrkit::engine::font {

// 这个 pixel_mode 能不能被本函数读成 8bpp（判据与展开逻辑**同一份**，不许两处各判一套）。
inline bool bitmap_is_readable(const FT_Bitmap& bm) {
    return bm.buffer != nullptr &&
           (bm.pixel_mode == FT_PIXEL_MODE_GRAY || bm.pixel_mode == FT_PIXEL_MODE_MONO);
}

// 取 (x, y) 处的 8bpp 值；**调用方必须先过 `bitmap_is_readable`**（不可读时返回 0 只作兜底）。
inline uint8_t bitmap_pixel_8bpp(const FT_Bitmap& bm, int x, int y) {
    const uint8_t* row = bm.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(bm.pitch);
    if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) return row[x];
    if (bm.pixel_mode == FT_PIXEL_MODE_MONO) {
        const uint8_t byte = row[x >> 3];
        return ((byte >> (7 - (x & 7))) & 1u) != 0 ? 255 : 0;   // MSB 最左
    }
    return 0;
}

// 把整张位图铺成**行主序紧密数组**（每行 `width` 字节，共 `height` 行）。
// 返回 false ⇔ `pixel_mode` 读不了（调用方照旧"没有可读像素"，别当空白成功）。
inline bool bitmap_to_gray_tight(const FT_Bitmap& bm, int width, int height,
                                 std::vector<uint8_t>& out) {
    out.clear();
    if (!bitmap_is_readable(bm) || width <= 0 || height <= 0) return false;
    if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
        out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        for (int r = 0; r < height; ++r) {
            const uint8_t* row =
                bm.buffer + static_cast<std::size_t>(r) * static_cast<std::size_t>(bm.pitch);
            std::copy(row, row + width, out.begin() + static_cast<std::size_t>(r) * width);
        }
        return true;
    }
    out.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int r = 0; r < height; ++r) {
        for (int x = 0; x < width; ++x) out.push_back(bitmap_pixel_8bpp(bm, x, r));
    }
    return true;
}

}  // namespace xrkit::engine::font
