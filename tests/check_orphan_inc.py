#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`.inc` 生成物的**孤儿检测**（审计 2026-10-06 补充 H1-1 的判据原文）。

审计原文（H1-1，标 S1）：
  · `src/engine/fs/cp1251_tables.inc`（手写/历史：0x98 → `0xFFFD`）与
    生成物 `src/engine/text/cp1251_tables.inc`（0x98 → `0xFFFF`，严格解码必须失败）
    **是同名两份副本、内容不同**；
  · 生成器只写 text/（`tools/gen_encoding_tables.py` 的 `OUT_DIR`），门禁只查 text/，
    而 `src/engine/fs/db.cpp` 用的是**引号 include** ⇒ 先命中同目录的 fs 版；
    把 fs 版删掉/改名，db.cpp 会**静默**改用 0xFFFF 那份且编译不报错（0x98 的语义就此改变）；
  · 判据（原文）：**遍历 `src/**/*.inc`，要求每个都能被某生成器声明产出** ⇒ 直接抓 orphan。

本步落地三件事：
  ① **孤儿检测**：`src/**/*.inc` 每一份都必须在 `GENERATED` 清单里，且清单里的产出者
     （`tools/gen_*.py`）**确实还写着**那个文件名 —— 删掉生成器里的那行也红；
  ② 两份 cp1251 表**都由生成器产出**（fs 版不再是手写孤儿）；
  ③ 两份的差异**只许是有意的那一处**：数值表逐项比，除 `0x98`（下表第 24 项）外
     必须**完全一致** —— 防"同族数据悄悄各改一处"（H1-1 的根因就是这种分叉）。
  ④ 上面三条的分析器都要**自证**：喂反例必须报红。

退出码：0 = 全绿；1 = 有判据不成立。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 单一声明清单：产出物 → 产出者。**每一条都会被双向校验**（见 analyze_orphans）。
GENERATED: dict[str, str] = {
    "src/core/theme_tables.inc": "tools/gen_theme_tables.py",
    "src/engine/font/adv_tables.inc": "tools/gen_font_tables.py",
    "src/engine/font/charset_groups.inc": "tools/gen_font_templates.py",
    "src/engine/font/eaw_tables.inc": "tools/gen_font_tables.py",
    "src/engine/font/ltx_templates.inc": "tools/gen_font_templates.py",
    "src/engine/font/st_fallback.inc": "tools/gen_font_tables.py",
    "src/engine/plugin/contract_tables.inc": "tools/gen_plugin_contract.py",
    # ★ H1-1 的主角：fs 版从"手写孤儿"改成**同一个编码表生成器**的产物
    "src/engine/fs/cp1251_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/cp1251_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/cp1252_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/big5_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/euc_kr_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/gb18030_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/gb2312_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/gbk_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/shift_jis_tables.inc": "tools/gen_encoding_tables.py",
    "src/engine/text/sniff_profiles.inc": "tools/gen_sniff_profiles.py",
}

FAILURES: list[str] = []


def say(line: str) -> None:
    print(line, flush=True)


def check(ok: bool, label: str, detail: str = "") -> None:
    if ok:
        say("  \u2713 %s" % label)
    else:
        FAILURES.append(label)
        say("  \u2717 %s%s" % (label, ("\n      " + detail) if detail else ""))


# ── ① 孤儿检测（纯函数：喂反例可自证）────────────────────────────────────────

def analyze_orphans(files: list[str], manifest: dict[str, str],
                    producers: dict[str, str]) -> list[str]:
    """返回问题清单（空 = 全绿）。

    `files`     磁盘上真实存在的 `src/**/*.inc`（POSIX 相对路径）
    `manifest`  声明清单：产出物 → 产出者（仓库相对路径）
    `producers` 产出者源码文本：产出者路径 → 文本（用来交叉核对"它还写着这个文件名"）
    """
    problems: list[str] = []
    for rel in sorted(files):
        owner = manifest.get(rel)
        if owner is None:
            problems.append("%s 不在生成物清单里（**孤儿**：没人声明产出它，手改/删掉没人管）" % rel)
            continue
        text = producers.get(owner)
        if text is None:
            problems.append("%s 声明的产出者不存在：%s" % (rel, owner))
            continue
        if Path(rel).name not in text:
            problems.append("%s 声明的产出者 %s 里**已经不再产出**它（文件名在生成器里找不到）"
                            % (rel, owner))
    for rel, owner in sorted(manifest.items()):
        if rel not in files:
            problems.append("清单里的 %s 在磁盘上**不存在**（被删掉/改名了？产出者 %s）" % (rel, owner))
        if not owner.endswith(".py"):
            problems.append("清单里的产出者不像生成器脚本：%s → %s" % (rel, owner))
    return problems


