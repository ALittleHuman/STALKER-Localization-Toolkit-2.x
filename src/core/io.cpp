#include "xrkit/core/io.h"

#include "xrkit/core/path.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace xrkit::core {

bool read_file_capped(const std::string& path_utf8, std::size_t max_bytes, std::string& out,
                      std::string& error) {
    out.clear();
    error.clear();
    const std::filesystem::path native = path_from_utf8(path_utf8);
    std::error_code ec;
    const auto size = std::filesystem::file_size(native, ec);
    if (ec) {
        error = "读不到文件大小：" + path_utf8 + "（" + ec.message() + "）";
        return false;
    }
    if (size > max_bytes) {
        error = "文件超过读取上限：" + path_utf8 + "（" + std::to_string(size) + " 字节 > " +
                std::to_string(max_bytes) + "）";
        return false;
    }
    std::ifstream in(native, std::ios::binary);
    if (!in) {
        error = "打不开文件：" + path_utf8;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        out.clear();
        error = "读取失败：" + path_utf8;
        return false;
    }
    // 读到的字节数与声明大小不符 ⇒ 也报（截断/并发写坏都算）
    if (out.size() != static_cast<std::size_t>(size)) {
        error = "读取长度与大小不一致：" + path_utf8 + "（" + std::to_string(out.size()) + " vs " +
                std::to_string(size) + "）";
        out.clear();
        return false;
    }
    return true;
}

bool read_file_capped(const std::string& path_utf8, std::string& out, std::string& error) {
    return read_file_capped(path_utf8, kMaxWholeFileBytes, out, error);
}

}  // namespace xrkit::core
