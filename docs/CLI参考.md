# CLI 参考（`xrkit_cli.exe`）

本页是常用动作的说明；**参数表以程序自身为准**：

```bat
xrkit_cli --help
xrkit_cli --help <动作>
xrkit_cli --actions
```

`--actions` 输出结构化动作表（JSON）；`--help` 是完整文本表。

## 0. 通用约定

- 退出码：`0` 成功；`1` 出错（有理由）；`3` 没做（用法错误、写盘守卫或待确认，读理由行）。
- 路径含中文/空格时加引号；单文件/目录尽量用绝对路径。
- 编码名用标准名（如 `windows-1251`），不确定用 `auto`。
- `--only` 清单每行一个包内路径，`/` 分隔、大小写敏感；空清单拒绝执行。
- GUI 的模式名「行数/ID 统计」「ID 文本对比」对应 CLI 的 `--mode stats` / `--mode text`。

## 1. 追更与快照

```bat
xrkit_cli --snapshot <根目录> <out.json>
xrkit_cli --diff-snapshot <旧.json> <新.json>
xrkit_cli --track <baseline_dir> <current_dir> <out_dir> [--baseline old.json] [--rehash]
```

- `--snapshot`：文件级 manifest（`rel/kind/size/mtime/sha256` + JSGME 行序），无时间戳，两次运行逐字节一致。
- `--diff-snapshot`：只比 manifest，打印 added/removed/modified/jsgme_changed；有变化也返回 0。
- `--track`：文件级预筛 + XML 条目级文本对比；写 `report.json` 与 `changed/*.tsv`；`.ltx/.script/.txt` 只到文件级。

## 2. XML 对比

```bat
xrkit_cli --xml-compare-dirs <目录A> <目录B> [--mode stats|text] [--enc-a X] [--enc-b Y]
```

默认 `stats` 只比 ID 集合和文件数；找文本改动必须 `--mode text`。只处理 `.xml`。

## 3. 文本提取

```bat
xrkit_cli --extract-text <源目录> <输出目录> [--prefix p] [--id-prefix p] [--lang rus] [--clean]
```

`<源目录>` 可以直接是 `scripts`/`gameplay`，也可以是**游戏根、模组根、addons 根**：

- 根模式会递归找出所有 `scripts`/`gameplay` 子树；
- 改写后的文件按**相对源根的路径**落到输出目录（例如 `addon/gamedata/scripts/...`）；
- 文本表落在引擎可读的 `.../gamedata/configs/text/<lang>/`；`--lang` 默认 `rus`；
- 每棵子树的 ID 前缀带相对路径哈希，跨表不重名；
- 根节点不是 `string_table` 的配置 XML按无文本跳过，不当表、不复制；
- 没有可提取文本的子树只计 `notext`，不生成空文件；
- 输出目录不能等于、包含或位于源目录内。

`--clean` 先清输出落点。

汇总行字段：

```text
TEXT ok kind=scripts files=4 strings=18 chars=202 notext=1 skipped=1 replskip=0 writefail=0 ...
```

- `notext`：没有可译文本的文件数；
- `skipped`：跳过数；
- `replskip`：提取到但没替换上的数量（应为 0）；
- `writefail`：写回失败数（应为 0）。

含 `--` 的脚本文本可能被注释剥离逻辑跳过，见 `已知问题与边界.md`。

## 4. 汉化包

矢量字体（IX-Ray）的“字号”不是改 TTF，而是决定运行时图集大小：
- 生成器按包内字符数 + `fonts.ltx` 最大字号计算 `FontAtlasSize`；
- 最小 4096，按 2 的幂升到 8192，只升不降；
- 字号补丁也会把 `engine_external.ltx` 一并写入补丁。

`--merge-glyphs` 只把源字体的缺字形和 `hmtx` 抄进目标；**不改目标字体的 hhea / OS/2 垂直度量**（ascent/descender 决定引擎的文本格高度）。字符集统计也会排除 C1 控制区（U+0080–U+009F），UTF-8 续字节不会被当成真实文字。

### `--font-pack`

