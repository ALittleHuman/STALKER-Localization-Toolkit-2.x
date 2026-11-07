# -*- coding: utf-8 -*-
"""语言包收敛判据（--langpack-collect）：以**真实参照包**为金标准。

口径（2026-11-07 用户两条）：
  ① 所有语言都收：`configs/text/<lang>` 有几种收几种（不再钉死 rus）；
  ② 地图纹理不是语言包通用件：默认**不收**，`--include-map` 才收。

断言：
  · 默认：输出 == 参照包去掉 `gamedata/textures/**`，且额外塞的 text/eng **要被收**；
  · `--include-map`：输出 == 参照包全部（231）+ text/eng；
  · scripts/anims 噪声永远不进。
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


def run_collect(src: Path, out: Path, *extra) -> tuple[int, str]:
    p = subprocess.run([str(CLI), "--langpack-collect", str(src), str(out), *extra],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 语言包收敛：全语言 / 默认不收地图 ==")
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
        # 噪声：scripts/anims + 另一种语言
        (src / "gamedata" / "scripts").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "scripts" / "a.script").write_text("-- noise\n", encoding="utf-8")
        (src / "gamedata" / "anims").mkdir(parents=True, exist_ok=True)
        (src / "gamedata" / "anims" / "x.anm").write_bytes(b"\x00noise")
        eng_dir = src / "gamedata" / "configs" / "text" / "eng"
        eng_dir.mkdir(parents=True, exist_ok=True)
        (eng_dir / "st_x.xml").write_text("<string_table></string_table>", encoding="utf-8")

        ref = files(REF)
        expected_default = {r: p for r, p in ref.items()
                            if not r.startswith("gamedata/textures/")}
        expected_default["gamedata/configs/text/eng/st_x.xml"] = eng_dir / "st_x.xml"

        out = td / "out"
        rc, log = run_collect(src, out)
        if rc != 0:
            fail("默认 collect 失败 rc=%d\n%s" % (rc, log[-300:]))
        else:
            got = files(out)
            (ok if not (set(expected_default) - set(got)) else fail)(
                "默认：参照包（去地图）+ 多语言都收齐（缺 %d）"
                % len(set(expected_default) - set(got)))
            (ok if not (set(got) - set(expected_default)) else fail)(
                "默认：没有多收（多 %s）" % sorted(set(got) - set(expected_default))[:3])
            changed = [r for r in expected_default if r in got and sha(expected_default[r]) != sha(got[r])]
            (ok if not changed else fail)("默认：逐字节相同（不同 %d）" % len(changed))
            (ok if not any(r.startswith("gamedata/textures/") for r in got) else fail)(
                "默认：**不含地图纹理**")

        out2 = td / "out_map"
        rc2, log2 = run_collect(src, out2, "--include-map")
        if rc2 != 0:
            fail("--include-map 失败 rc=%d\n%s" % (rc2, log2[-300:]))
        else:
            got2 = files(out2)
            expected_all = dict(ref)
            expected_all["gamedata/configs/text/eng/st_x.xml"] = eng_dir / "st_x.xml"
            (ok if set(got2) == set(expected_all) else fail)(
                "--include-map：与参照包全部一致（缺 %d / 多 %d）"
                % (len(set(expected_all) - set(got2)), len(set(got2) - set(expected_all))))

    if PROBLEMS:
        print("LANGPACK-COLLECT: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("LANGPACK-COLLECT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
