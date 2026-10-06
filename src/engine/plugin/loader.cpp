#include "xrkit/engine/plugin/loader.h"

#include "xrkit/core/log.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/plugin/abi_host.h"

#include <algorithm>
#include <filesystem>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xrkit::plugin {
namespace {

struct Loaded {
    void* handle = nullptr;
    std::string path;
};

std::vector<Loaded>& loaded() {
    static std::vector<Loaded> instance;
    return instance;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool is_plugin_dll_name(const std::string& filename) {
    // ★★ S2-2（B 批）：后缀判定**唯一一份**、**大小写不敏感** —— Windows 文件名不分大小写，
    //   所以 `NLC.DLL` 与 `nlc.dll` 是同一个文件，必须都认（修前只认小写，`NLC.DLL` 静默漏载）。
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0;
}

LoadResult load_plugin(const std::string& dll_path, const PluginApiAdapter& adapter) {
    LoadResult result;
    result.plugin_name = base_name(dll_path);
    const stk_api& api = adapter.api();
#ifndef _WIN32
    result.error = "本平台暂不支持插件 DLL（只有 Windows 构建）";
    return result;
#else
    const std::filesystem::path native = xrkit::core::path_from_utf8(dll_path);
    HMODULE handle = ::LoadLibraryW(native.wstring().c_str());
    if (handle == nullptr) {
        result.error = "LoadLibrary 失败（错误码 " + std::to_string(::GetLastError()) + "）：" +
                       dll_path;
        return result;
    }
    // 入口名固定；找不到入口 = 不是插件（**不当作致命错误**，只记录）
    using InitFn = int (*)(const stk_api*);
    auto init = reinterpret_cast<InitFn>(
        reinterpret_cast<void*>(::GetProcAddress(handle, STK_PLUGIN_ENTRY)));
    if (init == nullptr) {
        result.error = std::string("DLL 里没有入口 ") + STK_PLUGIN_ENTRY + "：" + dll_path;
        ::FreeLibrary(handle);
        return result;
    }
    const int rc = init(&api);
    result.init_rc = rc;
    if (rc == 0) {
        result.error = "插件入口返回 0（它自己说初始化失败）：" + dll_path;
        // ★★ S1-2：**先按 owner 回滚它这次 init 期间的全部注册，然后才 FreeLibrary**。
        //   顺序反了就没意义：库一放，表里那些裸函数指针就已经指向 unmap 掉的代码段
        //   （之后 `Registry::invoke` 之外的任何消费都是 AV —— 实测 0xC0000005）。
        //   owner 名取**适配器自己的**（与它注册时用的前缀是同一个）⇒ 不会撤错别家。
        result.registrations = adapter.registry().rollback_plugin(adapter.plugin_name());
        core::log(core::Level::Warn, "插件 init 返回 0，已按 owner 回滚 " +
                                         std::to_string(result.registrations) + " 条注册：" +
                                         adapter.plugin_name());
        ::FreeLibrary(handle);
        return result;
    }
    result.ok = true;
    // ★★ 2026-10-06（审计第 4 组）：**成功路径也要填 `registrations`** —— 修前只有失败
    //   分支（`init` 返回 0 回滚）写它，成功时恒为 0 ⇒ 这个字段对成功加载**是死的**
    //   （`tools/cli` 的 `--plugin-init-rollback` 探针打出来的 `registrations=` 因此
    //   永远只在失败那一路有值）。口径与失败分支**同一份实现**：按 owner 逐容器报账。
    result.registrations = adapter.registry().residue_for(adapter.plugin_name()).registrations();
    loaded().push_back({reinterpret_cast<void*>(handle), dll_path});
    return result;
#endif
}

std::vector<RegistrationError> load_plugins_from_dir(const std::string& dir, Registry& registry,
                                                     const stk_ui* ui) {
    std::vector<RegistrationError> errors;
    std::error_code ec;
    const std::filesystem::path root = xrkit::core::path_from_utf8(dir);
    if (!std::filesystem::is_directory(root, ec)) {
        errors.push_back({"<loader>", "插件目录不存在：" + dir});
        // 目录不存在**不算启动失败** —— 只记录，宿主照常起来
        return errors;
    }
    std::vector<std::string> files;
    for (std::filesystem::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string name = xrkit::core::path_to_utf8(it->path().filename());
        // ★★ S2-2（B 批）：后缀判定**只此一份**且**大小写不敏感**（见 loader.h 的说明）。
        //   修前：这里判原名的 `.dll`（`NLC.DLL` 静默漏载），而给它小写化的
        //   `std::transform` 结果被丢弃（死代码）。
        if (is_plugin_dll_name(name)) {
            files.push_back(xrkit::core::path_to_utf8(it->path()));
        }
    }
    std::sort(files.begin(), files.end());  // 文件名升序：加载顺序可复现

    for (const std::string& file : files) {
        const std::string name = base_name(file);
        PluginApiAdapter adapter(registry, name, ui);
        // ★★ S1-2：这个 mark 是**对账**用的（回滚后条数必须回到加载前），**不是**回滚手段 ——
        //   回滚走 `Registry::rollback_plugin(owner)`（按 owner 逐容器撤）：`registration_count()`
        //   是 9 个容器的**总和**（总量 ≠ 归属），撤错了别家的也能让总数对上。
        //   `errors_` 不计入总数，所以对账时把它单独加上（留痕照旧要写）。
        const size_t before = registry.registration_count() + registry.registration_errors().size();
        const LoadResult result = load_plugin(file, adapter);
        if (!result.ok) {
            errors.push_back({name, result.error});
            registry.add_error(name, result.error);
            // ★ 回滚自检（零静默）：回滚已经在 `load_plugin` 里、FreeLibrary **之前**跑过了。
            //   这里只按"加载前 + 刚写的那条留痕"对账：不等 ⇒ 回滚没撤干净（或撤多了），
            //   如实再记一条 —— 绝不静默。
            const size_t after =
                registry.registration_count() + registry.registration_errors().size();
            if (after != before + 1) {
                registry.add_error(name, "回滚后注册表条数与加载前对不上（加载前 " +
                                             std::to_string(before) + "，加载后 " +
                                             std::to_string(after) + "，已回滚 " +
                                             std::to_string(result.registrations) + " 条）");
            }
            core::log(core::Level::Warn, "插件加载失败：" + result.error);
            continue;
        }
        // 适配器活到下一次 load（owned_ 里的字符串指针要一直有效）——这里刻意泄漏一份，
        // 与"不卸载正在被引用的模块"同一条纪律：插件回调随时可能被调用。
        new PluginApiAdapter(std::move(adapter));
    }
    return errors;
}

void unload_all() {
#ifdef _WIN32
    for (const Loaded& item : loaded()) {
        if (item.handle != nullptr) ::FreeLibrary(reinterpret_cast<HMODULE>(item.handle));
    }
#endif
    loaded().clear();
}

size_t loaded_count() { return loaded().size(); }

}  // namespace xrkit::plugin
