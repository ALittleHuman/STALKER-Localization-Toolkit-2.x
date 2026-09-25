#include "xrkit/engine/font/glyph.h"
#include "xrkit/engine/font/metrics.h"

#include "xrkit/engine/text/encoding.h"

#include <array>
#include <set>
#include <string>
#include <vector>

namespace xrkit::engine::font {

const char* cjk_sample() {
    // 1.x `CJK_SAMPLE`（**判据的一部分，不许随手改**）：必须混入**简体特有**的字 ——
    // 老样本"永你好汉化测"里 永/你/好/化 日文字体也有，于是"不同 bbox 数 > 1"
    // 照样判 ✓ 中文，而预览里那几个字是整字宽空洞（用户实测的矛盾）。
    return "永你好汉化测试这说们书见风";
}

const char* cjk_state_mark(CjkFontState state) {
    switch (state) {
        case CjkFontState::Ok:
            return "✓ 中文";
        case CjkFontState::Japanese:
            return "⚠ 日语字体（缺简体→用繁体）";
        case CjkFontState::Missing:
            return "⚠ 疑似缺字形";
        case CjkFontState::Unknown:
            return "? 检测失败";
    }
    return "? 检测失败";
}

namespace {

std::vector<uint32_t> sample_codepoints() {
    std::vector<uint32_t> out;
    const std::string text = cjk_sample();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    size_t pos = 0;
    uint32_t cp = 0;
    while (pos < text.size() && text::utf8_next(data, text.size(), pos, cp)) {
        out.push_back(cp);
    }
    return out;
}

std::string utf8_of(uint32_t cp) {
    std::string out;
    text::utf8_append(out, cp);
    return out;
}

}  // namespace

CjkFontCheck classify_cjk_font(const std::string& font_path, int pixel_size) {
    CjkFontCheck check;
    const std::vector<uint32_t> cps = sample_codepoints();
    std::set<std::array<int, 4>> boxes;
    bool saw_missing = false;

    for (uint32_t cp : cps) {
        GlyphMaskInfo info;
        if (!glyph_mask_info(font_path, pixel_size, cp, info) || !info.ok) {
            // 检测失败（字体打不开/渲染不了）—— 与"缺字形"**不是一回事**
            check.state = CjkFontState::Unknown;
            return check;
        }
        if (!info.has_ink) {
            // 1.x 在**第一个**缺字处就短路：先把"缺哪些字 / 哪些能用繁体顶"算全，再定态
            saw_missing = true;
            break;
        }
        boxes.insert({info.x0, info.y0, info.x1, info.y1});
    }

    if (saw_missing) {
        // `sample_missing` + `sample_fallback`：缺的字里有哪些能用繁体字形顶上
        //（判据是该字在表里有繁体替代**且字体真画得出那个繁体**）
        for (uint32_t cp : cps) {
            GlyphMaskInfo info;
            if (!glyph_mask_info(font_path, pixel_size, cp, info) || !info.ok || info.has_ink) {
                continue;
            }
            const std::string simplified = utf8_of(cp);
            check.missing.push_back(simplified);
            const uint32_t alt = st_fallback(cp);
            if (alt == 0) continue;
            GlyphMaskInfo alt_info;
            if (glyph_mask_info(font_path, pixel_size, alt, alt_info) && alt_info.ok &&
                alt_info.has_ink) {
                check.fallbacks[simplified] = utf8_of(alt);
            }
        }
        // ★ 照**代码**：`return "japanese" if sample_fallback(path) else "missing"` ——
        //   非空即可（只要有一个缺字能补就判 japanese），不是"全都能补"。
        check.state = check.fallbacks.empty() ? CjkFontState::Missing : CjkFontState::Japanese;
        return check;
    }

    // 样本里每个字都画得出 ⇒ 还要看"是不是画什么都一样"（notdef 方块）
    check.state = boxes.size() > 1 ? CjkFontState::Ok : CjkFontState::Missing;
    return check;
}

}  // namespace xrkit::engine::font
