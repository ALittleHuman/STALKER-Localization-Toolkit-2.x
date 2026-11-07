# -*- coding: utf-8 -*-
"""语言包收敛判据（--langpack-collect）—— **规则判据，不拿参考包当答案**。

规则（来自口径，不是从样本反推）：
  收：任意前缀的 …/configs/text/<lang>/**、…/configs/{fonts,localization}.ltx、
      与某个 configs **同根**的 …/fonts/**
  不收：scripts / gameplay / anims / textures / meshes / sounds …
  语言名从路径扫出来；--lang 可过滤；源里没有语言时明确失败。

判据全部用**合成树**；真实参考包只打印一条“对照”信息，不影响 PASS/FAIL。
"""
from __future__ import annotations

import hashlib
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


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def files(root: Path):
    return {p.relative_to(root).as_posix(): p for p in root.rglob("*") if p.is_file()}


def run(src: Path, out: Path, *extra) -> tuple[int, str]:
    p = subprocess.run([str(CLI), "--langpack-collect", str(src), str(out), *extra],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 语言包收敛：规则判据（合成树） ==")
    if not CLI.is_file():
        print("LANGPACK-COLLECT skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory(prefix="lpc_") as td:
        td = Path(td)
        src = td / "src"
        def put(rel: str, data: bytes = b"<string_table/>") -> Path:
            p = src / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
            return p

        # 两份不同前缀的语言树 + 各种噪声
        put("gamedata/configs/text/rus/rus_a.xml")
        put("gamedata/configs/text/eng/eng_a.xml")
        put("gamedata/configs/fonts.ltx", b"[fonts]\n")
        put("gamedata/configs/localization.ltx", b"[string_table]\n")
        put("gamedata/fonts/rus/f.ttf", b"ttf")
        put("mods/foo/configs/text/eng/foo.xml")
        put("mods/foo/configs/fonts.ltx", b"[fonts]\n")
        put("mods/foo/fonts/eng/g.ttf", b"ttf")
        # 噪声：都不是语言包
        put("gamedata/scripts/s.script", b"-- x\n")
        put("gamedata/gameplay/x.xml")
        put("gamedata/anims/x.anm", b"\x00")
        put("gamedata/textures/map/m.dds", b"dds")
        put("gamedata/sounds/s.ogg", b"ogg")
        put("stray/not_configs/fonts/x.ttf", b"ttf")   # 没有同根 configs ⇒ 不收

        expected = {
            "gamedata/configs/text/rus/rus_a.xml",
            "gamedata/configs/text/eng/eng_a.xml",
            "gamedata/configs/fonts.ltx",
            "gamedata/configs/localization.ltx",
            "gamedata/fonts/rus/f.ttf",
            "mods/foo/configs/text/eng/foo.xml",
            "mods/foo/configs/fonts.ltx",
            "mods/foo/fonts/eng/g.ttf",
        }

        out = td / "out"
        rc, log = run(src, out)
        if rc != 0:
            fail("collect 失败 rc=%d\n%s" % (rc, log[-300:]))
        else:
            got = files(out)
            (ok if set(got) == expected else fail)(
                "只收规则内的文件（缺 %s / 多 %s）"
                % (sorted(expected - set(got))[:3], sorted(set(got) - expected)[:3]))
            changed = [r for r in expected if r in got and sha(src / r) != sha(got[r])]
            (ok if not changed else fail)("逐字节相同（不同 %d）" % len(changed))
            (ok if "gamedata/textures/map/m.dds" not in got else fail)("textures 不收")
            (ok if "gamedata/scripts/s.script" not in got else fail)("scripts 不收")
            (ok if "stray/not_configs/fonts/x.ttf" not in got else fail)(
                "没有同根 configs 的 fonts 不收")

        # --lang 过滤（只影响 configs/text/<lang>；字体不按语言过滤）
        out_eng = td / "out_eng"
        rc2, log2 = run(src, out_eng, "--lang", "eng")
        if rc2 == 0:
            got2 = files(out_eng)
            has_eng = "gamedata/configs/text/eng/eng_a.xml" in got2
            no_rus = "gamedata/configs/text/rus/rus_a.xml" not in got2
            (ok if has_eng and no_rus else fail)("--lang eng 只收 eng 文本")
        else:
            fail("--lang eng 失败 rc=%d\n%s" % (rc2, log2[-200:]))

        # 空源：明确失败
        empty = td / "empty"
        empty.mkdir()
        rc3, log3 = run(empty, td / "out_empty")
        (ok if rc3 != 0 and "没有可收敛" in log3 else fail)("空源 ⇒ 明确失败（rc=%d）" % rc3)

        # 输出落在源内：拒绝
        rc4, log4 = run(src, src / "inner")
        (ok if rc4 != 0 else fail)("输出与源重叠 ⇒ 拒绝（rc=%d）" % rc4)

        # 参考包只做**对照**，不参与判据
        ref = Path("E:/Software/Games/STALKER/Games/Chronicles of Pripyat/Chronicles of Pripyat/MODS/\u6c49\u5316/rus")
        if ref.is_dir():
            ref_files = files(ref)
            print("  · 对照参考包（仅报告，不影响判定）：参照 %d 个文件；其中"
                  " `gamedata/textures/**` %d 个按规则不收"
                  % (len(ref_files),
                     sum(1 for r in ref_files if r.startswith("gamedata/textures/"))))

    if PROBLEMS:
        print("LANGPACK-COLLECT: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("LANGPACK-COLLECT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
