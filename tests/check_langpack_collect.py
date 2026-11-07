# -*- coding: utf-8 -*-
"""语言包收敛判据（--langpack-collect）：以**真实参照包**为金标准。

参照包：MODS/汉化/rus（231 文件）。
断言：对参照包自身跑一次 collect ⇒ 输出与参照包 **逐字节相同**（路径全集 + 内容）。
另加：源树里塞噪声（scripts/gameplay/anims/text/eng）⇒ 输出**不许**包含它们。
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
REF = Path("E:/Software/Games/STALKER/Games/Chronicles of Pripyat/Chronicles of Pripyat/MODS/汉化/rus")
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def files(root: Path):
    return {p.relative_to(root).as_posix(): p for p in root.rglob("*") if p.is_file()}


def main() -> int:
    print("== 语言包收敛：以参照包为金标准 ==")
    if not CLI.is_file():
        print("LANGPACK-COLLECT skip 缺 CLI")
        return 2
    if not REF.is_dir():
        print("LANGPACK-COLLECT skip 缺参照包")
        return 2
    with tempfile.TemporaryDirectory(prefix="lpc_") as td:
        td = Path(td)
        src = td / "src"
        # 参照包 + 噪声
        import shutil
        shutil.copytree(REF, src)
        (src / "gamedata" / "scripts").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "scripts" / "a.script").write_text("-- noise\n", encoding="utf-8")
        (src / "gamedata" / "anims").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "anims" / "x.anm").write_bytes(b"\x00noise")
        (src / "gamedata" / "configs" / "text" / "eng").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "configs" / "text" / "eng" / "st_x.xml").write_text(
            "<string_table></string_table>", encoding="utf-8")

        out = td / "out"
        p = subprocess.run([str(CLI), "--langpack-collect", str(src), str(out), "--lang", "rus"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = p.stdout.decode("utf-8", "replace")
        if p.returncode != 0:
            fail("collect 失败 rc=%d\n%s" % (p.returncode, log[-300:]))
            print("LANGPACK-COLLECT: FAIL")
            return 1
        ref, got = files(REF), files(out)
        only_ref = sorted(set(ref) - set(got))
        only_got = sorted(set(got) - set(ref))
        (ok if not only_ref else fail)("与参照包路径齐全（缺 %d：%s）"
                                       % (len(only_ref), only_ref[:3]))
        (ok if not only_got else fail)("没有多余文件（多 %d：%s）"
                                       % (len(only_got), only_got[:3]))
        changed = [r for r in ref if r in got and sha(ref[r]) != sha(got[r])]
        (ok if not changed else fail)("逐字节相同（不同 %d：%s）" % (len(changed), changed[:3]))
        noise = [r for r in got if r.startswith(("gamedata/scripts/", "gamedata/anims/"))
                 or "/text/eng/" in r]
        (ok if not noise else fail)("噪声没被收敛进来（%s）" % (noise[:3] or "无"))

    if PROBLEMS:
        print("LANGPACK-COLLECT: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("LANGPACK-COLLECT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
