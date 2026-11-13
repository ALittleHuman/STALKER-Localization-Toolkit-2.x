# STALKER 汉化工具集 2.0

面向 X-Ray 引擎（SoC / Clear Sky / CoP / IX-Ray）的资源查看、解包、文本提取与汉化包生成工具。

当前源码版本：`2.0.0 BETA.244`。正式发布包以 `dist\2.0.0\BETA\<build>` 为准。

## 它能做什么

- 读取/写入 X-Ray 的 DB 六格式与 SquashFS；
- 解包树、散装层、长路径与链接安全处理；
- 检测/转换 cp1251、cp1252、GB18030、Big5、Shift-JIS、EUC-KR、UTF-16/32 等编码；
- 从 `configs/gameplay`、`scripts`、`.ltx` 提取可翻译文本；
- 生成字体包：标准字号 + 可选 `字号+N` 补丁；
- 生成 JSGME 结构，自带 JSGME.exe/.lng；
- IX-Ray 矢量字体包 / fonts.ltx 字号补丁；
- 视频转 OGM；
- 插件扩展：Python 菜单项、面板、工具、比较模式等。

## 运行环境

- Windows 10/11 x64；
- 发行包自带 Qt6、Python 3.14、字体与 JSGME，不要求系统 Python；
- 源码构建需要 Visual Studio 2022 Build Tools、CMake、Ninja、Qt 6.8.3。

## 使用发行包

```text
xrkit_app.exe          图形界面
xrkit_cli.exe          命令行
xrkit_updater.exe      本体更新器（独立进程，不联网）
```

图形界面内置页面：

```text
文件系统
编码转换
文本工具（XML）
视频转换
汉化包生成
```

## 汉化包生成（推荐）

```bat
xrkit_cli.exe --font-pack ^
  --ref builtin ^
  --text-root "E:\...\2.汉化" ^
  --out "E:\...\更新\2.汉化 A.R.E.A" ^
  --langs rus ^
  --jsgme --jsgme-category 汉化 --jsgme-name "2.汉化 A.R.E.A" ^
  --patch-offsets 7
```

规则：

- `--text-root` 可指包根、`gamedata` 根、`gamedata/configs/text/<lang>` 或文本目录；
- 包根模式会自动带 `gamedata/scripts` 与 `gamedata/configs/gameplay`；
- `--font` 可省略，默认找 NotoSansSC，再回退微软雅黑；
- `--patch-offsets 7`：base 标准字号，另出“字号+7”独立 MOD；
- `--jsgme` 与 `--patch-offsets` 可同用；
- 完整说明见 `docs\2.0.0-汉化包CLI.md`。

## 更新

本体更新只认 GitHub Release：

```text
正式版 → 只查正式 Release
测试版 → 可升后续测试版，也可升更高正式版
```

- 正式版不会被推送到测试版；
- 想在正式版上试测试版，必须自己从 GH/其他渠道安装测试包；
- 更新检查支持 GitHub API 直连与 `gh-proxy.com` 多源；
- 下载使用 `.part` + Range 断点续传；
- 文件替换由独立 `xrkit_updater.exe` 完成，失败回滚；
- 更新器不联网；
- 更新检查可被「工具 → 联网功能」总开关关闭。

## 插件

插件目录：

```text
plugins/<name>/plugin.py
plugins/<name>/__init__.py
```

加载器只扫描顶层 `*.py` 与上述目录入口。插件可注册命令、菜单项、面板、工具、选项等扩展点。

插件更新：

- 更新前自动禁用；
- 整目录备份/替换；
- 启动时确认，失败回滚；
- 原本禁用的插件更新后仍禁用。

插件与宿主同进程运行；插件异常可能影响宿主，这是已知边界。

## 源码构建

```bat
scripts\build.ps1
scripts\package.ps1
scripts\release.ps1 -Index <n> -Version 2.0.0 -Mark BETA
```

产物：

```text
build\msvc-shell\      GUI
build\msvc-release\    CLI / 引擎
dist\2.0.0\<MARK>\<n>
```

## 自检

```bat
set QT_QPA_PLATFORM=offscreen
xrkit_app.exe --smoke
xrkit_cli.exe --selftest
python tests\run_ci.py --profile fast
```

打包脚本会从包目录再跑一次 `--smoke`，要求 `failed=0`。

## 已知边界

- 字体选择器 smoke 交互段尚未恢复行为覆盖；
- 插件注册表仍有若干注册点未绑定 Python；
- 插件可绕过宿主联网总开关；
- 部分崩溃（0xC00000FD / 0xC0000409）尚未完全归因；
- 详细审计见工作区外 `stalker_toolkit_audit\99_代码问题审计报告.md`。

## 文档

`docs\` 下按主题拆分规格、口径与历史复盘。若文档与代码冲突，以代码、测试和实际包为准。
