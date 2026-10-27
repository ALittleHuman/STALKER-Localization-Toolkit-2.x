#pragma once
// xrkit::engine::font::ixr —— **IX-Ray 矢量包生成**（产品入口）。
//
// 事实（用户 2026-10-26/27 已拍板）：
//   · IX-Ray 是**矢量**字体（`fonts/<lang>/*.ttf`），内置位图出包器做不了它；
//   · 合并内核在 **fonttools**（纯 Python）里——用户原话「好用的现成东西，拿过来用就好了」；
//   · 所以产品侧把它当**外部助手**（与 ffmpeg 同一模式）：本模块只负责
//       「找 python + 找启动器 + 起进程 + 收退码与尾巴」，
//     不重写合并算法（免得两份口径）。
//
// 路径口径（**零静默**：缺哪个就说哪个）：
//   python ：环境变量 `XRKIT_IXR_PYTHON` → 包内 `python/python.exe` → `python`（PATH）
//   启动器 ：环境变量 `XRKIT_IXR_RUN` → 包内 `tools/ixr_run.py`
//   字体库 ：由启动器在同目录 `tools/fontlib/fontTools` 里找（见 `ixr_run.py` 文件头）

#include <string>

namespace xrkit::engine::font {

struct IxrBuildOptions {
    std::string pack;      // 参照包（能当模组打的整包目录）
    std::string font;      // 中文字体（.ttf/.otf）
    std::string out;       // 输出目录
    std::string langs;     // `rus,eng`；空 = 用包里的 primary lang
    bool all_langs = false;
    bool split_output = false;
    std::string plan;      // 逐槽位字体计划 JSON（可空）
    std::string sizes;     // 分区字号档（逗号分隔，可空）
};

struct IxrBuildResult {
    bool ok = false;
    std::string error;
    int exit_code = -1;
    std::string python;      // 实际用的解释器（排障用）
    std::string launcher;    // 实际用的启动器
    std::string stdout_tail; // 末尾若干行（含 `IXRGEN {...}` 报告）
    std::string stderr_tail;
};

IxrBuildResult ixr_build(const IxrBuildOptions& options);

// 解析出来的工具链（自检/判据用；不真跑）
struct IxrTools {
    std::string python;
    std::string launcher;
    std::string error;   // 非空 ⇒ 缺东西，内容说明缺什么
};
IxrTools ixr_resolve_tools();

}  // namespace xrkit::engine::font
