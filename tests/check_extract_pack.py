# -*- coding: utf-8 -*-
"""判据：`--extract-pack` = CLI/GUI 共用的**提取编排**（2026-11-10）。

覆盖：
  · 根 `scripts/` + `configs/scripts/` + `configs/gameplay/` 三个分支一次跑完；
  · 文本表合并、无文本文件不复制；
  · 语言包复制可选；
  · 空成果（目录名合法但无可提取文本）⇒ 非 0，不许 `ok files=0`。

退出码：0=全绿 / 1=断言不成立 / 2=缺 CLI。
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                         "cli" / "xrkit_cli.exe")))
PROBLEMS: list[str] = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label)
        return True
    print("   \u2717 " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append(label + (("（%s）" % detail) if detail else ""))
    return False


def run(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== --extract-pack：CLI/GUI 共用的提取编排 ==")
    if not CLI.is_file():
        print("!! 缺 CLI：%s" % CLI)
        print("EXTRACT-PACK: SKIP (cli-missing)")
        return 2
    work = Path(tempfile.mkdtemp(prefix="xrkit_extract_pack_"))
    try:
        src = work / "src"
        cfg = src / "gamedata" / "configs"
        (cfg / "text" / "rus").mkdir(parents=True)
        (cfg / "gameplay").mkdir(parents=True)
        (cfg / "scripts").mkdir(parents=True)
        (src / "gamedata" / "scripts").mkdir(parents=True)
        (cfg / "localization.ltx").write_text("[string_table]\nlanguage = rus\n",
                                               encoding="utf-8")
        (cfg / "text" / "rus" / "a.xml").write_text(
            "<?xml version='1.0' encoding='utf-8'?><string_table>"
            "<string id=\"a\"><text>源文本</text></string></string_table>",
            encoding="utf-8")
        (cfg / "gameplay" / "g.xml").write_text(
            "<?xml version='1.0' encoding='utf-8'?><string_table>"
            "<string id=\"g\"><text>玩法文本</text></string></string_table>",
            encoding="utf-8")
        (src / "gamedata" / "scripts" / "x.script").write_text(
            'local t = "脚本文本"\n', encoding="utf-8")
        (cfg / "scripts" / "no_text.ltx").write_text(
            "[logic]\nactive = ph_idle@open\n", encoding="utf-8")

        out = work / "out"
        rc, text = run(["--extract-pack", str(src), str(out)])
        want(rc == 0, "extract-pack 退出码 0", "rc=%d\n%s" % (rc, text[-500:]))
        okline = [ln for ln in text.splitlines() if ln.startswith("EXTRACTPACK ok ")]
        want(bool(okline), "有 EXTRACTPACK ok 收尾行", text[-300:])
        line = okline[0] if okline else ""
        want("files=2" in line and "strings=2" in line,
             "根 scripts + gameplay 各 1 个改写；configs/scripts 无文本跳过", line)
        want("notext=1" in line, "无文本 .ltx 计入 notext", line)
        want((out / "gamedata" / "scripts" / "x.script").is_file(),
             "根 scripts 产物落到 gamedata/scripts")
        want((out / "gamedata" / "configs" / "gameplay" / "g.xml").is_file(),
             "gameplay 产物落到 gamedata/configs/gameplay")
        want(not (out / "gamedata" / "configs" / "scripts" / "no_text.ltx").exists(),
             "无文本 configs/scripts 文件不复制")
        texts = out / "gamedata" / "configs" / "text" / "rus"
        want((texts / "gs_scripts_texts.xml").is_file(),
             "根 scripts 文本表在 rus 目录")
        want((texts / "gs_gameplay_texts.xml").is_file(),
             "gameplay 文本表在 rus 目录")

        # 只抽硬编码文本：不复制语言包、不写改写产物
        out2 = work / "out2"
        rc2, text2 = run(["--extract-pack", str(src), str(out2),
                          "--no-language-pack", "--no-rewritten"])
        want(rc2 == 0, "只抽硬编码：退出码 0", text2[-300:])
        want(not (out2 / "gamedata" / "configs" / "text" / "rus" / "a.xml").exists(),
             "只抽硬编码：不复制源语言包 XML")
        want(not (out2 / "gamedata" / "scripts" / "x.script").exists(),
             "只抽硬编码：不写改写脚本")
        want((out2 / "gamedata" / "configs" / "text" / "rus" /
              "gs_scripts_texts.xml").is_file(),
             "只抽硬编码：文本表仍生成")

        # 目录名合法但无文本 ⇒ 非 0
        empty = work / "empty" / "configs" / "scripts"
        empty.mkdir(parents=True)
        (empty.parent / "text" / "rus").mkdir(parents=True)
        (empty.parent / "text" / "rus" / "empty.xml").write_text(
            "<?xml version='1.0' encoding='utf-8'?><string_table></string_table>",
            encoding="utf-8")
        (empty / "only.ltx").write_text("[logic]\n", encoding="utf-8")
        rc3, text3 = run(["--extract-pack", str(work / "empty"), str(work / "out3")])
        want(rc3 != 0 and "未找到可提取的源文件/文本" in text3,
             "空成果返回非 0 并说明原因", text3[-300:])
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("EXTRACT-PACK fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("EXTRACT-PACK ok %d 项（三源编排、无文本跳过、语言包可选、空成果非 0）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
