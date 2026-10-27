# 批报告：BETA.12（2026-10-27）—— 删「文本提取」页 + 扫描全局化 + 提取模式进下拉

> 结论：`--fast` PASS、`--release` PASS（130 步）；包 `dist/2.0.0/BETA/12`（765 文件）；
> 包内自检 `SHELL-SMOKE ok checks=912 failed=0`；**可见页＝5 页**（fs/convert/xml/video/fontpack）；
> `exe_sha256=e37a658261a55f55a0c5310e94387c9259b8a11c3007a47eb65e2562fe30cbc1`。
> BETA.1/2/258/3/4/5/6/7/8/9/10/11 未动。

## 一、用户 2026-10-27 追加的三条

| # | 要求 | 落地 |
|---|---|---|
| ① | 「文本提取」页**删掉** | 从内置页清单摘除（6 → **5 页**）；插件夹具 `text_page.py` 一并删；`text-plugin` 门禁步删除；页类本体保留（提取链的 smoke/引擎判据仍在用） |
| ② | 扫描**全局生效**，并自动判断 SoC/ClS/CoP/IX-R | 参照包行与「扫描参照包」按钮**始终可见**（不再跟 IX-Ray 绑定）；扫描后**自动切下拉**（ixr→IX-Ray / cop→CoP / cis→ClS / soc→SoC），并预填空的「文本 XML」目录 |
| ③ | 提取模式要出现在**下拉第三项** | 一侧为空 ⇒ 模式下拉**真的多出「提取文本」**并选中；两侧非空 ⇒ 自动移除。目录不存在也算空；`textChanged` 也触发（浏览/粘贴都能触发） |

## 二、连带改动（删页的波纹，全部有意为之）

- `page_factory` / `smoke` 期望表与槽位表 / `package.ps1` 的 `$builtinSlots` / `check_package_fixture_pages` 的 `BUILTIN_IDS` / `check_release_criterion` 的页数与名单：6 → 5；
- `check_package_plugin_list` 的 `EXPECTED_PAGES`、`package.ps1` 的 `$fixturePages`、`FIXTURE_PAGES`/`PLUGIN_IDS`：去掉 `text_page.py`（7 → 6 枚页夹具；抬起来后 6 个插件页）；
- smoke 两处阈值（拖放输入 14→12、区域 13→12）与 P7c 页数 6→5；
- smoke 的"下拉底令牌"判据改成**逐页切过去扫**（只看当前页会得到 0 个样本 ⇒ 恒红）。

## 三、本批抓到的真 bug

1. **`LangPackModel::slots` 在 Qt 的 TU 里编不过** —— `slots` 是 Qt 宏（`Q_SLOTS`）⇒ 改名 `font_slots`（引擎/CLI/facade 同步）；
2. **真包夹具路径过期**：用户把 `MODS` 挪到了游戏目录**上一级**、且新增了去 `~` 的副本 ⇒ 判据改成"候选根 + 两种前缀"解析；
3. **`check_release_criterion` 里两处写死 6**（页数与名单）⇒ 一并改 5。

## 四、仍挂着

1. IX-Ray 页面还没暴露**逐槽位选字体**与**分区（--sizes）**（引擎/CLI 已支持）；
2. 被摘掉的 `TextPage` 类仍在 `src/shell/pages/`（只剩 smoke/引擎判据在用）；要不要连同 5 处 smoke 提取链检查一起清，得单独决定；
3. PKG-PAGES 间歇红仍未定性。
