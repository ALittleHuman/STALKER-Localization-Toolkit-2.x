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
- 真机样本沿用 1.x 那一份（**2026-09-24 实测：已从 `E:\Human\` 迁到**
  `E:\Software\Games\STALKER\Games\`）：`NLC Improved [OBT]\gamedata.sq_*`、`bin_x64\xrEngine.exe`；
  正确的补丁版在 `E:\Software\Tools\Ghidra\Projects\NLC Improved\Exported\xrEngine_patched.exe`
  （`xrEngine.exe` 与它逐字节对拍是最硬的判据）。文本类语料见
  `docs/2.0.0-文本模块规格.md`（gameplay/scripts 真树）。

## 文档

- `docs/2.0.0-设计口径.md` —— 分层架构、插件 C ABI、里程碑、七条验收判据
- `docs/2.0.0-行为口径对照表.md` —— 1.x 逐条行为判据（重写必须逐条重验）
- `docs/2.0.0-待拍板清单.md` —— 14 条开放项与决策记录
- `docs/2.0.0-SquashFS-writer规格.md` —— 自写 writer 的**逐字节复刻规则**与取证（M1-5）
- `docs/2.0.0-文本模块规格.md` —— 文本模块的两条编码链、提取规则、门禁与缺口记账（M1-6）

## 构建与门禁

```powershell
# 构建（core + engine + cli；Qt 外壳另用 -Preset msvc-shell）
powershell -File scripts\build.ps1

# 门禁（三档：--fast / 默认 local / --release）
python tests\run_ci.py
python tests\run_ci.py --fast
python tests\run_ci.py --release      # 任何 SKIP 与「未覆盖清单」非空即 FAIL
```

产物：`build/msvc-release/tools/cli/xrkit_cli.exe`
（`--selftest` / `--bench` / `--themecheck` / `--dump` / `--extract` / `--pack-db` /
`--pack-sqfs` / `--decode-text` / `--sniff` / `--extract-text`；
退出码 0=通过 1=失败 **2=显式跳过** 3=用法错误）。

## 开发约定（都是本机实测踩出来的，别改回去）

1. **`.ps1` 必须存成 UTF-8 with BOM** —— PowerShell 5.1 会把无 BOM 的脚本按 GBK 读，
   中文注释直接被读坏、脚本语法报错。
2. **PowerShell 里给 cmake 传 `-DNAME=$var` 必须加引号**：裸写 `-DXRKIT_ROOT=$root`
   变量**不会展开**，cmake 收到的是字面量 `$root`（实测）。
3. **从 Python 调 `cmd /c` 必须用 `shell=True`**：走 list 形式时 subprocess 的
   `list2cmdline` 会给整串再套一层引号，cmd 解析失败且被 `>nul` 吞掉，现象酷似"文件不存在"。
4. **必须在全权文件模式下构建**：`workspace-write` 沙箱会让 `cmake→ninja→cl→link`
   这条深层进程链死锁（CMake 永久卡在 "Detecting CXX compiler ABI info"）。
5. **收尾行一律 ASCII**（`SELFTEST pass=N fail=M` / `BENCH skip ...`）：中文行在别的进程里是乱码。

## 第三方依赖（vendored，离线可复现）

源码放 `third_party/`，每个 tarball 记录 sha256；拉取脚本 `tools/fetch_third_party.py`。
可达源在本机环境（GitHub 被 hosts 屏蔽、schannel TLS 不可用）下实测如下：

| 依赖 | 版本 | 源 | 备注 |
|---|---|---|---|
| FreeType | 2.14.3 | gh-proxy → GitHub tag `VER-2-14-3` | **对齐 Pillow 12.3.0 捆绑的 2.14.3**（字库产物"保持原样"的基准） |
| zlib | 1.3.1 | gh-proxy → `madler/zlib` tag `v1.3.1` | |
| xz (liblzma) | 5.6.4 | tukaani.org 官方 | |
| zstd | 1.5.6 | gh-proxy → `facebook/zstd` tag `v1.5.6` | |
| lz4 | 1.10.0 | gh-proxy → `lz4/lz4` tag `v1.10.0` | |
| lzo | 2.10 | oberhumer.com 官方 | |
| PCRE2 | 10.48 | USTC Debian pool（`pcre2_10.48.orig.tar.gz`） | |

> ⚠ gh-proxy 是第三方代理。取回的 tarball 一律**记录 sha256 并在构建前校验**，
> 保证后续构建可复现；provenance 记录在 `third_party/SOURCES.md`。
