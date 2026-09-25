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

LoadResult load_plugin(const std::string& dll_path, const stk_api& api) {
    LoadResult result;
    result.plugin_name = base_name(dll_path);
#ifndef _WIN32
    result.error = "本平台暂不支持插件 DLL（只有 Windows 构建）";
    (void)api;
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
        ::FreeLibrary(handle);
        return result;
    }
    result.ok = true;
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
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0) {
            std::transform(name.begin(), name.end(), name.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            files.push_back(xrkit::core::path_to_utf8(it->path()));
        }
    }
    std::sort(files.begin(), files.end());  // 文件名升序：加载顺序可复现

    for (const std::string& file : files) {
        const std::string name = base_name(file);
        PluginApiAdapter adapter(registry, name, ui);
        const size_t before = registry.registration_count() + registry.registration_errors().size();
        const LoadResult result = load_plugin(file, adapter.api());
        if (!result.ok) {
            errors.push_back({name, result.error});
            registry.add_error(name, result.error);
            core::log(core::Level::Warn, "插件加载失败：" + result.error);
            continue;
        }
        // 适配器活到下一次 load（owned_ 里的字符串指针要一直有效）——这里刻意泄漏一份，
        // 与"不卸载正在被引用的模块"同一条纪律：插件回调随时可能被调用。
        (void)before;
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
