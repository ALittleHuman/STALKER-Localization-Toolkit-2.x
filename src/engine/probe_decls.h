#pragma once
// xrkit::engine —— **模块在编**的探针声明（`src/engine` 内部用，不进公共头）。
//
// 为什么要有它（审计 2026-10-06 第 4 组：`engine::status_line()` 的**常量自报**）：
//   修前 `status_line()` 是
//       return std::string("engine id=") + engine_id() + " ready=1 modules=5/5";
//   —— `5/5` 是**字面量**，与"到底编进来几个模块"没有任何关系。实测它早就不对了：
//   `src/engine` 下有 6 个模块（fs/font/text/xml/video/plugin），报告里还写着 5/5。
//   一个恒等于常量的"自报"没法被判据反驳 ⇒ 谁把模块摘掉、谁少写一个，界面与 CLI 都照报。
//
// 口径（与 `third_party_probe.h` 的 `probe_third_party()` 同款）：
//   · 每个模块在自己的**翻译单元**里定义一个 `probe_<模块>()`，返回**本模块内定义的**
//     一个常量（不复用别的模块的符号）—— 函数能链上就说明该模块真在二进制里；
//   · `engine.cpp` 的模块表逐个调它们 ⇒ `module_count()` 是"真链上的那些"，
//     不是清单里写了几个。
//   · **名字与函数一一对应**：加模块 = 在这里加一行声明 + 实现里加一行定义 + 进模块表，
//     三处都少不掉（少一处就链接失败，不是静默少报）。
namespace xrkit::engine {

const char* probe_fs();
const char* probe_text();
const char* probe_xml();
const char* probe_video();
const char* probe_font();
const char* probe_plugin();

}  // namespace xrkit::engine
