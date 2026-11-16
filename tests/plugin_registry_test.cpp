// 回归测试：注册表 ∖ 本地 的合并逻辑（纯 C++，不依赖 Qt，可单独编译）。
//
// 覆盖的关键行为：
//   1. 本地已装的条目不进"可安装"列表；
//   2. 本地没装的条目进"可安装"列表，且保持注册表顺序；
//   3. 选中 id 为空 ⇒ 取注册表第一条（平台原行为）；
//   4. 选中 id ⇒ 精确匹配；匹配不到 ⇒ false；
//   5. target 缺省取 id。
#include "xrkit/core/plugin_registry.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void want(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}

std::vector<xrkit::core::RegistryEntry> make_registry() {
    std::vector<xrkit::core::RegistryEntry> r;
    xrkit::core::RegistryEntry a;
    a.id = "ai_translate";
    a.version = "2.0.1";
    a.package = "https://example.invalid/ai_translate_plugin_v2.0.1_install.zip";
    a.sha256 = "555aef58610a091d4b2031fa3ad83e06b81a8ebec7b2f26f25e283d47da7754b";
    a.entry = "plugin.py";
    a.target = "ai_translate";
    r.push_back(a);
    xrkit::core::RegistryEntry b;
    b.id = "other_plugin";
    b.version = "1.2.3";
    b.package = "https://example.invalid/other.zip";
    b.entry = "plugin.py";  // target 留空 ⇒ 缺省取 id
    r.push_back(b);
    return r;
}

}  // namespace

int main() {
    const auto registry = make_registry();

    // 本地只装了 ai_translate
    std::vector<xrkit::core::LocalPluginRef> local;
    local.push_back({"ai_translate", "2.0.0"});

    const auto missing = xrkit::core::registry_missing(local, registry);
    want(missing.size() == 1, "本地已装的条目不进可安装列表");
    want(!missing.empty() && missing[0].id == "other_plugin",
         "本地没装的条目进可安装列表");
    want(!missing.empty() && missing[0].version == "1.2.3", "版本号原样带出");
    want(!missing.empty() && missing[0].effective_target() == "other_plugin",
         "target 缺省取 id");

    // 全空的本地 ⇒ 两条都可安装，顺序与注册表一致
    const auto missing_all =
        xrkit::core::registry_missing({}, registry);
    want(missing_all.size() == 2, "空本地列表 ⇒ 全部可安装");
    want(missing_all.size() == 2 && missing_all[0].id == "ai_translate" &&
             missing_all[1].id == "other_plugin",
         "可安装列表保持注册表顺序");

    // 选中 id 为空 ⇒ 取第一条
    xrkit::core::RegistryEntry picked;
    want(xrkit::core::registry_pick(registry, "", &picked) &&
             picked.id == "ai_translate",
         "选中 id 为空 ⇒ 取注册表第一条");

    // 选中 id ⇒ 精确匹配
    want(xrkit::core::registry_pick(registry, "other_plugin", &picked) &&
             picked.version == "1.2.3",
         "选中 id ⇒ 精确匹配");

    // 匹配不到 ⇒ false
    want(!xrkit::core::registry_pick(registry, "not_installed", &picked),
         "匹配不到 ⇒ false");

    // 空 id 的注册表条目不进可安装列表（防止装到无名目录）
    std::vector<xrkit::core::RegistryEntry> bad;
    xrkit::core::RegistryEntry blank;
    blank.version = "9.9.9";
    bad.push_back(blank);
    want(xrkit::core::registry_missing({}, bad).empty(),
         "空 id 的条目不进可安装列表");

    std::printf(g_failed == 0 ? "PLUGIN-REGISTRY-TEST ok\n"
                              : "PLUGIN-REGISTRY-TEST failed=%d\n",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}
