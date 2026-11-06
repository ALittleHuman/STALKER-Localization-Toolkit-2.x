# -*- coding: utf-8 -*-
"""最小统一判据（--font-pack）：段表来源、writer 选择、缺字策略、状态隔离。

覆盖：
  ① `--ref builtin` ⇒ `SEGMENTS source=builtin n=15` + `writer=bitmap` + 出包；
  ② `--ref <参照包>` ⇒ `SEGMENTS source=ref n=36`（段表来自扫描，不是内置模板）；
  ③ 状态隔离：跑完 ref 之后，builtin 产物与之前逐字节相同（共享核心不得串状态）；
  ④ `--on-missing fail`（默认）语料缺字 ⇒ rc≠0；`warn` ⇒ rc=0 且打 `MISSING used=N`。
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
CJK = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
LATIN = Path(r"C:\Windows\Fonts\arial.ttf")
PACKS = [Path("E:/Software/Games/STALKER/Games/Chronicles of Pripyat/Chronicles of Pripyat/Chronicles of Pripyat 4.26 IX-Ray/MODS/~NotoSansSC")]
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


def tree_hash(root: Path) -> str:
    h = hashlib.sha256()
    for f in sorted(root.rglob("*")):
        if f.is_file():
            h.update(f.relative_to(root).as_posix().encode())
            h.update(f.read_bytes())
    return h.hexdigest()


def main() -> int:
    print("== 最小统一：--font-pack 段表/ writer / 缺字策略 ==")
    if not CLI.is_file():
        print("FONT-PACK-UNIFIED skip 缺 CLI")
        return 2
    if not CJK.is_file():
        print("FONT-PACK-UNIFIED skip 缺系统字体")
        return 2
    with tempfile.TemporaryDirectory(prefix="fpu_") as td:
        td = Path(td)
        xml = td / "xml"
        xml.mkdir()
        # 语料：中文 + 西里尔（位图 writer 需要 CJK 字体；NotoSansSC 有 CJK 无西里尔 ⇒ 基线警告）
        (xml / "a.xml").write_text(
            '<string_table><string id="a"><text>汉化 Привет</text></string></string_table>',
            encoding="utf-8")

        rc1, out1 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", CJK, "--out", td / "b1")
        (ok if "SEGMENTS source=builtin n=15" in out1 else fail)("① builtin 段数 15")
        (ok if rc1 == 0 and "writer=bitmap" in out1 else fail)("① builtin ⇒ 位图 writer 出包")

        rc2, out2 = run("--font-pack", "--ref", PACKS[0], "--text-root", xml, "--font", CJK,
                        "--out", td / "v1", "--langs", "rus")
        gone = not PACKS[0].is_dir()
        if gone:
            print("  · 参照包不在（跳 ②③）")
        else:
            (ok if "SEGMENTS source=ref n=36" in out2 else fail)("② 参照包 ⇒ 段数 36（扫描真的生效）")
            # ③ 状态隔离：再跑一次 builtin，产物与第一次逐字节相同
            rc3, out3 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                            "--text-root", xml, "--font", CJK, "--out", td / "b2")
            same = (rc3 == 0 and (td / "b1").is_dir() and (td / "b2").is_dir() and
                    tree_hash(td / "b1") == tree_hash(td / "b2"))
            (ok if same else fail)("③ 跑完 ref 后 builtin 产物逐字节相同（状态隔离）")

        # ④ 缺字策略：arial（无 CJK）对中文语料
        (xml / "b.xml").write_text(
            '<string_table><string id="b"><text>汉化</text></string></string_table>', encoding="utf-8")
        rc4, out4 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", LATIN, "--out", td / "m1")
        (ok if rc4 != 0 and "FONTPACK error" in out4 else fail)(
            "④ 默认 fail：语料缺字 ⇒ 不出包（rc=%d）" % rc4)
        rc5, out5 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", LATIN, "--out", td / "m2",
                        "--on-missing", "warn")
        (ok if rc5 == 0 and "MISSING used=" in out5 else fail)(
            "④ --on-missing warn：继续出包且打 MISSING used=N（rc=%d）" % rc5)

    if PROBLEMS:
        print("FONT-PACK-UNIFIED: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FONT-PACK-UNIFIED: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
