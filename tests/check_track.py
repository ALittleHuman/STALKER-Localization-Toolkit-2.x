# -*- coding: utf-8 -*-
"""追更 CLI 第 2 步：`--track`（manifest 预筛 + 文本级：modified/added/removed 的 .xml）。

真值（合成样本）：
  base: x.xml(ids a,b) / y.xml(id c, 将被删) / gone.txt / JSGME.ini
  cur : x.xml(a 文本改) / new.xml(2 个新 id) / added.txt / JSGME.ini 行序变
⇒ 文件级：added=2 removed=2 modified=1
⇒ 文本级：x(1 changed) + new.xml(2 added) + y.xml(1 removed) = changed_entries=4, changed_files=3
另：cp1251 的 XML 只改 1 条 ⇒ changed_entries=1（证明编码走探测链，不是"当 UTF-8"）
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


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def xml(*entries):
    body = "".join('<string id="%s"><text>%s</text></string>\n' % (i, t) for i, t in entries)
    return '<string_table>\n' + body + '</string_table>\n'


def main() -> int:
    print("== 追更 CLI：--track（含 added/removed XML 与编码）==")
    if not CLI.is_file():
        print("TRACK skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        base = td / "base" / "gamedata" / "configs"
        base.mkdir(parents=True)
        (base / "x.xml").write_text(xml(("a", "A"), ("b", "B")), encoding="utf-8")
        (base / "y.xml").write_text(xml(("c", "C")), encoding="utf-8")
        (td / "base" / "gone.txt").write_text("gone", encoding="utf-8")
        (td / "base" / "JSGME.ini").write_text("mod_a=1\nmod_b=1\n", encoding="utf-8")
        shutil.copytree(td / "base", td / "cur")
        cur = td / "cur" / "gamedata" / "configs"
        (cur / "x.xml").write_text(xml(("a", "A CHANGED"), ("b", "B")), encoding="utf-8")
        (cur / "new.xml").write_text(xml(("n1", "N1"), ("n2", "N2")), encoding="utf-8")
        (cur / "y.xml").unlink()
        (td / "cur" / "added.txt").write_text("added", encoding="utf-8")
        (td / "cur" / "gone.txt").unlink()
        (td / "cur" / "JSGME.ini").write_text("mod_b=1\nmod_a=1\n", encoding="utf-8")

        rc, out = run("--track", td / "base", td / "cur", td / "out1")
        want = ("added=2 removed=2 modified=1 changed_files=3 changed_entries=4 "
                "jsgme_changed=1 changed=1")
        (ok if want in out and rc == 0 else fail)("端到端计数（%s）" % want)
        (ok if "TRACK caps text=xml" in out else fail)("能力清单可见（caps: 只有 xml 做文本级）")

        report = (td / "out1" / "report.json").read_text(encoding="utf-8")
        checks = [
            ('"rel": "gamedata/configs/x.xml", "id": "a", "status": "changed"', "modified xml 的 changed 条目"),
            ('"rel": "gamedata/configs/new.xml", "id": "n1", "status": "added"', "added xml 的 added 条目"),
            ('"rel": "gamedata/configs/y.xml", "id": "c", "status": "removed"', "removed xml 的 removed 条目"),
            ('"caps": {"text": "xml"', "report 里写明能力边界"),
        ]
        for needle, label in checks:
            (ok if needle in report else fail)(label)
        tsvs = list((td / "out1" / "changed").glob("*.tsv"))
        (ok if len(tsvs) == 3 else fail)("三个文件都有 changed/*.tsv（实得 %d）" % len(tsvs))

        run("--track", td / "base", td / "cur", td / "out2")
        h1 = hashlib.sha256((td / "out1" / "report.json").read_bytes()).hexdigest()
        h2 = hashlib.sha256((td / "out2" / "report.json").read_bytes()).hexdigest()
        (ok if h1 == h2 else fail)("同输入两跑 report.json 一致（确定性）")

        # cp1251：只改 1 条文本 ⇒ 必须只报 1 条（编码走探测链）
        cp_old = td / "cp_base" / "text"; cp_new = td / "cp_cur" / "text"
        cp_old.mkdir(parents=True); cp_new.mkdir(parents=True)
        def cp_xml(text):
            return ('<?xml version="1.0" encoding="windows-1251" ?>\n<string_table>\n'
                    '<string id="a"><text>%s</text></string>\n</string_table>\n' % text)
        (cp_old / "cp.xml").write_bytes(cp_xml("Привет").encode("cp1251"))
        (cp_new / "cp.xml").write_bytes(cp_xml("Привет!").encode("cp1251"))
        _, cp_out = run("--track", td / "cp_base", td / "cp_cur", td / "cp_out")
        (ok if "changed_entries=1" in cp_out else fail)(
            "cp1251 只改 1 条 ⇒ changed_entries=1（实得 %s）" % cp_out.strip().splitlines()[-1])

        # 反例：stats 漏报 / text 能报
        _, stats_out = run("--xml-compare-dirs", base.parent.parent, cur.parent.parent, "--mode", "stats")
        _, text_out = run("--xml-compare-dirs", base.parent.parent, cur.parent.parent, "--mode", "text")
        (ok if "text_diff=0" in stats_out and "text_diff=1" in text_out else fail)(
            "反例钉死：stats 漏报、text 能报")

    if PROBLEMS:
        print("TRACK: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("TRACK: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
