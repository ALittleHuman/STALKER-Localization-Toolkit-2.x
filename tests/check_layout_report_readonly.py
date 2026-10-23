# -*- coding: utf-8 -*-
"""**诊断模式不许改仓库状态**：`xrkit_build_gui --layout-report` 前后，版本三件套的 sha256 必须不变。

为什么要有它（2026-10-23 实测抓到的真凶）
----------------------------------------
`--layout-report` 本来是**纯诊断**（离屏构造窗口 → 量排版 → 打结论）。但 `BuildWindow` 构造尾部
调 `refresh_derived()` → `sync_store_and_read_next()` → `xrkit_cli --version-store set`
⇒ **把 `version.json` 的 `current` 覆盖成下拉框选中项**。实测：

    before 8a94c36b140e66fc
    --layout-report  →  36b1824f494b31f        （git diff：BETA/1 → ALPHA/258）
    --smoke          →  36b1824f494b31f

门禁里唯一跑 `--layout-report` 的步骤是 `shell-smoke` ⇒ 这就是那条
"`shell-smoke` 改了 version.json"（P17-133）的**唯一写入者**。
而且修前它"绿"是靠**写盘掩盖了不一致**（界面按 ALPHA 算 next、CLI 按 current 算 next）。

修法（`tools/build_gui/main.cpp`）：① 诊断模式 `read_only`（只算不写）；② prerelease 默认选 `current`。
**本判据锁的是"以后不许再有人在诊断路径上写盘"** —— 比只修一次值。

判据
----
A1 三件套（`version.json` / `include/xrkit/core/version.h` / `assets/xrkit_winres.rc`）
   在 `--layout-report` 前后 **sha256 逐个不变**。
A2 同一个可执行文件**连跑两次**也不变（防"第一次写、第二次不写"这种半修）。
A3 反例自证：对**临时副本**做一次"写盘"，本脚本的比对逻辑必须报"变了"（否则它恒绿）。

退出码：0=绿 / 1=红（诊断模式改了状态） / 2=缺前提（exe 没编出来）。
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUI = ROOT / "build" / "msvc-shell" / "src" / "shell" / "xrkit_build_gui.exe"

# 与门禁 `run_ci.py::_VERSION_IDENTITY` 同一份清单（同进同出）
WATCH = (
    ROOT / "version.json",
    ROOT / "include" / "xrkit" / "core" / "version.h",
    ROOT / "assets" / "xrkit_winres.rc",
)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot() -> dict:
    return {p: (sha(p) if p.is_file() else "missing") for p in WATCH}


def diff(before: dict, after: dict) -> list:
    return [p.name for p in WATCH if before.get(p) != after.get(p)]


def run_layout_report() -> tuple:
    tmp = Path(tempfile.mkdtemp(prefix="lrro_", dir=str(ROOT / "build")))
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"   # 诊断模式本来就按离屏跑
    env["TEMP"] = str(tmp)
    env["TMP"] = str(tmp)
    try:
        proc = subprocess.run([str(GUI), "--layout-report"], cwd=str(ROOT), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        return proc.returncode, proc.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return 124, "[超时]"
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def selfcheck_detects_write() -> bool:
    """反例自证：**临时副本**被写一个字节，`diff()` 必须报出来（否则本判据恒绿）。"""
    tmp = Path(tempfile.mkdtemp(prefix="lrro_sc_", dir=str(ROOT / "build")))
    try:
        a, b = tmp / "a.bin", tmp / "b.bin"
        a.write_bytes(b"x")
        b.write_bytes(b"x")
        before = {a: sha(a), b: sha(b)}
        b.write_bytes(b"xy")
        after = {a: sha(a), b: sha(b)}
        changed = [p.name for p in (a, b) if before.get(p) != after.get(p)]
        return changed == ["b.bin"]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    print("== 诊断模式不许改仓库状态（--layout-report 前后三件套 sha256 不变）==")
    if not GUI.is_file():
        print("LAYOUT-READONLY skip 缺 %s（先跑 shell-smoke 步把 GUI 编出来）" % GUI)
        return 2
    if not selfcheck_detects_write():
        print("LAYOUT-READONLY fail 反例自证不通过（本判据自己不可信）")
        return 1

    before = snapshot()
    rc1, out1 = run_layout_report()
    after1 = snapshot()
    changed1 = diff(before, after1)
    layout_ok = any(line.strip().startswith("LAYOUT PASS") for line in out1.splitlines())
    print("   第一次：rc=%d layout=%s 改动=%s"
          % (rc1, "PASS" if layout_ok else "未通过", "、".join(changed1) if changed1 else "无"))

    # 第二次（防"第一次写、第二次不写"）
    rc2, _ = run_layout_report()
    after2 = snapshot()
    changed2 = diff(after1, after2)
    print("   第二次：rc=%d 改动=%s"
          % (rc2, "、".join(changed2) if changed2 else "无"))

    problems = []
    if changed1:
        problems.append("`--layout-report` 改了：%s" % "、".join(changed1))
    if changed2:
        problems.append("`--layout-report` **第二次**又改了：%s（半修？）" % "、".join(changed2))
    if not layout_ok:
        problems.append("`--layout-report` 自己没打 `LAYOUT PASS`（诊断本身不成立，判据没有意义）")

    if problems:
        for item in problems:
            print("   \u2717 %s" % item)
        print("LAYOUT-READONLY fail %d 项不成立" % len(problems))
        return 1
    print("   \u2713 两次运行前后三件套 sha256 全部不变（诊断模式是只读的）")
    print("LAYOUT-READONLY ok 诊断不写盘（%s）"
          % "、".join(p.name for p in WATCH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
