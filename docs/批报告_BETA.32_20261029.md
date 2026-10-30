# 批报告 BETA.32（2026-10-29）

## 本批主题

把「ori 树」的**扫描 → 生成合同**钉住：生成只消费一条 ORI profile=，下游禁止再猜语言/源根。

## 改动

1. CLI --ori-tree 生成成功后追加一行稳定契约：

`
ORI profile={"lang":…,"lang_source":…,"out":…,"containers":[
  {"rel":…,"lang":…,"lang_source":…,"kind":…,"has_text":…,"has_fonts":…,
   "has_scripts":…,"has_gameplay":…,"extracted":…,"rewritten":…,"texts":…}]}
`

2. lang_source ∈ {localization.ltx, text_dir, arg, fallback}：语言从哪来必须写清楚，
   生成阶段不再 detect_langs。
3. 判据 check_ori_tree 新增三条：
   - Profile 行可解析；
   - Profile 容器集合 == 生成用的容器集合（gamedata / ixr_addons/~bodycam / bc）；
   - 每个容器都带 lang_source。

## 判据与门禁

`
tests/check_ori_tree.py            ORI-TREE: PASS（20 条）
--fast   PASS（132 步）
--release PASS（132 步）
`

## 包

`
dist/2.0.0/BETA/32   765 文件
SHELL-SMOKE ok checks=912 failed=0 not_covered=0
`

## 未做（下一步）

- 位图 / IXR 两条后端还没有消费同一份 Profile（目前只有文本链的 --ori-tree 吐契约行）；
- 字体 hhea/OS2/head 纵向度量；
- 参考包 ixr_addons/ 白名单收窄；
- 页面两个复选框合并成单一「生成 ori」按钮。