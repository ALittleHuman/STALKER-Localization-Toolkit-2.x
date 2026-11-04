# -*- coding: utf-8 -*-
"""追更加速器：`--track ... --baseline <old.json>` / `--rehash`（DSH/HA 2026-11-04）。

钉死的三条硬约束：
  ① 复用条件保守：**size 且 mtime 都相等**才复用旧 sha256；
  ② 逃生口 `--rehash`：忽略缓存全部重算，且结果必须与"无 baseline"逐字节相同；
  ③ root 指纹（绝对路径 + 文件数 + 总字节数）不符 ⇒ 报错。
另加（HA）：old_root 内容与快照不一致 ⇒ 报错，不许静默用旧 hash。
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []
XML_OLD = '<string_table>\n<string id="a"><text>A</text></string></string_table>\n'
XML_NEW = '<string_table>\n<string id="a"><text>A CHANGED</text></string></string_table>\n'


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main() -> int:
    print("== 追更加速器：--baseline / --rehash ==")
    if not CLI.is_file():
        print("BASELINE skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        old = td / "old" / "gamedata"
        old.mkdir(parents=True)
        (old / "x.xml").write_text(XML_OLD, encoding="utf-8")
        (old / "keep.txt").write_text("keep", encoding="utf-8")
        shutil.copytree(td / "old", td / "new")
        (td / "new" / "gamedata" / "x.xml").write_text(XML_NEW, encoding="utf-8")

        run("--snapshot", td / "old", td / "old.json")
        rc0, out0 = run("--track", td / "old", td / "new", td / "out_nb")
        rc1, out1 = run("--track", td / "old", td / "new", td / "out_b", "--baseline", td / "old.json")
        rc2, out2 = run("--track", td / "old", td / "new", td / "out_r", "--baseline", td / "old.json", "--rehash")

        (ok if rc0 == rc1 == rc2 == 0 else fail)("三种跑法都成功（rc=%d/%d/%d）" % (rc0, rc1, rc2))
        same_b = sha(td / "out_nb" / "report.json") == sha(td / "out_b" / "report.json")
        same_r = sha(td / "out_nb" / "report.json") == sha(td / "out_r" / "report.json")
        (ok if same_b else fail)("有 baseline 与无 baseline 逐字节相同")
        (ok if same_r else fail)("--rehash 与无 baseline 逐字节相同")

        rc3, out3 = run("--track", td / "old", td / "new", td / "out_missing", "--baseline", td / "nope.json")
        (ok if rc3 != 0 and "读不了 baseline" in out3 else fail)("缺 baseline json ⇒ 报错")

        rc4, out4 = run("--track", td / "nope_dir", td / "new", td / "out_noroot", "--baseline", td / "old.json")
        (ok if rc4 != 0 and "不是目录" in out4 else fail)("缺 old_root ⇒ 报错（不是静默写 old=\"\"）")

        # 指纹不符：加一个文件（文件数变）
        (td / "old" / "gamedata" / "extra.txt").write_text("x", encoding="utf-8")
        rc5, out5 = run("--track", td / "old", td / "new", td / "out_fp", "--baseline", td / "old.json")
        (ok if rc5 != 0 and "指纹不符" in out5 else fail)("指纹不符 ⇒ 报错")
        (td / "old" / "gamedata" / "extra.txt").unlink()

        # 旧树被动过（同大小、mtime 变）⇒ 报错，指出文件
        p = td / "old" / "gamedata" / "keep.txt"
        st = p.stat()
        p.write_text("KEEP", encoding="utf-8")
        os.utime(p, (st.st_atime, st.st_mtime + 2))
        rc6, out6 = run("--track", td / "old", td / "new", td / "out_changed", "--baseline", td / "old.json")
        (ok if rc6 != 0 and "old_root 与 baseline 快照不一致" in out6 and "keep.txt" in out6
         else fail)("old_root 被动过 ⇒ 报错并指出文件")
        # --rehash 下允许（明确要求全量重算）
        rc7, out7 = run("--track", td / "old", td / "new", td / "out_rehash2", "--baseline", td / "old.json", "--rehash")
        (ok if rc7 == 0 else fail)("--rehash 时允许旧树变化（全部重算，rc=%d）" % rc7)

    if PROBLEMS:
        print("BASELINE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("BASELINE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
