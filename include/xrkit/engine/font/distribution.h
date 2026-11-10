#pragma once
// xrkit::engine::font —— 汉化包**分发编排**（base 标准包 + 每个选中档位一个补丁）。
//
// 口径（2026-11-10，用户）：
//   · base：offset=0 的完整语言包；
//   · 位图补丁：`<out>/_patches/font±N`，只含该档位的 DDS+INI；
//   · IX-Ray 矢量补丁：`<out>/_patches/font±N`，只含改过 `size` 的 fonts.ltx；
//   · 任一补丁失败 ⇒ 整体失败（base 已产出，错误点名档位）。
//
// CLI 与 GUI 都只调这一份；不再各自写 base+patch 循环。

#include "xrkit/engine/font/package.h"

#include <string>
#include <vector>

namespace xrkit::engine::font {

struct FontDistributionRequest {
    FontPackRequest base;              // base.out 是输出根；offset 由本函数强制为 0
    std::vector<int> patch_offsets;    // 额外档位（不能含 0）
};

struct FontDistributionPatch {
    int offset = 0;
    bool vector_path = false;
    bool ok = false;
    std::string out;                   // gamedata 目录（位图）或 fonts.ltx 路径（矢量）
    int fonts = 0;                     // 位图写出的 DDS/INI 对数（10）
    int sizes_changed = 0;             // 矢量改过的 size 行数
    std::string error;
};

struct FontDistributionOutcome {
    bool ok = false;
    std::string error;
    FontPackOutcome base;
    std::vector<FontDistributionPatch> patches;
    int patch_count = 0;
};

FontDistributionOutcome build_font_distribution(const FontDistributionRequest& request);

}  // namespace xrkit::engine::font