def run_orphan_check() -> None:
    files = [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / "src").rglob("*.inc"))]
    check(bool(files), "src/ 下确实扫到了 *.inc（判据不是空集真空跑）",
          "找到 0 个 .inc —— 扫描口径坏了就是恒绿")
    producers: dict[str, str] = {}
    for owner in set(GENERATED.values()):
        path = ROOT / owner
        producers[owner] = path.read_text(encoding="utf-8") if path.is_file() else ""
    problems = analyze_orphans(files, GENERATED, producers)
    check(not problems, "src/**/*.inc 每一份都被某个生成器声明产出（H1-1 的判据原文）",
          "\n      ".join(problems))
    say("  （扫描 %d 份 .inc；清单 %d 条）" % (len(files), len(GENERATED)))

    # ★ 自证：同一个分析器吃三组反例必须报红（防"判据恒真"）
    bad1 = analyze_orphans(["src/engine/fs/orphan.inc"], {}, {})
    check(bool(bad1), "自证：孤儿 .inc 必须被判红", str(bad1))
    bad2 = analyze_orphans(["src/a.inc"], {"src/a.inc": "tools/gen_x.py"},
                           {"tools/gen_x.py": "# 里没有任何 .inc 文件名\n"})
    check(bool(bad2), "自证：产出者里不再产出它必须被判红", str(bad2))
    bad3 = analyze_orphans([], {"src/gone.inc": "tools/gen_x.py"}, {"tools/gen_x.py": "gone.inc"})
    check(bool(bad3), "自证：清单里有、磁盘上没有必须被判红", str(bad3))


# ── ③ 两份 cp1251 只许一处有意差异 ──────────────────────────────────────────

ARRAY_RE = re.compile(r"kCp1251ToUnicode\[128\]\s*=\s*\{(.*?)\}", re.S)


def parse_table(text: str) -> list[int] | None:
    match = ARRAY_RE.search(text)
    if match is None:
        return None
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", match.group(1))]


def compare_cp1251(fs_text: str, text_text: str) -> list[str]:
    problems: list[str] = []
    left = parse_table(fs_text)
    right = parse_table(text_text)
    if left is None or right is None:
        return ["两份 cp1251 表里没有找到 `kCp1251ToUnicode[128]` 数组"]
    if len(left) != 128 or len(right) != 128:
        return ["数组长度不是 128（fs=%d text=%d）" % (len(left), len(right))]
    diff = [i for i in range(128) if left[i] != right[i]]
    # 下标 i 对应字节 0x80 + i ⇒ 0x98 是第 24 项（0 基）
    if diff != [0x98 - 0x80]:
        problems.append("两份表的差异不止 0x98 一处：下标 %s（字节 %s）"
                        % (diff, ["0x%02X" % (0x80 + i) for i in diff]))
    if left[0x98 - 0x80] != 0xFFFD:
        problems.append("fs 版 0x98 应该是 0xFFFD（1.x `decode(cp1251, replace)` 语义），实得 0x%04X"
                        % left[0x98 - 0x80])
    if right[0x98 - 0x80] != 0xFFFF:
        problems.append("text 版 0x98 应该是 0xFFFF（严格解码必须失败），实得 0x%04X"
                        % right[0x98 - 0x80])
    return problems


def run_cp1251_check() -> None:
    fs_path = ROOT / "src" / "engine" / "fs" / "cp1251_tables.inc"
    text_path = ROOT / "src" / "engine" / "text" / "cp1251_tables.inc"
    for path in (fs_path, text_path):
        if not path.is_file():
            check(False, "两份 cp1251 表都在（%s）" % path.relative_to(ROOT).as_posix())
            return
    problems = compare_cp1251(fs_path.read_text(encoding="utf-8"),
                              text_path.read_text(encoding="utf-8"))
    check(not problems, "两份 cp1251 表**只差 0x98 一处**（其余 127 项必须逐项相同）",
          "\n      ".join(problems))

    # ★ 自证：把一项无关的值改掉 ⇒ 必须报红
    tampered = (fs_path.read_text(encoding="utf-8")
                .replace("0x0402, 0x0403", "0x0403, 0x0403", 1))
    check(bool(compare_cp1251(tampered, text_path.read_text(encoding="utf-8"))),
          "自证：两份表在 0x98 之外再有差异必须被判红")


def main() -> int:
    say("ORPHAN-INC 生成物孤儿检测 + cp1251 两份副本的差异面（H1-1）")
    run_orphan_check()
    run_cp1251_check()
    if FAILURES:
        say("ORPHAN-INC: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("ORPHAN-INC: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
