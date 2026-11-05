# -*- coding: utf-8 -*-
"""P0 修复的判据（2026-11-05 交接）：中文路径、--only BOM、零命中退出码、XML 必现算。

① `--snapshot` / `--track` 在**中文路径**下必须与 ASCII 路径行为一致（不再"不是目录"）；
② `--only` 清单带 UTF-8 BOM ⇒ 仍能命中（剥 BOM）；
③ `--only` 清单一条都不命中 ⇒ **非零退出**（零产物不许 exit=0）；
④ `--baseline` 下 **XML 永远现算**：旧树同 size/mtime 内容被换 ⇒ 不许复用旧 sha（会漏更新）。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []
XML_A = '<string_table>\n<string id="a"><text>A</text></string></string_table>\n'
XML_B = '<string_table>\n<string id="a"><text>B</text></string></string_table>\n'


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
    print("== P0 修复：中文路径 / BOM / 零命中 / XML 必现算 ==")
    if not CLI.is_file():
        print("P0-FIXES skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory(prefix="p0_") as td:
        td = Path(td)
        cn = td / "中文路径" / "汉化" / "old"
        cn.mkdir(parents=True)
        (cn / "x.xml").write_text(XML_A, encoding="utf-8")

        rc1, out1 = run("--snapshot", cn, td / "中文路径" / "snap.json")
        (ok if rc1 == 0 and "SNAPSHOT ok" in out1 else fail)(
            "① 中文路径下 --snapshot 能跑（rc=%d）" % rc1)
        rc2, out2 = run("--track", cn, cn, td / "中文路径" / "out")
        (ok if rc2 == 0 and "TRACK ok" in out2 else fail)(
            "① 中文路径下 --track 能跑（rc=%d）" % rc2)

        # ② --only 清单带 BOM
        src = td / "src"
        src.mkdir()
        (src / "a.xml").write_text(XML_A, encoding="utf-8")
        pack = td / "p.sqfs"
        rc3, _ = run("--pack-sqfs", pack, src)
        (ok if rc3 == 0 and pack.is_file() else fail)("准备 sqfs 样本")
        listed = td / "only.txt"
        listed.write_bytes(b"\xef\xbb\xbfa.xml\n")
        rc4, out4 = run("--unpack-tree", pack, td / "out_only", "--only", listed)
        (ok if rc4 == 0 and "files=1" in out4 else fail)(
            "② BOM 清单仍命中 1 项（rc=%d）" % rc4)

        bad = td / "bad.txt"
        bad.write_text("no/such.txt\n", encoding="utf-8")
        rc5, out5 = run("--unpack-tree", pack, td / "out_bad", "--only", bad)
        (ok if rc5 != 0 and "files=0" in out5 else fail)(
            "③ 零命中 ⇒ 非零退出且产物数如实为 0（rc=%d, %s）"
            % (rc5, out5.strip().splitlines()[-1] if out5.strip() else ""))

        # ④ baseline 下 XML 必现算：旧树同 size/mtime 换成 B，current 也是 B ⇒ 结果必须"没变化"
        old = td / "old"
        old.mkdir()
        (old / "x.xml").write_text(XML_A, encoding="utf-8")
        stat = (old / "x.xml").stat()
        run("--snapshot", old, td / "old.json")
        cur = td / "cur"
        shutil.copytree(old, cur)
        (cur / "x.xml").write_text(XML_B, encoding="utf-8")
        (old / "x.xml").write_text(XML_B, encoding="utf-8")
        os.utime(old / "x.xml", (stat.st_atime, stat.st_mtime))
        rc6, out6 = run("--track", old, cur, td / "out_xml", "--baseline", td / "old.json")
        (ok if rc6 == 0 and "changed=0" in out6 else fail)(
            "④ 同 size/mtime 换内容的 XML 不许用旧 sha（rc=%d, %s）"
            % (rc6, out6.strip().splitlines()[-1] if out6.strip() else ""))

    # ⑤ 编码别名 + ⑥ --pack-layout（P1-3 / P1-4；用独立临时目录）
    with tempfile.TemporaryDirectory(prefix="p1_") as td2s:
        td2 = Path(td2s)
        raw = td2 / "cp.bin"
        raw.write_bytes(bytes([0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2]))   # "Привет" in cp1251
        rc7, out7 = run("--codec-lossy", "decode", "cp1251", raw, td2 / "cp.txt")
        (ok if rc7 == 0 and "CODEC ok" in out7 else fail)(
            "⑤ cp1251 别名可用（rc=%d, %s）" % (rc7, out7.strip()[:120]))

        lp = td2 / "lp"
        (lp / "text" / "rus").mkdir(parents=True)
        (lp / "text" / "rus" / "a.xml").write_text(XML_A, encoding="utf-8")
        rc8, out8 = run("--langpack-build", lp / "text" / "rus", "-", lp / "pack", "--pack-layout")
        want = lp / "pack" / "gamedata" / "configs" / "text" / "rus" / "a.xml"
        (ok if rc8 == 0 and want.is_file() else fail)(
            "⑥ --pack-layout 产出 gamedata/configs/text/rus/a.xml（rc=%d）" % rc8)

    if PROBLEMS:
        print("P0-FIXES: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("P0-FIXES: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
