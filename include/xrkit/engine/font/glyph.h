#pragma once
// xrkit::engine::font —— 字形栅格（M1-7）。
//
// 现在的入口只有"探针导出"：M1-7 的第一件事是确认**我们能不能逐字节复现 Pillow 的
// `font.getmask(ch, mode="L")`**（1.x 的字库产物是 Pillow 12.3.0 做的，它捆绑的
// FreeType 正是 2.14.3，与本工程 vendored 的版本一致）。探针不一致就没必要往下写
// 渲染管线 —— 先把 flags 对齐。
//
// 容器格式（小端，定长，便于 Python 侧逐字节对拍）：
//   u32 magic 'GLYP' | u32 pixel_size | u32 count
//   每条：u32 cp, i32 left, i32 top, i32 advance_26_6, i32 ascender_26_6,
//         i32 descender_26_6, i32 height, i32 width, i32 pitch, u32 len, bytes(len)

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::font {

// 用内置样本字符表（常用汉字 + 标点 + Latin + 西里尔）导出栅格结果。
// `cps` 非空时改用它（顺序即输出顺序）—— 对拍/排障时要能指定任意字符。
// 返回 false 只在"字体打不开 / 字号设置失败 / 写不出文件"时。
bool dump_glyphs(const std::string& font_path, int pixel_size,
                 const std::vector<uint32_t>& cps, const std::string& out_path);

// 单个码位的掩码信息（与 Pillow `font.getmask(ch, mode="L")` 同一栈）。
// `has_ink=false` 对应 Pillow 的 `getmask(ch).getbbox() is None` —— 即"这个字在这个
// 字体里画出来是**空的**"（⑪11.8 的判据就是它，不是"字体里有没有这个码位"：
// 有些字体的 .notdef 本身就是空白）。
struct GlyphMaskInfo {
    bool ok = false;        // 渲染这一步成功（字体能打开、码位能设置）
    bool has_ink = false;   // 掩码里有非零像素
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 紧致 bbox（无墨迹时全 0）
    int width = 0, height = 0;           // 掩码尺寸（未裁）
};

bool glyph_mask_info(const std::string& font_path, int pixel_size, uint32_t cp,
                     GlyphMaskInfo& out);

}  // namespace xrkit::engine::font
