# STALKER 汉化工具集 2.0.0

1.x（`../STALKER_Toolkit`，Tkinter 版，已发布 1.1.0 / `main` = `5d61206`）的**完全重写**。
本目录是 2.0.0 的唯一源码出处。

## 技术口径（用户 2026-09-24 拍板）

| 项 | 口径 |
|---|---|
| 语言 / 框架 | **C++20 + Qt 6 Widgets**（完全重写，不复用 Python 实现） |
| 平台 | Windows x64 |
| 编译器 | MSVC 14.44（VS 2022 BuildTools 17.14） |
| 构建 | CMake + Ninja |
| 插件机制 | **C++ DLL 插件 ABI**（放弃 1.x 的 Python 插件契约，ABI 待设计口径定稿） |

**为什么重写**（用户原话"不破不立"）：X-Ray 引擎本身是 C++，解包/封包用 C++ 更顺、精度更高；
闭源插件源码已在我方手中；插件生态因题材冷门而不重要。
PySide6 试点已实测：同为拖动分隔条，Qt 12~15 ms vs Tk 85~118 ms（根因是 Tk"每控件一个 Win32 窗口(HWND)"的架构）。

## 本机工具链（2026-09-24 实测可用）

| 组件 | 路径 / 版本 |
|---|---|
| Qt | `E:\Software\Tools\Qt\6.8.3\msvc2022_64`（6.8.3 LTS，qtbase + qttools + qtimageformats） |
| MSVC | `E:\Software\Tools\Visual Studio\2022\BuildTools`（14.44.35207；另有 VS 2026 / 14.51 可用） |
| Windows SDK | 10.0.26100 |
| CMake / Ninja | VS 自带（`...\Common7\IDE\CommonExtensions\Microsoft\CMake\`） |

冒烟判据（工具链端到端自检）：`QT-VERIFY ok qt=6.8.3 platform=offscreen widget=640x200`。
MSVC 编 Qt 必须带 `/Zc:__cplusplus`（否则 Qt 触发 `#error C1189`）与 `/utf-8`。

> ⚠ 构建必须在**全权文件模式（danger-full-access）**下跑：`workspace-write` 沙箱会让
> `cmake → ninja → cl → link` 这条深层进程链死锁（2026-09-24 实测）。

## 与 1.x 的关系

- **1.x 是行为基准，不是代码基准。** 2.0.0 的验收判据逐条对照 1.x 的行为口径
  （滚动条四看、拖动 line 档、文件合并"后者为准 + 散装最后"、日志栏、任务壳、字库定标……）。
- 真机样本沿用 1.x 那一份：`E:\Human\NLC Improved [OBT]`（`gamedata.sq_*`、`xrEngine.exe` /
  `xrEngine_patched.exe` 逐字节对拍）。

## 文档

- `docs/` —— 设计口径、验收判据、里程碑（编写中；**口径未过目前不写业务代码**）
