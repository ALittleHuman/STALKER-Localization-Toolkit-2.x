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

// ★★ P17-125（复审计 `P2-8`）：**这里原来在命名空间作用域引了一个 `namespace stdfs` 别名**
//   （`namespace stdfs = std::filesystem;`）—— 头文件里这么做会**泄漏到每个包含它的 TU**，
//   与 `extract_tree.cpp` / `overlay.cpp` 各自那份别名**同名重叠**（"收敛面"）。
//   ⇒ 本头文件**不再引入别名**，一律写 `std::filesystem::`（本文件里只出现两次，不值得为它开别名）。
//
//   ★ 关于复审计同条里的 `inline`：**这个是必需的，不是收敛问题** ——
//     头文件里的**自由函数**必须 `inline`，否则每个包含它的 TU 都会生成一份定义
//     ⇒ 链接期重复定义。如实记下这条，免得下次有人照"复审计原文"把它一起删掉。
//
// ★★ P17-121（复审计 `P2-4`）：**"会跟出去"不再靠两个标签的白名单，改判位**。
//
// 事实（修前）：这里只认 `IO_REPARSE_TAG_MOUNT_POINT`（junction，`0xA0000003`）与
//   `IO_REPARSE_TAG_SYMLINK`（`0xA000000C`）两个值 ⇒ **任何别的"名字代理型"重解析点都漏网**，
//   被 `std::filesystem::is_directory()` 跟随。复审计点名的是 **WSL 的目录重解析点**
//   （`IO_REPARSE_TAG_LX_SYMLINK = 0xA000001D` 等 `LX_*` 族）。
//
// 判据（**判位，不是列清单**）：微软对"**重解析点的名字本身就是解释对象**"（name surrogate）
//   的标记位是 `0x20000000` —— 置位的那些**会把路径解析指到别处**，必须当链接对待；
//   没置位的是**过滤器/元数据型**（WOF 压缩 `0x80000017`、Dedup `0x80000013`、
//   AppExecLink `0x8000001B`、云端分层 `0x9000001A` …）—— 它们**不改变落点**，误判会把正常
//   目录拒掉（`P17-103` 就为这个把 Dedup/OneDrive/WIM 排除在外）。
//
// ★ 这条规则**在本机用能测到的真值核对过**（`FindFirstFileW` 实测）：
//     · junction `0xA0000003` → 置位 ⇒ follow（与实测一致：`is_directory` 确实会跟出去）；
//     · WOF `0x80000017` / Dedup `0x80000013` / AppExecLink `0x8000001B` / Cloud `0x9000001A`
//       → **不**置位 ⇒ 不当链接（与 P17-103 的处置一致）。
//   ⇒ 用位判据的好处：**新出现的名字代理型标签自动被覆盖**（含 WSL 那一族），
//     不必每次去补一份清单（"补一个漏一个"正是这条审计的来路）。
//
// ★ 边界（如实写）：本机**没装 WSL**（`wsl --status` 报"未安装用于 Linux 的 Windows 子系统"）
//   ⇒ `LX_*` 那类标签**在本机无法实测**；上面它被归为 follow 是**按官方位定义推出来的**，
//   不是实测结论。判据里对此有自曝（见 `check_loose_entries_guard.py` 的 ⑤ 段）。
// ★★ S-06（第三方只读审计 2026-10-18）：**"判不出来"必须与"不是链接"分开**。
//   修前 `is_link_or_junction` 在 `FindFirstFileW` 失败时直接 `return false` —— 那是 **fail-open**：
//   非法字符、超长路径、权限异常……任何一种"stat 失败"都被读成"这里没有链接，放心写"，
//   守卫在最需要它的那类输入上**静默失效**（审计原话）。
//   现在拆三态：`NotLink` / `IsLink` / `StatFailed`，**只有"路径不存在"才算 NotLink**
//   （那是"等会儿要建的普通目录/文件"）；其它失败一律 `StatFailed`，由**守卫侧**决定怎么处置
//   （`refuse_if_link_traversed` 的选择是**拒绝** —— 守卫就该 fail-closed）。
//   ★ 兼容：`is_link_or_junction()` 保留（等价于 `probe_link()==IsLink`），老调用点不必全改；
//     **安全相关**的调用点要用 `probe_link()` 并显式处理 `StatFailed`。
enum class LinkProbe {
    NotLink,     // 不是"会跟出去"的链接（含"路径不存在"）
    IsLink,      // 软链接 / junction / 名字代理型重解析点
    StatFailed,  // **问不出来**（FindFirstFileW / symlink_status 失败，且不是"不存在"）
};

inline LinkProbe probe_link(const std::filesystem::path& p) {
#if defined(_WIN32)
    std::wstring w = p.wstring();
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    if (w.empty()) return LinkProbe::NotLink;
    WIN32_FIND_DATAW data{};
    ::SetLastError(ERROR_SUCCESS);
    HANDLE h = ::FindFirstFileW(w.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // 只把"真的不存在"读成 NotLink；`ERROR_INVALID_NAME`(123)、`ERROR_ACCESS_DENIED`(5)、
        // `ERROR_FILENAME_EXCED_RANGE`(206) 等一律 StatFailed —— 那是"问不出来"，不是"没有"。
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return LinkProbe::NotLink;
        return LinkProbe::StatFailed;
    }
    ::FindClose(h);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return LinkProbe::NotLink;
    // 名字代理位（name surrogate）：置位 ⇒ 路径解析跟着走 ⇒ 当链接。
    constexpr DWORD kNameSurrogate = 0x20000000u;
    return (data.dwReserved0 & kNameSurrogate) != 0 ? LinkProbe::IsLink : LinkProbe::NotLink;
#else
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::symlink_status(p, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) return LinkProbe::NotLink;
        return LinkProbe::StatFailed;
    }
    return std::filesystem::is_symlink(st) ? LinkProbe::IsLink : LinkProbe::NotLink;
#endif
}

inline bool is_link_or_junction(const std::filesystem::path& p) {
    return probe_link(p) == LinkProbe::IsLink;
}

}  // namespace xrkit::engine::fs
