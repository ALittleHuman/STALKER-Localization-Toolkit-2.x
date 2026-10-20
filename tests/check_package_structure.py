# -*- coding: utf-8 -*-
"""判据（决策 4 的 **A 半**）：发行包"分类计划"必须落成约定形态。

用户 2026-10-19/20 口径
----------------------
「不要让夹具去『模拟整个 package.ps1』，那会变成第二套打包逻辑。给 `package.ps1` 加一个
`-PlanOnly`：只读源清单，输出『每个文件落到哪』的 JSON，不复制、不写盘。然后用临时目录造一个
假舞台跑它，断言：根目录散文件 ≤ 12；`logs\\`、`user.ltx` 不在计划里；`python314.dll` 留在根；
`.pyd` 进 `python\\`；ffmpeg 进 `bin\\`；`version.txt` 有 `layout=bin+python`。
这条 step 叫 `package-structure`，与将来的 `package-real` 分开。
★ `package-structure` 绿**不代表包能发**；`package-real` 没跑，release 档记阻塞。」

本判据做的正是上面这几条，全部**通过 `-PlanOnly` 的真实输出**判定（不复制、不写盘）：
A 计划 JSON 能解析、`layout=bin+python`、`root_files ≤ 12`；
B `logs\\*` 与 `user.ltx` 在 `excluded` 里（运行期产物不进包）；
C `python314.dll` / `python3.dll` / `vcruntime140.dll` / `Qt6Core.dll` 留在根；
D `*.pyd` 与 `python3*.zip`/`_pth`/`python.exe`/`sqlite3.dll` 等进 `python\\`；
E `ffmpeg.exe` / `ffprobe.exe` / `download_ffmpeg.py` 进 `bin\\`；
F 静态锁：`package.ps1` 里那条 `layout=bin+python` 真的会写进 `version.txt`。

退出码：0=绿 / 1=红 / 2=缺前提（powershell 或 package.ps1 不在）。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "package.ps1"
POWERSHELL = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"

# 假舞台：三类必须留根 ＋ python\ 与 bin\ 的子件 ＋ 运行期产物
ROOT_KEEP = ["xrkit_app.exe", "python314.dll", "python3.dll", "vcruntime140.dll",
             "Qt6Core.dll", "README.txt", "version.txt"]
PYTHON_DIR = ["python314.zip", "python314._pth", "python.exe", "python.cat",
              "sqlite3.dll", "libssl-3.dll", "zzz.pyd"]
BIN_DIR = ["ffmpeg.exe", "ffprobe.exe", "download_ffmpeg.py"]
EXCLUDED = ["user.ltx", "logs/log_20260101_000000.txt"]

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def build_stage(root: Path) -> None:
    for name in ROOT_KEEP + PYTHON_DIR + BIN_DIR:
        (root / name).write_bytes(b"x")
    (root / "logs").mkdir(parents=True, exist_ok=True)
    (root / "user.ltx").write_bytes(b"x")
    (root / "logs" / "log_20260101_000000.txt").write_bytes(b"x")


def run_plan(stage: Path) -> tuple:
    if not POWERSHELL.is_file():
        return None, "缺 powershell.exe：%s" % POWERSHELL
    p = subprocess.run([str(POWERSHELL), "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", str(SCRIPT), "-PlanOnly", "-SourceRoot", str(stage)],
                       capture_output=True, timeout=600)
    out = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
    if p.returncode != 0:
        return None, "package.ps1 -PlanOnly 退出码 %d：%s" % (p.returncode, out[-400:])
    for line in out.splitlines():
        if line.startswith("PLAN {"):
            try:
                return json.loads(line[len("PLAN "):]), ""
            except json.JSONDecodeError as exc:
                return None, "PLAN JSON 解析失败：%s" % exc
    return None, "输出里没有 `PLAN {...}` 行：%s" % out[-300:]


def main() -> int:
    print("== 决策 4（A 半）：发行包分类计划（-PlanOnly，不动盘）==")
    if not SCRIPT.is_file():
        print("PKG-STRUCTURE skip 缺 %s" % SCRIPT)
        return 2

    src = SCRIPT.read_text(encoding="utf-8", errors="replace")
    want("layout=bin+python" in src, "F1 `package.ps1` 会往 version.txt 写 `layout=bin+python`")
    want("-PlanOnly" in src and "Get-PackageDest" in src,
         "F2 有 `-PlanOnly` 模式，且与真打包**共用** `Get-PackageDest`（不是第二套逻辑）")

    stage = Path(tempfile.mkdtemp(prefix="pkgstruct_"))
    try:
        build_stage(stage)
        plan, err = run_plan(stage)
        if plan is None:
            want(False, "A0 `-PlanOnly` 能跑出计划 JSON", err[:200])
            print("-" * 66)
            print("PKG-STRUCTURE fail %d 项不成立：" % len(PROBLEMS))
            for item in PROBLEMS:
                print("  · " + item)
            return 1

        entries = plan.get("entries", [])
        excluded = plan.get("excluded", [])
        dest_of = {Path(e["src"].replace("\\", "/")).name: e["dest"] for e in entries}
        exc_names = {Path(x["src"].replace("\\", "/")).name for x in excluded}

        want(plan.get("layout") == "bin+python", "A1 计划自报 `layout=bin+python`",
             str(plan.get("layout")))
        want(int(plan.get("root_files", 999)) <= 12, "A2 **根目录散文件 ≤ 12**",
             "实测 %s" % plan.get("root_files"))
        want(len(entries) > 0, "A3 计划里有条目", "%d 条" % len(entries))

        for name in ("user.ltx", "log_20260101_000000.txt"):
            want(name in exc_names, "B `%s` 不进包（运行期产物）" % name)

        for name in ("python314.dll", "python3.dll", "vcruntime140.dll", "Qt6Core.dll",
                     "xrkit_app.exe"):
            got = dest_of.get(name, "<缺失>")
            want(got == name, "C `%s` 留在**根**" % name, "dest=%s" % got)

        for name in ("zzz.pyd", "python314.zip", "python314._pth", "python.exe", "sqlite3.dll"):
            got = dest_of.get(name, "<缺失>")
            want(got.startswith("python/"), "D `%s` 进 `python/`" % name, "dest=%s" % got)

        for name in BIN_DIR:
            got = dest_of.get(name, "<缺失>")
            want(got == "bin/" + name, "E `%s` 进 `bin/`" % name, "dest=%s" % got)
    finally:
        shutil.rmtree(stage, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("PKG-STRUCTURE fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PKG-STRUCTURE ok %d 项（计划形态对：根 ≤12、三类留根、pyd→python\\\\、ffmpeg→bin\\\\、"
          "运行期产物不进包）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
