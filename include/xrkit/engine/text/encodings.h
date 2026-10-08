#pragma once
// xrkit::engine::text —— **源/目标两张编码表**（唯一一份；2026-10-08 P9a 从 `convert_page.cpp` 搬来）。
//
// 为什么搬到引擎（而不是留在页面 / 插件里各写一份）：
//   · 这两张表的**成员资格**是产品纪律定下来的（"只列与 Python 逐字节对拍过的编码"），
//     不是界面品味 —— 页面、插件页、以后的批量工具都该看到**同一张表**；
//   · 表项引用的是引擎自己的编码常量（`etxt::kUtf8` 等）⇒ 放引擎才没有回头依赖。

#include <string>
#include <vector>

namespace xrkit::engine::text {

// 源 / 目标**共用**的那张表（1.x `ENCODING_CHOICES` 与 `convert_app.py:192` 的交集，
// 双字节 + 编码侧已全量对拍；utf-16/32 六族也在 2026-09-26 对拍过 ⇒ 可以列）。
const std::vector<std::string>& manual_encodings();

// 只在**目标侧**多出来的一项：`ascii`（源侧不放它 —— 源是"猜"的，塞进去会让人以为工具会猜 ascii）。
const std::vector<std::string>& target_only_encodings();

}  // namespace xrkit::engine::text
