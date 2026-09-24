#include "xrkit/core/atomic_write.h"

#include "xrkit/core/path.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace xrkit::core {
namespace {

namespace stdfs = std::filesystem;

}  // namespace

bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, bool atomic,
                       std::string& error) {
    error.clear();
    const stdfs::path target = path_from_utf8(path);
    std::error_code ec;
    const stdfs::path dir = target.parent_path();
    if (!dir.empty()) {
        stdfs::create_directories(dir, ec);
        if (ec) {
            error = "建目录失败：" + ec.message();
            return false;
        }
    }
    const stdfs::path temp = atomic ? stdfs::path(target.string() + ".part") : target;

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "打不开输出文件";
            return false;
        }
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        out.close();
        if (!out) {
            error = "写盘失败（磁盘满/被占用）";
            if (atomic) stdfs::remove(temp, ec);
            return false;
        }
    }

    if (!atomic) return true;

    // 逐字节复核：写盘被截断（磁盘满、被占用）只在**读回**时看得见
    {
        std::ifstream in(temp, std::ios::binary);
        if (!in) {
            error = "复核读不回临时文件";
            stdfs::remove(temp, ec);
            return false;
        }
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        if (back.size() != data.size() ||
            (data.size() != 0 && std::memcmp(back.data(), data.data(), data.size()) != 0)) {
            error = "复核不一致（临时文件与待写内容不同）";
            stdfs::remove(temp, ec);
            return false;
        }
    }

    // 同盘 rename（1.x 是 `os.replace`）：要么旧文件还在，要么新文件完整
    stdfs::rename(temp, target, ec);
    if (ec) {
        error = "替换目标文件失败：" + ec.message();
        stdfs::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace xrkit::core
