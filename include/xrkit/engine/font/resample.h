#pragma once
// xrkit::engine::font::resample —— Pillow `Image.resize(..., LANCZOS)` 的等价实现（M1-7 第⑥步）。
//
// 为什么单独一个模块：x2 档（16/17/18）的产物只差这一步，而它是"看着像、实则全错"的高危点
// （定点系数、边界裁剪、两趟顺序、取整口径差一点就整片像素不同）。单独拆出来才能**孤立对拍**：
// `xrkit_cli --resample-dump` 直接吃一张原始 8 位图，与 Pillow 的 `resize` 逐字节比。
//
// 逐句照抄 `Pillow-12.3.0/src/libImaging/Resample.c` 的 **8bpc 单通道**路径：
//   · `precompute_coeffs`：`scale = (in1−in0)/outSize`、`filterscale = max(scale, 1)`、
//     `support = 3 * filterscale`（LANCZOS）、`ksize = ceil(support)*2 + 1`；
//     `center = in0 + (xx+0.5)*scale`、`xmin = (int)(center−support+0.5)`（**向零截断**，
//     不是 floor）、`xmax = min((int)(center+support+0.5), inSize)`；系数按
//     `lanczos((x+xmin−center+0.5)/filterscale)` 算完再除以和（`ww != 0` 才归一）；
//   · `normalize_coeffs_8bpc`：定点 `PRECISION_BITS = 32−8−2 = 22`，
//     正数 `(int)(0.5 + v*2^22)`、负数 `(int)(−0.5 + v*2^22)`；
//   · 累加：`ss = 1 << 21` 起头，逐项加 `像素 * 系数`（`int32`），最后 `clip8(ss >> 22)`
//     夹到 0..255（等价于 Pillow 那张 −640..639 的查表）；
//   · 两趟顺序：**先横后竖**；横向那趟只处理源图 `[ybox_first, ybox_last)` 这些行，
//     竖向那趟的 bounds 相应左移 `ybox_first`；
//   · `box` 恒为 `(0, 0, 源宽, 源高)`（1.x 调 `resize` 没给 box）。

#include <cstdint>

namespace xrkit::engine::font {

// 把 8 位单通道图从 (sw×sh) 重采样到 (dw×dh)。`src`/`dst` 必须不重叠，行距 = 宽度。
void resample_lanczos_8bpc(const uint8_t* src, int sw, int sh,
                           uint8_t* dst, int dw, int dh);

}  // namespace xrkit::engine::font
