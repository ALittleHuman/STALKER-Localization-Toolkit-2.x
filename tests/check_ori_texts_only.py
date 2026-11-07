# -*- coding: utf-8 -*-
"""「提取文本」的两条硬判据（2026-11-07，用户口径：提取就是提取文本）。

① 已有译文不许被覆盖：容器里 text/rus 与 text/eng 都有同一文件 ⇒ 跑 ori 之后
   rus 那份**逐字节不变**（修前会被 eng 覆盖 —— 实测 92 个文件就是这么坏的）。
② --texts-only：产出里**不许出现** anims/textures 这类整树复制物；
   但字面量表必须还在。
"""
from __future__ import annotations

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


def build_src(root: Path) -> None:
    gd = root / "gamedata"
    (gd / "configs" / "text" / "rus").mkdir(parents=True)
    (gd / "configs" / "text" / "eng").mkdir(parents=True)
    (gd / "configs" / "text" / "rus" / "st_keep.xml").write_bytes(
        b'<string_table><string id="a"><text>\xd0\xa0\xd0\xa3\xd0\xa1</text></string></string_table>')
    (gd / "configs" / "text" / "eng" / "st_keep.xml").write_bytes(
        b'<string_table><string id="a"><text>ENG</text></string></string_table>')
    (gd / "scripts").mkdir(parents=True)
    (gd / "scripts" / "a.script").write_text(
        'function f()\n  local s = "Hello world"\nend\n', encoding="utf-8")
    (gd / "anims").mkdir(parents=True)
    (gd / "anims" / "x.anm").write_bytes(b"\x00\x01\x02binary")


def main() -> int:
    print("== 提取文本：不覆盖译文 / 不复制整树 ==")
    if not CLI.is_file():
        print("ORI-TEXTS skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory(prefix="ori_t_") as td:
        td = Path(td)
        src = td / "src"
        build_src(src)
        rus_before = (src / "gamedata" / "configs" / "text" / "rus" / "st_keep.xml").read_bytes()
        eng_before = (src / "gamedata" / "configs" / "text" / "eng" / "st_keep.xml").read_bytes()

        rc1, out1 = run("--ori-tree", src, td / "out_full")
        if rc1 == 0:
            rus_after = (td / "out_full" / "gamedata" / "configs" / "text" / "rus" / "st_keep.xml")
            kept = rus_after.is_file() and rus_after.read_bytes() == rus_before
            (ok if kept else fail)("① 已有 rus 译文没被 eng 覆盖")
            eng_after = (td / "out_full" / "gamedata" / "configs" / "text" / "eng" / "st_keep.xml")
            (ok if eng_after.is_file() and eng_after.read_bytes() == eng_before else fail)(
                "① eng 原文也没被改")
            (ok if (td / "out_full" / "gamedata" / "anims" / "x.anm").is_file()
             else fail)("① 整树模式仍复制 anims（默认行为未变）")
        else:
            fail("整树模式跑失败 rc=%d\n%s" % (rc1, out1[-300:]))

        rc2, out2 = run("--ori-tree", src, td / "out_texts", "--texts-only")
        if rc2 == 0:
            (ok if not (td / "out_texts" / "gamedata" / "anims").exists()
             else fail)("② --texts-only 不复制 anims（提取不是解包）")
            texts = list((td / "out_texts").rglob("*_texts.xml"))
            (ok if texts else fail)("② --texts-only 仍然产出字面量表（%d 个）" % len(texts))
        else:
            fail("--texts-only 跑失败 rc=%d\n%s" % (rc2, out2[-300:]))

        src_changed = []
        for p in src.rglob("*"):
            if p.is_file():
                rel = p.relative_to(src).as_posix()
                if rel.endswith("st_keep.xml"):
                    continue
        (ok if not src_changed else fail)("源目录未被改写")

    if PROBLEMS:
        print("ORI-TEXTS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("ORI-TEXTS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
