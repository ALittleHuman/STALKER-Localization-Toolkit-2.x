#pragma once
// xrkit::engine::font —— 字库**渲染管线**（M1-7 第④⑤步）：1.x `render_font` + `write_dds` 的等价实现。
//
// 判据：同输入产物**逐字节一致**（`ui_font_NN.dds` + `ui_font_NN.ini`）。所以这里逐句照抄
// `font_pack.render_font`，一步都不"优化"：
//
//   ① `fit_render_size`：候选字号 `size..size+13` 取最大的、对样本每字满足
//      `墨迹高 ≤ cell−4` 且 `墨迹宽 ≤ min(推进宽, cell)`；
//   ② `vshift = max(0, cell − (asc+desc)//S)`（`font.getmetrics()` 的 ascent/descent）；
//   ③ 画布：`tex_w` 1024→2048→4096；`max_y = rows*cell`；`tex_h` = ≥max_y 的 2 幂；
//   ④ 逐字：画布宽 = advance 像素、高 = max(墨迹行数, bitmap_top)、墨迹贴 `(bitmap_left, 0)`；
//      `mb` = 该画布的**非零紧致 bbox**（首尾全零行列要裁掉）；缺墨迹 → 简繁表顶；
//   ⑤ 宽度裁决顺序：`rect_w_for/advance` → `punct_box_px` → `clamp_box_to_ink` → 封顶 cell
//      → `ink_draw_w`（见 metrics.h 的 `adjudicate_box`，与探针共用一处）；
//   ⑥ 纵向：`top = bbox_y0//S + vshift`；装不下先整体上移，再装不下才裁进行带；
//   ⑦ 横向：`x = col*cell + (cell−rw0)//2 + left0 − pl`，`paste(glyph, (x+pl, y+top))`；
//   ⑧ ini 行 `f"{cp:05d}= {x}, {y}, {x+rw}, {y+cell}"`；`8888` 每字节 → `(v,v,v,v)`；DDS 头照抄。
//
// ★ 两处已实测的 Pillow 口径（见 `docs/2.0.0-字库模块规格.md` §2.1）：
//   · `mask` 画布：宽 = advance 像素、高 = `max(bitmap_rows, bitmap_top)`、墨迹在 `(bitmap_left, 0)`；
//   · `font.getbbox(ch)` 的 **y0 = ascent 像素 − bitmap_top**（`getbbox` 的 y1 与墨迹无关，
//     1.x 只用到 y0）。
// ★ Python 的 `//` 是**向下取整**，C++ 的 `/` 是向零截断 —— 负数会差 1（如 `-3//2 = -2`
//   而 `-3/2 = -1`）。凡是给 `//` 当分母 2 的地方（`bbox_y0//S`、`(asc+desc)//S`）一律走
//   `floor_div`。
//
// 本模块依赖 FreeType（vendored 2.14.3 = Pillow 12.3.0 捆绑的版本）。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xrkit::engine::font {

// 一个字号的产物。
struct FontProduct {
    std::vector<uint8_t> dds;  // 完整 DDS（含 128 字节 legacy 头）
    std::string ini;           // ini 正文（LF；1.x 用文本模式落盘 ⇒ 文件里是 CRLF）
    int tex_w = 0;
    int tex_h = 0;
    int max_y = 0;             // 行带总高（= 行数 × cell）
    int fitted_size = 0;       // 按墨迹定标后的渲染字号（= 1.x fit_render_size 的返回）
    int clipped = 0;           // 墨迹超出槽位高、被裁进行带的字符数（1.x 会告警）
    std::vector<std::pair<uint32_t, uint32_t>> substituted;  // (原字, 用来顶的繁体字)
    std::string error;         // 非空 = 失败（字体打不开 / 放不下 / 写不出）
};

// `chars` 的顺序**即槽位顺序**（与 1.x 的 `chars` 字符串一致，重复码位也照排）。
// `render_size` 是 1.x 的 `size` 参数（`build_package` 里 = 槽位名 + offset）；
// `x2` = 该档是否超采样（1.x `SIZES` 的第二个字段）；`fmt` = "8888" 或 "A8"。
bool render_font(const std::string& font_path, const std::vector<uint32_t>& chars,
                 int render_size, bool x2, const std::string& fmt, FontProduct& out);

// `\n` → `\r\n`（**每一个** `\n` 都翻，与 Python 文本模式的语义一致 —— 不是"已经是 CRLF
// 就跳过"）。1.x 用 `open(path, "w", encoding=...)` 写 .ini/.ltx，Windows 文本模式会把
// `\n` 翻成 `\r\n` —— 产物逐字节一致必须复刻这一步。
std::string to_windows_newlines(const std::string& text);

// ── 字体自检（出包前的守卫，= 1.x `font_missing_chars` / `font_unrenderable`）──
//
// 判据与 1.x 相同：把某字的掩码字节与私用区码位 `U+E000` 的掩码逐字节比 —— 一样就说明
// 字体画的是 `.notdef` 方框。**没有这两道守卫，用错字体会得到十张全是同一个方框的图集，
// 而工具照样报"完成"。**
//
// `font_missing_chars`：**按类别成组抽样**（CJK / 西里尔 / "A"），整组都画不出才算该类别
// 缺失 → 用于**拒绝**废字体；`font_unrenderable`：逐字判定 → 用于**逐字告警**
// （日文字体缺 Є І Ї 这类西里尔扩展是常态，不该拒绝整包）。
std::vector<uint32_t> font_missing_chars(const std::string& font_path,
                                         const std::vector<uint32_t>& chars);
std::vector<uint32_t> font_unrenderable(const std::string& font_path,
                                        const std::vector<uint32_t>& chars);

}  // namespace xrkit::engine::font
