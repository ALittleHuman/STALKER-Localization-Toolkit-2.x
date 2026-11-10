#include "xrkit/engine/font/distribution.h"

#include <algorithm>
#include <string>

namespace xrkit::engine::font {

FontDistributionOutcome build_font_distribution(const FontDistributionRequest& request) {
    FontDistributionOutcome out;
    if (request.base.out.empty()) {
        out.error = "缺输出目录";
        return out;
    }
    if (!request.patch_offsets.empty() && !request.base.only_sizes.empty()) {
        out.error = "字号补丁必须覆盖全部槽位，不能与 only_sizes 同用";
        return out;
    }

    FontPackRequest base = request.base;
    base.offset = 0;
    base.patch_only = false;
    out.base = build_font_pack(base);
    if (!out.base.ok) {
        out.error = out.base.error;
        return out;
    }
    if (request.patch_offsets.empty()) {
        out.ok = true;
        return out;
    }

    std::vector<int> wanted;
    for (int off : request.patch_offsets) {
        if (off == 0) {
            out.error = "补丁档位不能是 0（0 就是标准 base）";
            return out;
        }
        if (std::find(wanted.begin(), wanted.end(), off) == wanted.end()) {
            wanted.push_back(off);
        }
    }

    for (int off : wanted) {
        FontDistributionPatch patch;
        patch.offset = off;
        patch.out = base.out + "/_patches/font" + (off >= 0 ? "+" : "") +
                    std::to_string(off);
        if (out.base.vector_path) {
            FontSizePatchRequest req;
            req.ref = base.ref;
            req.offset = off;
            req.out = patch.out;
            const FontSizePatchResult res = build_font_size_patch(req);
            patch.vector_path = true;
            patch.ok = res.ok;
            patch.out = res.out_file;
            patch.sizes_changed = res.sizes_changed;
            patch.error = res.error;
        } else {
            FontPackRequest req = base;
            req.patch_only = true;
            req.offset = off;
            req.out = patch.out;
            const FontPackOutcome res = build_font_pack(req);
            patch.vector_path = false;
            patch.ok = res.ok;
            patch.out = res.bitmap.out_gamedata;
            patch.fonts = res.bitmap.fonts_written;
            patch.error = res.error;
        }
        if (!patch.ok) {
            out.error = "字号补丁 " + std::to_string(off) + " 生成失败：" + patch.error;
            out.patches.push_back(std::move(patch));
            return out;
        }
        out.patches.push_back(std::move(patch));
        ++out.patch_count;
    }
    out.ok = true;
    return out;
}

}  // namespace xrkit::engine::font
