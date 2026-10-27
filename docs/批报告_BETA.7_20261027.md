# 批报告：BETA.7（2026-10-27）—— 提取链收口 + IXR 多语言/并排对比

> 结论：`--fast` PASS、`--release` PASS（129 步）；包 `dist/2.0.0/BETA/7`（77 文件）；
> 包内自检 `SHELL-SMOKE ok checks=917 failed=0 not_covered=0`；`commit=45ed987`；
> `exe_sha256=bb05a2fd066c2587e5c446f826f73bdd207c0ab14975574bf9d8039e8dd283d6`。
> BETA.1/2/258/3/4/5/6 未动。

## 一、自 BETA.6 之后的两刀

| commit | 内容 |
|---|---|
| `63009f1` | **XML 校对第三模式的功能②接上真实提取**：`build_language_pack_from_game`（提取 scripts/gameplay → 摆盘）+ CLI `--langpack-build … --extract` + 页面走完整链 |
| `45ed987` | **IXR 多语言各出一份**（`--langs`/`--all-langs` + 强制 `--split-output`）+ 插件**并排对比**（原字体 vs 候选，两张 `ui.image`） |
| `7e36cf5`… | BETA.6 的版本三件套与批报告（上一条滚动记录） |

## 二、本批关键事实（写进判据/口径）

1. **1.x 提取只提西里尔正文**：`<text>Привет мир</text>` → `sgtat_gs_gp_0_0`；纯中文不会被抽。
   ⇒ 「提取文本」模式的正确输入是**俄文原版那一侧**；判据夹具因此用俄文物料（不是缺口）。
2. **多语言必须 `--split-output`**：不给就退 1 —— 多份包写同一目录会互相覆盖（保护而非限制）。
3. **提取产物按分支名落地**：`scripts/…`、`gameplay/…`（第一版落在包根，被标准布局判据抓出）。
4. **对比预览用 `ui.image` + `ui.destroy`**：换新图先删旧控件（P2/P3 原语在同一处真用）。

## 三、判据增量（仍 129 步，步内加条）

| 步 | 新条 |
|---|---|
| `langpack-build` | `--extract`：gameplay 真被改写成 ID 引用 / 文本 XML 落标准布局且含原文 |
| `xml-extract-mode` | 锚点 19 条（+`build_language_pack_from_game`） |
| `ixr-plugin` | 多语言出两份 / 多语言不给 `--split-output` 退 1 |

## 四、目标完成度（对着 2026-10-26 objective）

| 目标 | 状态 |
|---|---|
| ① 门面 P3（`ui.image` 等） | ✅ `image` / `font_sample` 已上；`dialog`/`set_style` 按"按需"未加（无诉求） |
| ② IXR 插件 | ✅ 扫描 + 四档识别 + 「指定」模式 + 逐槽位（分区）字体 + fonttools 合并 + 多语言 + 并排预览 |
| ③ XML 合并 | ✅ 三模式触发 + 两复选框 + 标准语言包 + 真实提取链 |
| ④ 钉进门禁 | ✅ 5 个新步；判据以 MODS 三个真实字体包为夹具（langpack-scan 步） |
| 双档门禁 + 打包 + 批报告 | ✅ `--fast` / `--release` / BETA.6 / BETA.7 / 本报告 |

## 五、仍挂着（已知、不影响发包）

1. `plugins_ixr` 不进发行包（插件路线：仓库内开发，稳定后再合内置页）；
2. XML 第三模式的"输出目录"默认是 `<非空一侧>/_langpack`（可改），未与 `MODS\汉化\<lang>` 联动；
3. PKG-PAGES 间歇红（15~25%）未定性；
4. `--release` 的 bench 偶发（I/O 慢一倍时一次判红，复跑绿）。
