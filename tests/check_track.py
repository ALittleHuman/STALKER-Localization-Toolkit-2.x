# -*- coding: utf-8 -*-
"""追更 CLI 第 2 步：`--track`（manifest 预筛 + 只对 modified 的 xml 跑文本对比）。

判据：
  · 合成样本（改 1 条文本 + 加 1 文件 + 删 1 文件）⇒
    `added=1 removed=1 modified=1 changed_files=1 changed_entries=1 changed=1`；
  · `report.json` 里有 `"status": "changed"` 与新旧文本；`changed/*.tsv` 落盘；
  · 同输入两跑 `report.json` sha256 相同（确定性）；
  · **反例（钉死"必须 text"）**：同一对目录 `--xml-compare-dirs --mode stats` ⇒ text_diff=0，
    `--mode text` ⇒ text_diff=1（证明 stats 会漏、track 内部不许回退到 stats）。
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []

XML_OLD = ('<string_table>\n<string id="a"><text>A</text></string>\n'
           '<string id="b"><text>B</text></string>\n</string_table>\n')
XML_NEW = ('<string_table>\n<string id="a"><text>A CHANGED</text></string>\n'
           '<string id="b"><text>B</text></string>\n</string_table>\n')


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 追更 CLI：--track ==")
    if not CLI.is_file():
        print("TRACK skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        base = td / "base" / "gamedata" / "configs"
        base.mkdir(parents=True)
        (base / "x.xml").write_text(XML_OLD, encoding="utf-8")
        (td / "base" / "gone.txt").write_text("gone", encoding="utf-8")
        shutil.copytree(td / "base", td / "cur")
        cur = td / "cur" / "gamedata" / "configs"
        (cur / "x.xml").write_text(XML_NEW, encoding="utf-8")
        (td / "cur" / "added.txt").write_text("added", encoding="utf-8")
        (td / "cur" / "gone.txt").unlink()

        rc, out = run("--track", td / "base", td / "cur", td / "out1")
        want = ("added=1 removed=1 modified=1 changed_files=1 changed_entries=1 "
                "jsgme_changed=0 changed=1")
        (ok if want in out and rc == 0 else fail)("端到端计数（%s）" % want)

        report = (td / "out1" / "report.json").read_text(encoding="utf-8")
        if '"status": "changed"' in report and '"old": "A"' in report and '"new": "A CHANGED"' in report:
            ok("report.json 逐条含 id/status/新旧文本")
        else:
            fail("report.json 缺逐条明细")
        tsv = list((td / "out1" / "changed").glob("*.tsv"))
        (ok if tsv and "changed\ta\tA\tA CHANGED" in tsv[0].read_text(encoding="utf-8") else fail)(
            "changed/*.tsv 落盘且逐条可读")

        run("--track", td / "base", td / "cur", td / "out2")
        h1 = hashlib.sha256((td / "out1" / "report.json").read_bytes()).hexdigest()
        h2 = hashlib.sha256((td / "out2" / "report.json").read_bytes()).hexdigest()
        (ok if h1 == h2 else fail)("同输入两跑 report.json 一致（确定性）")

        # 反例：stats 漏报 / text 能报 —— 这是 track 里"必须 text"的牙
        _, stats_out = run("--xml-compare-dirs", base.parent.parent, cur.parent.parent,
                           "--mode", "stats")
        _, text_out = run("--xml-compare-dirs", base.parent.parent, cur.parent.parent,
                          "--mode", "text")
        if "text_diff=0" in stats_out and "text_diff=1" in text_out:
            ok("反例钉死：stats 漏报（text_diff=0）、text 能报（text_diff=1）")
        else:
            fail("反例不成立：stats/text 的 text_diff 与预期不符")

    if PROBLEMS:
        print("TRACK: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("TRACK: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
