# 批报告 BETA.39（2026-10-30）

主题：把「ori 树」的**扫描证据**从文本链一路接到 IXR 生成器，并落成产物文件。

## 本批累计（BETA.33 → BETA.39）

| 步 | 内容 |
|---|---|
| 6 | 参考包 ixr_addons/ 白名单收窄到 configs/text、configs/ui、onts、两份 ltx |
| 7 | 字体合并后同步 hhea/head/OS-2 纵向度量（低 ascent 夹具：need=2129, got=2129×4） |
| 8 | 页面语言选择 → IXR --langs（主线程快照；worker 读控件被线程判据抓过） |
| 9 | ori_profile_json() 提升为引擎能力，CLI 只打印 |
| 10 | 语言证据三态夹具：localization.ltx / 	ext_dir / rg / allback |
| 11 | Profile 落盘 ori/_xrkit_profile.json，与 stdout 行**逐字节一致**是判据 |
| 12 | IXR 生成器采用 profile 语言；profile 与包不符 ⇒ 如实 skipped |

## 判据

`
tests/check_ori_tree.py   27 条（容器/复制/四分支/语言三态/只读/拒绝覆盖/profile 落盘）
tests/check_ixr_gen.py    profile 语言采用 + 不符 skipped + 白名单 + 纵向度量
tests/check_ixr_product.py 页面语言接线锚点
`

## 门禁与包

`
--fast PASS（132 步）   --release PASS（132 步）
dist/2.0.0/BETA/39   765 文件   SHELL-SMOKE ok checks=912 failed=0
`

## 环境假红记录（两次，均未改阈值）

1. 2026-10-29 23:59 → 07:43：机器休眠 7.5 h，release 卡在 [28] bench；
2. 2026-10-30 12:00 前后：bench 三连跑之一 = **3,603,952 ms**（机器休眠 1 h）；
   单独重跑 bench c=0 / 261 s / BENCH PASS metrics=6 failed=0，重跑门禁全绿。

## 仍未做

- **位图链仍自扫语言**（uild_package 的 opt.lang 默认 chs），未读 _xrkit_profile.json；
- LangPackProfile 还没有独立模型（语言/容器在 OriResult 里）；
- 页面仍保留两个复选框（只当「至少勾一个」护栏）。