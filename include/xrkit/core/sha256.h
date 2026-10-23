#pragma once
// xrkit::core —— **SHA-256**（无 Qt、无第三方依赖；core/engine 的共同底座）。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

// 对一段字节算 SHA-256，返回**小写十六进制**（64 字符）。空输入也合法。
std::string sha256_hex(const uint8_t* data, size_t size);
inline std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    return sha256_hex(bytes.data(), bytes.size());
}

// 对**文件**算 SHA-256（流式读，1 MiB 一块；大包也不吃内存）。
// 失败（打不开/读错）⇒ 返回空串并把理由写进 `error`。
std::string sha256_file(const std::string& path, std::string& error);

}  // namespace xrkit::core
