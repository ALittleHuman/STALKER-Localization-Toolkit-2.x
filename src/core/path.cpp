#include "xrkit/core/case_fold.h"
#include "xrkit/core/path.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace xrkit::core {

std::filesystem::path path_from_utf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring wide(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          need);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.generic_wstring();
    if (wide.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
#else
    return path.generic_string();
#endif
}

bool path_equals(const std::string& a, const std::string& b) {
    const std::filesystem::path pa = path_from_utf8(a).lexically_normal();
    const std::filesystem::path pb = path_from_utf8(b).lexically_normal();
    auto ia = pa.begin();
    auto ib = pb.begin();
    for (; ia != pa.end() && ib != pb.end(); ++ia, ++ib) {
        const std::string ca = path_to_utf8(*ia);
        const std::string cb = path_to_utf8(*ib);
#ifdef _WIN32
        // Windows 下路径比较大小写不敏感（1.x 用 os.path.normcase）。
        // H3(b) 2026-11-21：切 **Unicode** 小写折叠（俄文/中文名的大小写同一性也要成立）。
        // 不能先比字节长度：折叠可能改变长度，所以整段折叠后再比。
        if (xrkit::core::case_fold_lower(ca) != xrkit::core::case_fold_lower(cb)) return false;
#else
        if (ca != cb) return false;
#endif
    }
    return ia == pa.end() && ib == pb.end();
}

}  // namespace xrkit::core
