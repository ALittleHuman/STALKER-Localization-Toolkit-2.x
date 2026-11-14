# STALKER 汉化工具集 2.0（xrkit）

面向 X-Ray 引擎（SoC / Clear Sky / CoP / IX-Ray）的资源查看、解包、文本提取与汉化包生成工具。

- 当前源码版本：`2.0.0 BETA.245`（正式交付以 `dist\2.0.0\<MARK>\<n>` 为准）
- 运行环境：Windows 10/11 x64；发行包自带 Qt6 运行库与 Python 3.14，**不需要系统 Python**
- 只写你自己的输出目录与工具自己的 `xrkit_user.ltx`；**不会安装或修改游戏文件**（解包/封包含"装进游戏"的只有你手动指定的目标目录）

---

## 一、它能做什么

| 能力 | 入口 | 说明 |
|---|---|---|
| 读/写 DB 六格式与 SquashFS | 文件系统页 / `xrkit_cli --dump` | 解包树、散装层、长路径与链接安全检查 |
| 编码检测与批量转换 | 编码转换页 | cp1251 / cp1252 / GB18030 / Big5 / Shift-JIS / EUC-KR / UTF-16/32；带扫描台账、备份/恢复 |
| 文本提取 | `xrkit_cli --extract-text` | 从 `configs/gameplay`、`scripts`、`.ltx` 提取可翻译文本（GUI 内置页不含此功能，见 §七） |
| XML 校对 | 文本工具页 | 目录 A/B 比对（stats / text 两种模式）、CSV/XLSX 导出、复制差异 ID |
| 视频转 OGM | 视频转换页 | 以参考 OGM 的参数为准；ffmpeg 只认两处（见 §五） |
| 汉化包生成 | 汉化包生成页 / `--font-pack` | 标准字库 + 可选 `字号+N` 补丁、JSGME 结构、IX-Ray 矢量字库 |
| 插件扩展 | `plugins/` | Python 插件可注册页面、菜单项、命令、比较模式（见 `docs/插件开发.md`） |

## 二、安装与启动

1. 把发行包解压到**任意目录**（建议路径短、全 ASCII，见 §七的路径限制）。
2. 双击 `xrkit_app.exe`（图形界面）；命令行用 `xrkit_cli.exe`。
3. 设置保存在程序目录的 `xrkit_user.ltx`（首次修改设置时创建；删除即恢复默认）。
4. 运行日志在 `logs\log_*.txt`，引擎行与界面上出现的每一行都在里面——反馈问题时附该文件即可。

```
xrkit_app.exe            图形界面
xrkit_cli.exe            命令行
xrkit_plugin_manager.exe 插件管理器（查看已加载/禁用/更新插件）
xrkit_updater.exe        本体更新器（独立进程，不联网）
plugins\                 顶层为空；放 <name>.py 即被加载（见 docs/插件开发.md）
```

## 三、五分钟上手

### 1）解包一个游戏资源包

1. 打开「文件系统」→「解包」标签；
2. 「加包」选 `.db0/.db1/…`、`.xdb`、`.sqfs` 等，或把包/文件夹**拖进窗口**；
3. 勾「加载选中」→ 右侧出现包内条目（懒加载，展开时才读）；
4. 可选：搜索框边打边筛、单击条目勾选/取消、勾文件夹整棵子树；
5. 设定「解包到」——**默认落点是来源所在目录**，请先确认；
6. 点「解包选中」或「批量解包」。完成后状态行会给出**覆盖了多少个文件**（覆盖是可见的，不是静默的）。

### 2）批量转换文本编码

1. 打开「编码转换」：选源路径（目录或单文件）、目标编码、扩展名（默认 `.xml`）；
2. 需要时改递归/备份/排除模式——**填了输出目录时"备份"会自动失效**（备份只对原地转换有意义）；
3. 点「开始转换」；扫描台账会写清"递归几层 / 扫到多少 / 各为什么被刷掉"；
4. 自动判定不确定时会弹**确认**（给出判定、理由、标记、置信度，含文件名）；取消即不转换；
5. 出错可「恢复备份」；`源目录\backup\*.bak` 是备份位置；「清空备份」会要求确认。

### 3）生成汉化包（推荐 CLI）

