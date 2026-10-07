// xrkit::plugin —— **引擎调用门面的默认表**（插件改装 P7a，用户拍板的方案 3）。
//
// ★ 这份实现住在**引擎**层（不是外壳）：它只调用 `xrkit::engine::*`，**不依赖 Qt、也不依赖
//   外壳的任何服务**；而 CLI（`xrkit_cli.exe --py-plugins`）同样是插件宿主 —— 实现在外壳里时
//   实测 CLI 里的插件调 `font_size_slots()` 拿到的是 None，而"让 CLI 也装上门面"就得让 CLI
//   链接外壳 ⇒ 把 Qt 拖进 CLI（J5 那条"引擎不许 include Qt"的意义就没了）。
// ⇒ 默认表放引擎、外壳可 `set_engine_facade()` 覆盖/扩展。
//
// 本文件只做**转发**（门面签名 → 引擎函数），**不在这一层加业务逻辑**：阈值/默认值/文案一律
// 从引擎拿，别在这里抄第二份 —— 本仓已经因为"旁边再写一份差不多的"被门禁拦下过两次。
// 枚举一律**转成字符串**（"ok"/"japanese"/"missing"/"unknown"）：数字枚举一变序就静默错。

#include "xrkit/engine/plugin/engine_facade.h"

#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"

#include <string>
#include <vector>

namespace xrkit::plugin {
namespace {

namespace efont = xrkit::engine::font;

const char* state_name(efont::CjkFontState state) {
    switch (state) {
        case efont::CjkFontState::Ok:
            return "ok";
        case efont::CjkFontState::Japanese:
            return "japanese";
        case efont::CjkFontState::Missing:
            return "missing";
        case efont::CjkFontState::Unknown:
            break;
    }
    return "unknown";
}

const EngineFacade kFontFacade = {
    /* font_check */
    [](const std::string& font_path) {
        FontCheckResult out;
        const efont::CjkFontCheck check = efont::classify_cjk_font(font_path);
        out.state = state_name(check.state);
        const char* mark = efont::cjk_state_mark(check.state);
        out.mark = mark == nullptr ? std::string() : std::string(mark);
        out.missing = check.missing;
        return out;
    },
    /* font_pack：与内置页**同一个**引擎实现（逐字节一致的前提） */
    [](const FontPackOptions& options) {
        efont::PackageOptions native;
        native.game = options.game;
        native.lang = options.lang;
        native.xml_dir = options.xml_dir;
        native.font_path = options.font_path;
        native.out_dir = options.out_dir;
        native.offset = options.offset;
        native.suffix = options.suffix;
        native.full_cyrillic = options.full_cyrillic;
        native.only_sizes = options.only_sizes;
        const efont::PackageResult result = efont::build_package(native);
        FontPackResult out;
        out.ok = result.ok;
        out.error = result.error;
        out.out_gamedata = result.out_gamedata;
        out.fonts_written = result.fonts_written;
        out.xml_written = result.xml_written;
        out.warnings = result.warnings;
        return out;
    },
    /* font_state_marks：四态文案**从引擎拿**（插件不许自己抄一份中文） */
    []() {
        FontStateMarks out;
        out.ok = efont::cjk_state_mark(efont::CjkFontState::Ok);
        out.japanese = efont::cjk_state_mark(efont::CjkFontState::Japanese);
        out.missing = efont::cjk_state_mark(efont::CjkFontState::Missing);
        out.unknown = efont::cjk_state_mark(efont::CjkFontState::Unknown);
        return out;
    },
    /* font_size_slots：字号槽位表（文件名与 ltx 用的是槽位名） */
    []() {
        std::vector<int> sizes;
        const int count = efont::slot_count();
        if (count > 0) sizes.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) sizes.push_back(efont::slot_size(i));
        return sizes;
    },
};

}  // namespace

// 引擎自带的默认表（惰性建出来；外壳可以 set_engine_facade() 覆盖它）
const EngineFacade* builtin_engine_facade() {
    static const EngineFacade instance = kFontFacade;
    return &instance;
}

}  // namespace xrkit::plugin
