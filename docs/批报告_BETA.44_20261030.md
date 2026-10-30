# 批报告 BETA.44（2026-10-30）

主题：LangPackProfile 独立结构落地（目标 ④ 的最后一块）。

## 改动

`include/xrkit/engine/text/ori.h` 新增：

- `struct OriProfile { lang, lang_source, out_dir, containers }` —— 证据（可落盘、可传下游）
- `ori_profile_of(OriResult)` —— 把"这次生成"抽成"证据"
- `ori_profile_json(OriProfile)` —— 序列化，CLI/落盘/下游共用（旧 `OriResult` 重载保留兼容）

语义分工：`OriResult` = 这次生成干了什么；`OriProfile` = 证据是什么。
文本链产出 profile、IXR 与位图消费 profile，生成阶段不再 detect_langs。

## 门禁异常（已核实为环境）

[sqfs-dos] 两条"独立进程解 xz"自证在门禁里判红（好流/坏流都 rc=1）；
单独重跑 `tests/check_sqfs_dos.py` 全绿、`python -c "import lzma"` 正常。
判定：机器高负载（同日 bench 两次被休眠打断）下的瞬时失败，非代码回归；
重跑 `--fast` 全绿后继续。

## 包

- dist/2.0.0/BETA/44：765 文件，SHELL-SMOKE ok checks=912 failed=0
- --fast PASS（132 步）/ --release PASS（132 步）