```bat
xrkit_cli.exe --font-pack ^
  --ref builtin ^
  --text-root "E:\...\2.汉化" ^
  --out "E:\...\更新\2.汉化 A.R.E.A" ^
  --langs rus ^
  --jsgme --jsgme-category 汉化 --jsgme-name "2.汉化 A.R.E.A" ^
  --patch-offsets 7
```

- `--text-root` 可给包根、`gamedata` 根、`gamedata/configs/text/<lang>` 或文本目录；
- `--font` 可省略（自动找 NotoSansSC，再回退微软雅黑）；
- `--patch-offsets 7` 生成独立的「字号+7」MOD；可与 `--jsgme` 同用；
- 完整参数见 `docs/CLI参考.md` 与 `xrkit_cli --help --font-pack`。

## 四、更新

- 只认 GitHub Release：**正式版只查正式 Release；测试版可升后续测试版，也可升更高正式版**；
- 检查走 GitHub API 直连 + `gh-proxy.com` 多源，下载用 `.part` + Range 断点续传；
- 文件替换由独立 `xrkit_updater.exe` 完成，失败回滚；更新器本身不联网；
- 「工具 → 联网功能」可整体关闭更新检查。

## 五、视频转换的 ffmpeg 约定

- 只认两处：`xrkit_user.ltx` 的 `[ffmpeg] path/probe`，或程序目录/`bin` 下的 `ffmpeg.exe`/`ffprobe.exe`；**不搜 PATH**；
- 只接受 **7.x**（8.x 的 theora 对 STALKER OGM 有 bug，页面会直接拒绝）；
- 发行包自带自建 minimal 7.1.5 **ffmpeg**，**不带 ffprobe**：没有 ffprobe 时自动降级用 `ffmpeg -i`（页面会显示"已降级"）。

## 六、构建（源码）

```bat
scripts\build.ps1                        :: 默认 msvc-release（CLI/引擎，无 Qt）
scripts\build.ps1 -Preset msvc-shell     :: GUI
scripts\package.ps1                      :: 打开发/发行包（含包内自检）
scripts\release.ps1 -Index <n> -Mark BETA
```

依赖：VS 2022 Build Tools、CMake、Ninja、Qt 6.8.3（可用 `XRKIT_VS`/`XRKIT_QT` 覆盖路径）。
门禁见 `docs/构建与发版.md`。

## 七、已知边界（务必先看）

1. **路径**：程序**没有**声明 UTF-8 代码页与长路径支持（无 `RT_MANIFEST`）⇒
   ① 安装路径/输入输出路径**尽量全 ASCII、层级浅**；命令行传中文路径可能乱码；
   ② 解包遇到 **>260 字符**的深层路径会被**整体拒绝**（报"路径过长/落点不可用"），不是静默跳过。
2. **插件与宿主同进程**：插件异常可能影响宿主；插件可以联网、可用标准库 `sqlite3`
   （发行包随带 Python SSL 与 sqlite3），「工具 → 联网功能」开关只约束宿主自身的检查更新与下载。
   `plugins\*_page.py` 是**非产品夹具**（API 示例），不承诺生产可用；插件 API 目前标记为实验性。
3. **"文本提取"只有 CLI**：`--extract-text`；内置页里没有这一页。
4. **LZHUF 上限**：极难压缩的数据在封包时可能触发编码失败，此时产物可能不可用；
   建议封包后用 `xrkit_cli --dump <包>` 验证一下，遇到读不出的包请附样本与日志反馈。
5. **8.x ffmpeg** 被有意拒绝；`--patch-offsets` 与 IXR 矢量字库的语义不同，别混用。

更多见 `docs/已知问题与边界.md`。

## 八、文档

| 文档 | 内容 |
|---|---|
| `docs/README.md` | 文档索引与阅读顺序 |
| `docs/用户手册.md` | 五个页面的逐项操作、常见流程、常见错误 |
| `docs/CLI参考.md` | 全部动作、参数、退出码、示例 |
| `docs/插件开发.md` | 插件结构、API、线程契约、`when`、拖放、调试 |
| `docs/构建与发版.md` | 环境、两档构建、打包、发版、门禁 |
| `docs/已知问题与边界.md` | 用户可见的限制与规避办法 |

## 九、许可

见 `LICENSE`（GPL-3.0）。本项目为非官方汉化工具，与 GSC Game World 无关。
