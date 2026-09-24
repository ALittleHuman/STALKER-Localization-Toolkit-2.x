#pragma once
// xrkit::core —— 原子写盘（两条写出路径共用的一份实现）。
//
// 为什么做成公共的：解包（`engine::fs::extract_tree`）与编码转换（`engine::text::convert`）
// 都需要同一条纪律 —— **同目录 `.part` → 读回逐字节复核 → 同盘 rename**，
// 任一环失败先 unlink。这条纪律写两份就一定会有一份先腐坏，而它保护的正是
// "宁可不写，也不留一个看起来存在的坏文件"。
//
// 与 1.x 的对应：`sqfs_extract` 的 `open(tmp,"wb")` → `os.replace(tmp, p)`；
// 2.0 多加了一步**读回复核**（写盘被截断只在读回时看得见，`ofstream` 的 `!out` 未必报）。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

// 把 data 写到 path。`atomic=false` 时直接写目标（仅用于明确不要临时文件的场合）。
// 成功返回 true；失败返回 false 并把原因写进 error（调用方负责上报，**不留全局变量**）。
bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, bool atomic,
                       std::string& error);

}  // namespace xrkit::core
