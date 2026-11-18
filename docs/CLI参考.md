# CLI 参考（`xrkit_cli.exe`）

> 权威来源是程序自带的 `xrkit_cli.exe --actions`（机器可读 JSON，只含有结构化元数据的动作）与 `--help`（完整动作表）。
> 本页列已结构化的动作；其它动作用 `--help <动作>` 查参数。发现不一致以程序为准并提 issue。

## 0. 通用约定

- **退出码**：`0` 成功；`1` 出错（有理由）；`3` 没做（用法错误，或写盘守卫/待确认——都读理由行）。
- **路径**：单文件/目录都用绝对路径最稳；含中文/空格的路径**加引号**（且见"已知边界"里的路径限制）。
- **输出**：解析型动作（`--snapshot`/`--track` 等）会写文件；其余多数只打印，不改输入。
- **自检**：`xrkit_cli --version` 打印版本；`--help` 列出全部动作；`--help <动作>` 打印参数签名。
- **编码名**：用标准名（如 `windows-1251`，不是 `cp1251`）；不确定就先 `auto`。
- **`--only` 清单**：每行一个包内路径（`/` 分隔、大小写敏感）；**空清单＝拒绝**（不是全选）；
  UTF-8 BOM 会被剥掉、`\` 会转成 `/`、前导 `./` 会被去掉。
- **模式显示名**：GUI 里是「行数/ID 统计」「ID 文本对比」，CLI 对应 `--mode stats` / `--mode text`。

## 1. 追更链路

### `--snapshot <根目录> <out.json>`
文件级 manifest：`rel/kind/size/mtime/sha256` + JSGME 行序。**无时间戳 ⇒ 两次跑逐字节一致**。

```bat
xrkit_cli --snapshot old\ old.json
```

### `--diff-snapshot <旧.json> <新.json>`
只比 manifest（不读文件内容），打印 added/removed/modified/jsgme_changed。有变化也返回 0。

```bat
xrkit_cli --diff-snapshot old.json new.json
```

### `--track <baseline_dir> <current_dir> <out_dir> [--baseline old.json] [--rehash]`
文件级预筛 + 条目级文本对比（XML 现算）。写 `out_dir/report.json` 与 `out_dir/changed/*.tsv`。
`.ltx/.script/.txt` 只到文件级（运行时打印 caps 行）；`--rehash` 忽略 size+mtime 缓存。

```bat
xrkit_cli --track old\ new\ out\ --baseline old.json
```

## 2. XML 对比

### `--xml-compare-dirs <目录A> <目录B> [--mode stats|text] [--enc-a X] [--enc-b Y]`
默认 `stats` 只比 ID 集合与文件数；**找文本改动必须 `--mode text`**。只比 `.xml`。

```bat
xrkit_cli --xml-compare-dirs old\ new\ --mode text
```

## 3. 文本提取

### `--extract-text <源目录> <输出目录> [--prefix p] [--id-prefix p] [--clean]`
从 `scripts` / `gameplay` / `.ltx` 目录提取可翻译文本，产出 `<prefix>_scripts_texts.xml`（或 gameplay 版）
与改写后的源码树（`game.translate_string("<id>")`）。`--clean` 先清输出落点。

```bat
xrkit_cli --extract-text gamedata\scripts out\ --prefix gs
```

输出一行结论，字段即契约：

```text
TEXT ok kind=scripts files=4 strings=18 chars=202 notext=1 skipped=1 replskip=0 writefail=0 long=0 xml=gs_scripts_texts.xml ms=11
```

`notext`=没有可译文本的文件数，`skipped`=跳过数，`replskip`=提取到了但没替换上的数量（应为 0），
`writefail`=写回失败数（应为 0）。

## 4. 汉化包

### `--font-pack`（主入口）
`--ref <参照包|builtin> --text-root <译文根|包根> [--font <TTF>] --out <目录> [--game SoC|ClS|CoP] [--lang rus]
[--langs rus] [--on-missing fail|warn] [--require-full-cyrillic] [--merge-glyphs <源TTF>]
[--patch-offsets 3,-1] [--jsgme --jsgme-category <分类> --jsgme-name <名>]`

- `--ref builtin`：内置模板（位图 writer，SoC/ClS/CoP 可用）；`--ref <目录>`：扫描参照 `fonts.ltx`（矢量 writer，IX-Ray 必须）；
- `--text-root` 可用包根 / `gamedata` 根 / `gamedata/configs/text/<lang>` / 文本目录；
- `--font` 省略时自动找 NotoSansSC → 微软雅黑；`--merge-glyphs` 仅矢量有效；
- `--patch-offsets 7` 生成独立「字号+7」MOD，可与 `--jsgme` 同用。

```bat
xrkit_cli --font-pack --ref builtin --text-root "E:\...\2.汉化" --out "E:\...\更新\2.汉化" ^
          --langs rus --jsgme --jsgme-category 汉化 --jsgme-name "2.汉化" --patch-offsets 7
```

### `--font-size-patch <参照包> <offset> <输出目录>`
只改 IX-Ray `fonts.ltx` 的显式 `size = N`（继承段跟随父段；TTF 为矢量，不重烘）。

```bat
xrkit_cli --font-size-patch 参照包\ -3 out\
```

### `--extract-pack <源根> <输出根> [--no-language-pack] [--no-rewritten]`
提取编排：根 `scripts` + `configs/scripts` + `gameplay`，语言包收集/文本表合并由引擎统一实现。
源根需含 `configs`/`config`；空成果返回 1。

### `--langpack-scan <汉化包或目录>`
识别 `gamedata/configs/text/<lang>` 槽位与字体/贴图资源；不解包、不改文件。

## 5. 资源查看

### `--dump <包文件> [DB格式]`
列出包内条目（DB 六格式 / SquashFS 自动嗅探）。只读。

```bat
xrkit_cli --dump gamedata.db0
```

## 6. 版本与打包（发版脚本用）

| 动作 | 用途 |
|---|---|
| `--version` | 打印版本行（含 `build=`） |
| `--write-version <版本> <标记> <序号>` | 改 `version.h` 与 `.rc`（不动 `version.json` 的 current） |
| `--build-package <version.json> <版本> <标记> <序号> [--prebuilt]` | 打包 + 自检 + 写 `version.txt` |
| `--exe-version <exe>` | 读任意 exe 的 VERSIONINFO（发版校验用） |

## 7. 插件

| 动作 | 用途 |
|---|---|
| `--py-plugins <目录>` | 加载并列出插件：`PYPLUGINS files=… loaded=… registrations=… errors=…`，逐条 warn/disabled |
| `--py-plugin-call <目录> <json>` | 无界面调用插件的 `headless(api, args)`（插件自己决定语义） |
| `--ui-facade` | 打印 `xrkit` 模块**实际暴露**的 `Api/Ui/Engine/Host` 名字与控件门面可用性（CLI 里 `available=0` 正常）；**插件方法清单以此为准** |

```bat
xrkit_cli --py-plugin-call plugins {"action":"scan","path":"E:\\x"}
```

## 8. 图形程序的自检入口（`xrkit_app.exe`）

| 动作 | 用途 |
|---|---|
| `--smoke` | 离屏全量自检，结论行 `SHELL-SMOKE ok checks=N failed=0`；`failed=` 必须为 0，退出码 0 |
| `--version` | 打印图形程序版本 |

```bat
set QT_QPA_PLATFORM=offscreen
xrkit_app.exe --smoke
```

## 9. 组合示例：两条常用流水线

**A. 追更 → 出文本对照表**

```bat
xrkit_cli --snapshot 旧包\ 旧.json
xrkit_cli --track 旧包\ 新包\ 追更\ --baseline 旧.json
xrkit_cli --xml-compare-dirs 旧包\configs\text 新包\configs\text --mode text
```

**B. 出汉化包 + 独立字号补丁**

```bat
xrkit_cli --font-pack --ref builtin --text-root 译文本\ --out 更新\base --langs rus --jsgme
xrkit_cli --font-pack --ref builtin --text-root 译文本\ --out 更新\plus7 --langs rus --patch-offsets 7
```
