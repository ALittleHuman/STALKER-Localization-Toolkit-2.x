# STALKER 汉化工具集 2.0（xrkit）

面向 X-Ray 引擎（SoC / Clear Sky / CoP / IX-Ray）的资源查看、解包、文本提取与汉化包生成工具。

- 当前源码版本：`2.0.0.524（BETA.264）`
- 运行环境：Windows 10/11 x64；发行包自带 Qt6 运行库与 Python 3.14，**不需要系统 Python**
- 只写你自己的输出目录、`logs\` 与工具自己的 `xrkit_user.ltx`；不会安装或修改游戏文件

---

## 一、能力

| 能力 | 入口 | 说明 |
|---|---|---|
| 读/写 DB 六格式与 SquashFS | 文件系统页 / `xrkit_cli --dump` | 解包树、散装层、长路径与链接安全检查 |
| 编码检测与批量转换 | 编码转换页 | cp1251 / cp1252 / GB18030 / Big5 / Shift-JIS / EUC-KR / UTF-16/32；带扫描台账、备份/恢复 |
| 文本提取 | 文本工具页（模式「提取文本」）/ `xrkit_cli --extract-text` | 从 `configs/gameplay`、`scripts`、`.ltx` 提取；也可直接给游戏根/addons 根，保留相对树 |
| XML 校对 | 文本工具页 | 目录 A/B 比对（统计 / 文本对比）、CSV/XLSX 导出、复制差异 ID |
| 视频转 OGM | 视频转换页 | 以参考 OGM 的参数为准；ffmpeg 只认两处 |
| 汉化包生成 | 汉化包生成页 / `--font-pack` | 标准字库、字号补丁、JSGME 平铺、IX-Ray 矢量字库 |
| 插件扩展 | `plugins/` | Python 插件可注册页面、菜单、命令、比较模式、解密器、包格式 |

## 二、安装与启动

1. 把发行包解压到任意目录（建议短路径；长路径见 §七）。
2. 双击 `xrkit_app.exe`；命令行用 `xrkit_cli.exe`。
3. 设置保存在程序目录的 `xrkit_user.ltx`（首次修改设置时创建；删除即恢复默认）。
4. 运行日志在 `logs\log_*.txt`；反馈问题时附该文件。
5. 退出码：0 = 成功；1 = 失败；3 = 没做（用法错误，或写盘守卫/待确认——读理由行）。

```text
xrkit_app.exe             图形界面（本体，不含 smoke）
xrkit_cli.exe             命令行
xrkit_plugin_manager.exe  插件管理器
xrkit_updater.exe         本体更新器（独立进程，不联网）
xrkit_smoke.exe           开发/门禁用离屏自检；不随发布包发放
plugins\                  插件目录；顶层空表示只有内置页
```

## 三、常用流程

### 1）解包资源包

1. 「文件系统」→「解包」；
2. 「加包」选择 `.db0`/`.db1`/`.xdb`/`.sqfs`，或把包/文件夹拖进窗口；
3. 点「加载选中」后右侧显示包内条目（懒加载）；
4. 可搜索、单击勾选、整棵子树勾选；
5. 确认「解包到」——默认落点是来源所在目录；
6. 「解包选中」或「批量解包」；状态行会报覆盖数量。

### 2）批量转换编码

1. 选源路径、目标编码、扩展名；
2. 勾选递归/备份/排除；填了输出目录时备份自动失效；
3. 开始后看扫描台账（递归层数、文件数、跳过原因）；
4. 自动判定不确定会弹确认框（判定、理由、置信度、文件名）；
5. 出错可「恢复备份」；备份在 `源目录\backup\*.bak`。

### 3）生成汉化包

推荐 CLI：

```bat
xrkit_cli.exe --font-pack ^
  --ref builtin ^
  --text-root "<译文目录>" ^
  --out "<输出目录>" ^
  --langs rus ^
  --jsgme --jsgme-name "<MOD 名>" ^
  --patch-offsets 7
