// xrkit::plugin —— **宿主服务门面的默认表**（2026-10-07 P7b）。
//
// ★ 住在**引擎**层：这四件（偏好读写 / exe 目录 / 系统字体目录）只依赖引擎与操作系统，
//   不依赖 Qt、也不依赖外壳 ⇒ CLI（`xrkit_cli.exe --py-plugins`）与 GUI 两个宿主都能用。
//   （同 P7a 的教训：把只依赖引擎的东西放外壳，CLI 那个宿主就用不到。）

#include "xrkit/engine/plugin/host_facade.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/font/system_dirs.h"

#include <string>
#include <vector>

namespace xrkit::plugin {
namespace {

namespace core = xrkit::core;
namespace efont = xrkit::engine::font;

const HostFacade kHostFacade = {
    /* pref_get：读 `user.ltx` 的 `[section] key`（缺失返回 fallback） */
    [](const std::string& section, const std::string& key, const std::string& fallback) {
        return core::load_user_value(section, key, fallback);
    },
    /* pref_set：只写点名的那一项（不整块覆盖别的键） */
    [](const std::string& section, const std::string& key, const std::string& value) {
        return core::save_user_value(section, key, value);
    },
    /* app_dir：exe 所在目录（发行版放 ffmpeg/ffprobe 的地方、`user.ltx` 的落点） */
    []() { return core::app_dir(); },
    /* system_font_dirs：唯一一份（内置页与插件共用；逐条照 1.x） */
    []() { return efont::system_font_dirs(); },
};

}  // namespace

const HostFacade* builtin_host_facade() {
    static const HostFacade instance = kHostFacade;
    return &instance;
}

}  // namespace xrkit::plugin
