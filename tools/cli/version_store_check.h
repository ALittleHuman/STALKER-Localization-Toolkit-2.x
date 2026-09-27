#pragma once
// 构建版本存储（version.json）的**判据表** —— 单独一个文件，不往 main.cpp 里塞大块代码
// （往巨型 main.cpp 盲插大块代码两次都踩了结构性的坑，见 lesson「盲插大块代码」）。
//
// 覆盖四组：树往返 / next=max+1 / **备份还原逐字节** / **BOM 与坏文件不覆盖**。

#include "xrkit/core/check.h"

namespace xrkit_cli {

void register_version_store_check(xrkit::core::CheckRegistry& reg);

}  // namespace xrkit_cli