```

- `--text-root` 可给包根、`gamedata` 根、文本目录；
- `--font` 省略时自动寻找可用的默认中文字体；
- `--ref <目录>` 扫描参照包：有矢量字体走矢量，有位图字体纹理走位图；证据冲突或缺失时要求显式 writer（--writer）；
- 位图单张图集最多 **8192**；50px 大字集硬扩到 8192×4096，不做多页/降字号；
- 所有字体段都参与生成（含自定义段）；基础 HUD/STAT 字体跳过；
- IX-Ray 矢量包会写 `gamedata/configs/engine_external.ltx` 的 `FontAtlasSize=8192`；
- 特殊字体段是否保留原贴图/英文由生成后人工复核。
- 可变字体会读取 `wght` 轴：页面有「字重」下拉与「字重预览」，默认 700；可预览后选择 100–900。

### 4）更新

- 只认 GitHub Release；BETA/RC 必须发布为 **Prerelease**，正式版才用 Release；
- 更新清单优先 GitHub API，失败走镜像；更新包优先 unpkg，失败回退 ghfast/gh-proxy/GitHub；
- 下载在后台，进度显示在「就绪」右侧；下载中有「取消下载」按钮，取消后保留分片；
- 下载完成询问是否重启安装；选「否」则下次启动安装；
- 「工具 → 启用检测更新」和「工具 → 联网功能」可关闭更新；启动参数 --no-auto-update 也可关闭自动检查；
- 文件替换由 `xrkit_updater.exe` 完成，失败回滚；更新器本身不联网。

当前 RC2 预发布为 `2.0.0.514`；BETA.264 只在本机打包，没有发布。

## 四、视频转换的 ffmpeg 约定

- 只认两处：`xrkit_user.ltx` 的 `[ffmpeg] path/probe`，或程序目录/`bin` 下的 `ffmpeg.exe`/`ffprobe.exe`；不搜 PATH；
- 只接受 **7.x**；8.x 的 theora 对 STALKER OGM 有 bug；
- 发行包自带 minimal 7.1.5 ffmpeg，不带 ffprobe；缺 ffprobe 时自动用 `ffmpeg -i` 降级。

## 五、构建

```bat
scripts\build.ps1                          :: msvc-release（CLI/引擎）
scripts\build.ps1 -Preset msvc-shell       :: GUI + xrkit_smoke
scripts\release.ps1 -Index <序号> -Version 2.0.0 -Mark BETA
```

- 需要 VS 2022 Build Tools、CMake、Ninja、Qt 6.8.3；
- 模型 C 版本：全局 `build_counter`，测试版版本串 `x.y.z.<build>`，通道只作标签；
- `xrkit_smoke.exe` 只用于构建/门禁和包内临时自检，发布包不带；
- 门禁与打包细节见 `docs/构建与发版.md`。

## 六、已知边界

1. **路径**：CLI 和 GUI 都已声明 `activeCodePage=UTF-8` + `longPathAware`；超过 260 字符仍受 Windows `EnableLongPaths` 开关限制。
2. **插件与宿主同进程**：崩溃、`import` 副作用会影响宿主；主线程 30 秒无心跳会触发 UI 看门狗，写 `logs/ui_watchdog.log` 并退出进程（fail-fast，不是热恢复）。长活请放 `task_run` worker。
3. **文本提取**：含 `--` 的脚本文本可能被注释剥离逻辑跳过；见 `docs/已知问题与边界.md`。
4. **汉化包**：失败会回滚；输出目录不要直接指游戏根。
5. **更新**：当前 npm 只修了发布/寻址链路，BETA.264 未上传 npm；客户端会回退 GitHub。

更多见 `docs/已知问题与边界.md`。

## 七、文档

| 文档 | 内容 |
|---|---|
| `docs/用户手册.md` | 界面逐页说明 |
| `docs/CLI参考.md` | 命令行动作与参数 |
| `docs/插件开发.md` | Python 插件 API/线程/加载/扩展点 |
| `docs/构建与发版.md` | 构建、smoke 拆分、打包、版本模型、npm |
| `docs/内部契约.md` | 维护者硬口径 |
| `docs/已知问题与边界.md` | 用户可见限制与规避 |
| `docs/路线图.md` | 非 2.0.0 规划：模块化字库、OGSR 二进制补丁 |
