# -*- coding: utf-8 -*-
"""版本身份四路一致 + 拒重打 + 半成品检测（HA/DSH 2026-11-04）。

BETA.103 的缝：`current` 前进而 `version.h` 同值 ⇒ 写回判"没改变字节" ⇒ 回滚判"没写过"
⇒ 序号被吃掉 ⇒ 靠"直接打下一号"绕过 ⇒ 留下半成品目录。

本判据钉死四路身份：
  ① version.json 的 current.index
  ② version.h 的 kBuildCount
  ③ 构建出的 exe 自报 build=（`--exe-version`）
  ④ 包内 version.txt 的序号
另加：同号重打必须拒绝且三件套一个字节不动；dist 下不许有"缺 version.txt / 缺 exe"的半成品。
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
EXE = ROOT / "build" / "msvc-shell" / "src" / "shell" / "xrkit_app.exe"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    print("== 版本身份四路一致 / 拒重打 / 半成品 ==")
    if not CLI.is_file() or not EXE.is_file():
        print("VERSION-CONSISTENCY skip 缺 CLI 或 exe")
        return 2

    store = json.loads((ROOT / "version.json").read_text(encoding="utf-8"))
    cur = store["current"]
    idx = int(cur["index"])
    pre = cur["prerelease"]
    ver = cur["version"]

    header = (ROOT / "include" / "xrkit" / "core" / "version.h").read_text(encoding="utf-8")
    m = re.search(r"kBuildCount\s*=\s*(\d+)", header)
    h_idx = int(m.group(1)) if m else -1
    (ok if h_idx == idx else fail)("version.json current=%d == version.h kBuildCount=%d" % (idx, h_idx))

    p = subprocess.run([str(CLI), "--exe-version", str(EXE)], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")
    m2 = re.search(r"numeric=\d+\.\d+\.\d+\.(\d+)", out) or re.search(r"\.(\d+)\b", out)
    exe_idx = int(m2.group(1)) if m2 else -1
    (ok if exe_idx == idx else fail)("exe 自报 build=%d == current=%d" % (exe_idx, idx))

    pkg_dir = ROOT / "dist" / ver / pre / str(idx)
    vt = pkg_dir / "version.txt"
    if vt.is_file():
        text = vt.read_text(encoding="utf-8", errors="replace")
        m3 = re.search(r"(?:index|build)=(\d+)", text)
        pkg_idx = int(m3.group(1)) if m3 else -1
        (ok if pkg_idx == idx else fail)("包内 version.txt 序号=%d == current=%d" % (pkg_idx, idx))
    else:
        fail("当前序号的包缺 version.txt：%s" % pkg_dir)

    before = (sha(ROOT / "version.json"), sha(ROOT / "include" / "xrkit" / "core" / "version.h"))
    p2 = subprocess.run([str(CLI), "--build-package", "version.json", ver, pre, str(idx)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out2 = p2.stdout.decode("utf-8", "replace")
    after = (sha(ROOT / "version.json"), sha(ROOT / "include" / "xrkit" / "core" / "version.h"))
    refused = p2.returncode != 0 and "拒绝重打" in out2
    (ok if refused else fail)("同号重打被拒（rc=%d）" % p2.returncode)
    (ok if before == after else fail)("拒重打时三件套一个字节都没动")

    bad = []
    for vdir in (ROOT / "dist").glob("*/*/*"):
        if not vdir.is_dir():
            continue
        if "FAIL" in vdir.parts:
            continue
        if not (vdir / "version.txt").is_file() or not (vdir / "xrkit_app.exe").is_file():
            bad.append(vdir.relative_to(ROOT).as_posix())
    (ok if not bad else fail)("dist 下没有半成品（缺 version.txt / exe）：%s" % (bad or "无"))

    if PROBLEMS:
        print("VERSION-CONSISTENCY: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("VERSION-CONSISTENCY: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
