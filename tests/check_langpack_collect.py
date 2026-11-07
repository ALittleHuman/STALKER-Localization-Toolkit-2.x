# -*- coding: utf-8 -*-
"""语言包收敛判据（--langpack-collect）：**结构规则 + 参照包金标准**。

用户口径（2026-11-07/08）：
  ① 所有语言都收（语言名从路径扫出来，不钉死 rus）；
  ② 没有任何模组特异内容：保留清单不认 `gamedata/` / `ixr_addons/` / 地图；
     只按结构规则 —— 任意前缀下的 `configs/text/<lang>`、`configs/{fonts,localization}.ltx`、
     与某个 `configs` 同根的 `fonts/**`。

断言：
  · 参照包 B：默认输出 == B 去掉 `gamedata/textures/**`（逐字节）；多语言 eng 也要收；
  · 通用性：非 `gamedata/` 前缀（`mods/foo/configs/text/eng/...`）同样能收；
  · 噪声：scripts/anims/textures 永远不进。
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


def run_collect(src: Path, out: Path) -> tuple[int, str]:
    p = subprocess.run([str(CLI), "--langpack-collect", str(src), str(out)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 语言包收敛：结构规则 / 全语言 / 无模组特异 ==")
    if not CLI.is_file():
        print("LANGPACK-COLLECT skip 缺 CLI")
        return 2
    if not REF.is_dir():
        print("LANGPACK-COLLECT skip 缺参照包")
        return 2
    with tempfile.TemporaryDirectory(prefix="lpc_") as td:
        td = Path(td)
        src = td / "src"
        shutil.copytree(REF, src)
        (src / "gamedata" / "scripts").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "scripts" / "a.script").write_text("-- noise\n", encoding="utf-8")
        (src / "gamedata" / "anims").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "anims" / "x.anm").write_bytes(b"\x00noise")
        eng_dir = src / "gamedata" / "configs" / "text" / "eng"
        eng_dir.mkdir(parents=True, exist_ok=True)
        (eng_dir / "st_x.xml").write_text("<string_table></string_table>", encoding="utf-8")
        # 通用性夹具：非 gamedata 前缀
        generic = src / "mods" / "foo"
        (generic / "configs" / "text" / "eng").mkdir(parents=True)
        (generic / "configs" / "text" / "eng" / "g.xml").write_text(
            "<string_table></string_table>", encoding="utf-8")
        (generic / "configs" / "fonts.ltx").write_text("[fonts]\n", encoding="utf-8")
        (generic / "fonts" / "eng").mkdir(parents=True)
        (generic / "fonts" / "eng" / "f.ttf").write_bytes(b"ttf")
        (generic / "scripts").mkdir(parents=True)
        (generic / "scripts" / "s.script").write_text("-- noise\n", encoding="utf-8")

        ref = files(REF)
        expected = {r: p for r, p in ref.items() if not r.startswith("gamedata/textures/")}
        expected["gamedata/configs/text/eng/st_x.xml"] = eng_dir / "st_x.xml"
        expected["mods/foo/configs/text/eng/g.xml"] = generic / "configs/text/eng/g.xml"
        expected["mods/foo/configs/fonts.ltx"] = generic / "configs/fonts.ltx"
        expected["mods/foo/fonts/eng/f.ttf"] = generic / "fonts/eng/f.ttf"

        out = td / "out"
        rc, log = run_collect(src, out)
        if rc != 0:
            fail("collect 失败 rc=%d\n%s" % (rc, log[-300:]))
            print("LANGPACK-COLLECT: FAIL")
            return 1
        got = files(out)
        (ok if not (set(expected) - set(got)) else fail)(
            "缺文件 %d：%s" % (len(set(expected) - set(got)),
                             sorted(set(expected) - set(got))[:3]))
        (ok if not (set(got) - set(expected)) else fail)(
            "多收 %s" % sorted(set(got) - set(expected))[:3])
        changed = [r for r in expected if r in got and sha(expected[r]) != sha(got[r])]
        (ok if not changed else fail)("逐字节相同（不同 %d）" % len(changed))
        (ok if not any(r.startswith("gamedata/textures/") for r in got) else fail)(
            "不含地图纹理（textures 不是语言包通用件）")
        (ok if not any("/scripts/" in r for r in got) else fail)("不含 scripts")

    if PROBLEMS:
        print("LANGPACK-COLLECT: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("LANGPACK-COLLECT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
