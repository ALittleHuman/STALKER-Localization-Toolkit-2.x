#include "xrkit/engine/font/resample.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace xrkit::engine::font {
namespace {

// Pillow `PRECISION_BITS = (32 - 8 - 2)`
constexpr int kPrecisionBits = 22;
constexpr int32_t kRoundHalf = 1 << (kPrecisionBits - 1);  // = 1 << 21

// M_PI 在 MSVC 上要 `_USE_MATH_DEFINES` 才有；这里写字面量（与 math.h 的 M_PI 同一个 double）
constexpr double kPi = 3.14159265358979323846;

// Pillow `sinc_filter` / `lanczos_filter`（LANCZOS 的 support = 3.0）
double sinc_filter(double x) {
    if (x == 0.0) return 1.0;
    x *= kPi;
    return std::sin(x) / x;
}

double lanczos_filter(double x) {
    if (-3.0 <= x && x < 3.0) return sinc_filter(x) * sinc_filter(x / 3);
    return 0.0;
}

// Pillow `clip8`：`clip8_lookups[in >> 22]`，表覆盖 −640..639（负 → 0，>255 → 255）。
// 索引越界在 Pillow 里是**读表外**（UB）；这里夹住，在本工程的取值范围里等价。
uint8_t clip8(int32_t in) {
    int idx = in >> kPrecisionBits;
    if (idx < -640) idx = -640;
    if (idx > 639) idx = 639;
    if (idx < 0) return 0;
    return static_cast<uint8_t>(idx > 255 ? 255 : idx);
}

// Pillow `precompute_coeffs` + `normalize_coeffs_8bpc` 的合并实现。
struct Coeffs {
    int ksize = 0;
    std::vector<int> bounds;      // 2 × outSize：(xmin, xmax)
    std::vector<int32_t> kk;      // outSize × ksize（已定点化）
};

Coeffs precompute_coeffs(int in_size, float in0, float in1, int out_size) {
    Coeffs c;
    double scale = static_cast<double>(in1 - in0) / out_size;
    double filterscale = scale;
    if (filterscale < 1.0) filterscale = 1.0;

    const double support = 3.0 * filterscale;  // LANCZOS.support
    const int ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.ksize = ksize;
    c.bounds.assign(static_cast<size_t>(out_size) * 2, 0);

    std::vector<double> kk(static_cast<size_t>(out_size) * static_cast<size_t>(ksize), 0.0);
    const double inv_filterscale = 1.0 / filterscale;
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = static_cast<double>(in0) + (xx + 0.5) * scale;
        double ww = 0.0;
        // ★ 这两行是向零截断的 `(int)` 转换，**不是** floor（Pillow 源码就是这么写的）
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;

        double* k = &kk[static_cast<size_t>(xx) * static_cast<size_t>(ksize)];
        int x = 0;
        for (; x < xmax; ++x) {
            const double w = lanczos_filter((x + xmin - center + 0.5) * inv_filterscale);
            k[x] = w;
            ww += w;
        }
        if (ww != 0.0) {
            for (int i = 0; i < xmax; ++i) k[i] /= ww;
        }
        for (; x < ksize; ++x) k[x] = 0;
        c.bounds[static_cast<size_t>(xx) * 2 + 0] = xmin;
        c.bounds[static_cast<size_t>(xx) * 2 + 1] = xmax;
    }

    c.kk.resize(kk.size());
    const double one = static_cast<double>(1 << kPrecisionBits);
    for (size_t i = 0; i < kk.size(); ++i) {
        const double v = kk[i];
        c.kk[i] = (v < 0) ? static_cast<int32_t>(-0.5 + v * one)
                          : static_cast<int32_t>(0.5 + v * one);
    }
    return c;
}

}  // namespace

void resample_lanczos_8bpc(const uint8_t* src, int sw, int sh,
                           uint8_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    const bool need_horizontal = (dw != sw);
    const bool need_vertical = (dh != sh);
    if (!need_horizontal && !need_vertical) {
        std::memcpy(dst, src, static_cast<size_t>(sw) * static_cast<size_t>(sh));
        return;
    }

    Coeffs cy = precompute_coeffs(sh, 0.0f, static_cast<float>(sh), dh);
    const int ybox_first = cy.bounds[0];
    const int ybox_last = cy.bounds[static_cast<size_t>(dh) * 2 - 2] +
                          cy.bounds[static_cast<size_t>(dh) * 2 - 1];

    std::vector<uint8_t> temp;
    const uint8_t* v_src = src;
    int v_sw = sw;

    if (need_horizontal) {
        const Coeffs cx = precompute_coeffs(sw, 0.0f, static_cast<float>(sw), dw);
        for (int i = 0; i < dh; ++i) cy.bounds[static_cast<size_t>(i) * 2] -= ybox_first;

        const int temp_h = ybox_last - ybox_first;
        temp.assign(static_cast<size_t>(dw) * static_cast<size_t>(temp_h), 0);
        for (int yy = 0; yy < temp_h; ++yy) {
            const uint8_t* in = src + static_cast<size_t>(yy + ybox_first) * sw;
            uint8_t* out = temp.data() + static_cast<size_t>(yy) * dw;
            for (int xx = 0; xx < dw; ++xx) {
                const int xmin = cx.bounds[static_cast<size_t>(xx) * 2 + 0];
                const int xmax = cx.bounds[static_cast<size_t>(xx) * 2 + 1];
                const int32_t* k = &cx.kk[static_cast<size_t>(xx) * cx.ksize];
                int32_t ss = kRoundHalf;
                for (int x = 0; x < xmax; ++x) {
                    ss += static_cast<int32_t>(in[x + xmin]) * k[x];
                }
                out[xx] = clip8(ss);
            }
        }
        v_src = temp.data();
        v_sw = dw;
    }

    if (need_vertical) {
        for (int yy = 0; yy < dh; ++yy) {
            const int ymin = cy.bounds[static_cast<size_t>(yy) * 2 + 0];
            const int ymax = cy.bounds[static_cast<size_t>(yy) * 2 + 1];
            const int32_t* k = &cy.kk[static_cast<size_t>(yy) * cy.ksize];
            uint8_t* out = dst + static_cast<size_t>(yy) * dw;
            for (int xx = 0; xx < v_sw; ++xx) {
                int32_t ss = kRoundHalf;
                for (int y = 0; y < ymax; ++y) {
                    ss += static_cast<int32_t>(
                              v_src[static_cast<size_t>(y + ymin) * v_sw + xx]) * k[y];
                }
                out[xx] = clip8(ss);
            }
        }
    } else if (need_horizontal) {
        std::memcpy(dst, temp.data(), static_cast<size_t>(dw) * static_cast<size_t>(dh));
    }
}

}  // namespace xrkit::engine::font
