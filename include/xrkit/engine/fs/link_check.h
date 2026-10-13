// -*- coding: utf-8 -*-
// ★★ P17-110（复审计 `01 F-07`）：**"这是不是会跟出去的那种链接"——唯一一份实现**。
//
// 为什么单开一个头：这个判断原来只长在 `extract_tree.cpp` 的匿名命名空间里（P17-103 为
// `01 F-06` 加的），而 **`overlay.cpp:loose_entries` 需要同一判断**（复审计 `01 F-07`：
// 它用 `std::filesystem::is_directory()` —— **跟随**软链接/junction ⇒ 会走出根、且能自指成环）。
// 两处各抄一份判断是这类缺陷的老路（"修一个带出一个"）⇒ 提上来共用。
//
// 口径（P17-103 实测得出，别改回去）：
//   · `std::filesystem::is_symlink()` 在 **Windows 上看不见 junction**（目录联接的重解析点）：
//     实测 `mklink /J` 造出来的目录，`symlink_status()` 报的是 `directory`。
//   · ⇒ Windows 走 **`FindFirstFileW`**：一次调用同时给出属性与**重解析标签**（`dwReserved0`）。
//   · 排除 **Data Dedup / OneDrive / WIM** 那类"不改路径解析"的重解析点（误判会把正常目录拒掉）。
//   · ★ 尾巴上的分隔符会让 `FindFirstFileW` 失败 ⇒ 先剥掉。
//   · ★ 走 `GetFileAttributesW` + `CreateFileW`/`GetFileInformationByHandleEx` 那一版在本机
//     **实测判不出来**（junction 被判成 0，见 P17-103 的探针记录）—— 别改回去。
#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace xrkit::engine::fs {

namespace stdfs = std::filesystem;

inline bool is_link_or_junction(const stdfs::path& p) {
#if defined(_WIN32)
    std::wstring w = p.wstring();
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    if (w.empty()) return false;
    WIN32_FIND_DATAW data{};
    HANDLE h = ::FindFirstFileW(w.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::FindClose(h);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
    return data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT ||
           data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
#else
    return stdfs::is_symlink(stdfs::symlink_status(p));
#endif
}

}  // namespace xrkit::engine::fs