```bat
xrkit_cli --font-pack ^
  --ref <参照包|builtin> ^
  --text-root <译文根|包根> ^
  --out <目录> ^
  [--font <TTF>] [--font-weight 700] [--game SoC|ClS|CoP] [--lang rus] [--langs rus] ^
  [--on-missing fail|warn] [--require-full-cyrillic] [--merge-glyphs <源TTF>] ^
  [--patch-offsets 3,-1] [--jsgme --jsgme-name <名>]
```

- `--ref builtin`：内置位图模板（SoC/ClS/CoP 可用）；
- `--ref <目录>`：扫描 `fonts.ltx`，按证据选 writer（矢量字体 ⇒ 矢量，位图字体纹理 ⇒ 位图）；冲突/缺失时要求 `--writer bitmap|vector`；
- `--text-root` 可给包根、`gamedata` 根、`gamedata/configs/text/<lang>` 或文本目录；
- `--font` 省略时自动寻找可用的默认中文字体；可变字体的 `--font-weight` 直接设 `wght` 轴（100–900，默认 700）；
- `--patch-offsets` 生成独立字号补丁，可与 `--jsgme` 同用；
- JSGME 摆盘固定 `MODS\<名>\` 平铺；`--jsgme-category` 仅为兼容旧命令行，会告警并忽略；
- 位图单图集最多 8192；50px 大字集硬扩到 8192×4096，不做多页/自动降字号；
- 所有字体段参与生成；基础 HUD/STAT 字体跳过；特殊字体段由人工复核；
- IX-Ray 矢量包会写 `gamedata/configs/engine_external.ltx` 的 `FontAtlasSize=8192`。

### `--font-size-patch`

```bat
xrkit_cli --font-size-patch <参照包> <offset> <输出目录>
```

只改 IX-Ray `fonts.ltx` 的显式 `size = N`（继承段跟随父段；TTF 不重烘）。和位图 `--patch-offsets` 是两套机制。

### `--extract-pack`

```bat
xrkit_cli --extract-pack <源根> <输出根> [--no-language-pack] [--no-rewritten]
```

提取编排：根 `scripts` + `configs/scripts` + `gameplay`；空成果返回 1。源根需含 `configs`/`config`。

### `--langpack-scan`

```bat
xrkit_cli --langpack-scan <汉化包或目录>
```

识别 `gamedata/configs/text/<lang>` 槽位与字体/贴图资源；不解包、不改文件。

## 5. 资源查看

```bat
xrkit_cli --dump <包文件> [DB格式]
```

列出 DB 六格式/SquashFS 条目；只读。格式省略时自动嗅探。

## 6. 版本与打包

| 动作 | 用途 |
|---|---|
| `--version` | 打印版本行（含 `build=`） |
| `--write-version <版本> <标记> <序号>` | 递增 `build_counter`，写 `version.h/.rc/version.json.current` |
| `--build-package <version.json> <版本> <标记> <序号> [--prebuilt]` | 算 sha、打包、写构建史；`--prebuilt` 只打包不重编 |
| `--exe-version <exe>` | 读 VERSIONINFO |
| `--sha256 <文件>` | 打印 SHA256 |

模型 C：测试版版本串是 `x.y.z.<build>`；`标记/序号` 只决定通道目录，例如：

```text
BETA.270 + build 530 ⇒ 版本串 2.0.0.530，dist\2.0.0\BETA\270
```

## 7. 自检与宿主诊断

```bat
xrkit_cli --version
xrkit_cli --python-host
xrkit_cli --py-plugins <目录>
xrkit_cli --ui-facade
xrkit_cli --py-plugin-call <目录> [JSON]
```

宿主更新诊断（通常由主程序/GUI 使用）：

```bat
xrkit_app --host-update-check
xrkit_app --host-update-apply
```

`--host-update-apply` 会下载并启动 `xrkit_updater.exe`；成功时当前进程退出。

`--smoke` 不是 CLI/本体动作；离屏自检由独立 `xrkit_smoke.exe` 执行。发布包不随带 smoke。
