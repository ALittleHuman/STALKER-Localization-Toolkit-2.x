#pragma once
// xrkit_cli 的构建/打包动作（**唯一实现**：GUI 用 QProcess 调它，脚本只做薄封装）。
//
// 用户 2026-09-27 口径 v2：构建的具体实现**不要单独一份**，直接引用 CLI 的就行 ——
// "这样也不会出现两者结果有差别的情况"。
//
//   --build-package <version.json> <版本> <标记> <序号>
//
// 语义（照他 2026-09-24 口述的 build 工具 spec）：
//   ① 开始构建前**自动存**版本，并留上一份备份；
//   ② 跑 build.ps1（真编译）+ package.ps1（铺 Qt / 写 README+version / 包内 --smoke 自检）；
//   ③ **失败 ⇒ 版本逐字节回退**，日志留在 dist\build_pkg_<N>.log；
//   ④ 成功 ⇒ 把这一次追加进构建史（version.json 的树）。
//
// 为什么单独一个文件：往巨型 main.cpp 里插大块代码两次都踩了结构性的坑（见 lesson
// 「盲插大块代码」）；这里 main.cpp 只加**一行调用**。

#include <string>
#include <vector>

namespace xrkit_cli {

// 返回值直接用 CLI 的退出码约定：0=OK / 1=FAIL / 2=SKIP / 3=用法错误
int run_build_package(const std::vector<std::string>& operands);

}  // namespace xrkit_cli
