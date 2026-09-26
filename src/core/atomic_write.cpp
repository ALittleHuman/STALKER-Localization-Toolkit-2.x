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
    // ★ 临时文件路径**必须全程走宽串**：`Path::string()` 在 Windows 上按 **CP_ACP** 转窄串，
    // ★ 2026-09-27 更正（我自己的实测）：**俄文在 CP936 下能无损编码**（`файл` → `a7e6a7d1a7dba7dd`）；真正会退化成 `?` 的是 **CP_ACP 表示不了的字符**——韩文、日文假名、emoji、Ё 等。早先注释写"俄文会变 ?"是错的，别再照它复刻。
    //   ⇒ 临时文件打不开、原子写**必然失败**（2026-09-25 HanaAgent 审计查出；我们的逐字节语料
    //   路径全是 ASCII，所以一直没暴露）。用 `+= L".part"` 保住 native 宽串。
    stdfs::path temp = target;
    if (atomic) temp += L".part";

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
