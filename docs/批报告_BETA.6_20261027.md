# 批报告：BETA.6（2026-10-27）—— 门面 P2/P3 + IX-Ray 四刀 + XML 校对第三模式

> 结论先行：`--fast` PASS、`--release` PASS（129 步）；包 `dist/2.0.0/BETA/6`（77 文件），
> 包内自检 `SHELL-SMOKE ok checks=917 failed=0`；`commit=64b7886`；
> `exe_sha256=a534548938fc381c7c1f0ec4d7a68a7f94cb96ce06830e49b2763847b1eb5ab2`。
> BETA.1/2/258/3/4/5 未动。

## 一、这一批做了什么（按提交顺序）

| commit | 内容 |
|---|---|
| `d7764c2` | 门面 **P2**：`ui.set_visible` / `ui.destroy` / `ui.timer`（复杂插件页缺的三件原语；能力表补理由） |
| `08651f8` | 门面 **P3**：`ui.image`（路径 → QLabel/QPixmap，等比缩放；读不到文件返回空句柄） |
| `fa32a5c` | **IXR 第一刀**：`--langpack-scan`（四档识别：ixr/cop/cis/soc + unknown 退 2；只读；判据含自证与只读快照） |
| `57ac528` | **IXR 第二刀**：`api.engine.langpack_scan`（转调引擎同一份实现；Engine 门面 77/77/77） |
| `23b98b3` | **IXR 第三刀**：真插件 `plugins_ixr/`（扫描→槽位树→分区→生成）+ `_lib/ixrgen.py`（fonttools 助手）+ 判据 |
| `b56d3c1` | **IXR 第四刀**：逐槽位字体计划 `--plan` + 分区 `--sizes` 真生效（修循环里字源未复位） |
| `1a11d8b` | **预览**：引擎 `render_text_sample`（PPM）+ `--font-sample` + `api.engine.font_sample` + 插件「预览样张」（`ui.image` 第一个真实用户） |
| `3fb4ba4` | **语言包装配**：`engine::text::build_language_pack` + `--langpack-build`（空表也写、布局、拒绝覆盖、遍历转调 `collect_tree`） |
| `64b7886` | **XML 校对第三模式**：界面接线（一侧为空 ⇒ 隐藏筛选/开始比较，两复选框至少一个，结果进状态行） |

## 二、判据（129 步，新增 5 步）

| 新步 | 判据要点 |
|---|---|
| `langpack-scan` | 四档合成夹具 + unknown 退 2 + **删 ttf 自证会红** + 只读快照 + MODS 三真包（ixr/rus/36 段/ttf>0） |
| `ixr-plugin` | 生成器：cmap 必含码位 / ltx 与文本逐字节未改 / 缺字入账 / 非字体包退 1；插件 headless 装载 + 探针 |
| `font-sample` | PPM 结构/墨迹/缺字口径（空格不算缺）/坏字体退 1/字号 0 不崩 |
| `langpack-build` | 布局 / 空表归一 / 有内容的 XML 逐字节未改 / 默认拒绝覆盖 / `--overwrite` / 两产物至少一个 |
| `xml-extract-mode` | 18 条接线锚点 + **改坏锚点自证会红** |

## 三、这批抓到的真 bug（都不是"顺手"）

1. **`ixrgen` 只加字形不加 cmap** —— `added=2600` 但输出 cmap 里没有「你」；判据读 cmap 才露。
2. **空格被算成缺字**（`missing=1` 假红）—— 改成只有 `FT_Get_Char_Index==0` 才算缺。
3. **逐槽位字源未复位** —— 循环里上一段的字体被带到下一段（两份字体互相污染）。
4. **`langpack.cpp` 自己写递归遍历** —— 被 `walk-one` 判红（D2：递归遍历全仓只有一套），改回 `fs::collect_tree`。
5. **提取模式语言目录推断错** —— 给"多语言父目录"时把父目录名当语言；改成三种形状都认。
6. 两处 rc=3 冻结表要按判据自己的规矩更新（50→51→53），不是放宽判据。

## 四、仍然挂着（下一批）

- IXR 插件：**多语言各一份**（一次出多个 `<lang>`）、**逐槽位并排预览**（原字体 vs 候选）；
- XML 第三模式：**真实提取（改写 scripts/configs）**——现在功能②是"原样搬运"，提取改写还没接；
- 打包产物里**不带** `plugins_ixr`（插件路线：先仓库内开发，稳定后再合内置页）；
- PKG-PAGES 间歇红仍在（本批又撞到过 1 次，复跑绿）。

## 五、口径落档

`docs/2.0.0-插件能力与IXR口径.md`（2026-10-26）——插件定位 / 门面 P2-P4 / IX-Ray 必须施工 / 字体包默认 NotoSansSC / XML 合并五条 / 版本纪律。
