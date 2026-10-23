# -*- coding: utf-8 -*-
"""P5 判决性实验（思路 3）：跑 N 次 `xrkit_app.exe --smoke`，读 trace 里两条新事件。

  · `place_null|... root_stale=0/1`  —— 空父静默返回发生了多少次（P5 那条"空页无痕"的出口）
  · `resolve|... root_in_reg= root_stale=` —— 根地址是否被登记、是否已死（地址复用假设）

判定：
  · place_null 全 0 且 root_stale 全 0  ⇒ "地址复用"假设**被证伪**，停止追它。
  · 任一 run 出现 root_stale=1          ⇒ 假设成立（修法：把容器登记成句柄）。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit_2.x")
EXE = ROOT / "build" / "msvc-shell" / "src" / "shell" / "xrkit_app.exe"
OUT = ROOT / "build" / "logs" / "p5_probe"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 12
TIMEOUT = 600

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def run_once(i: int):
    tmp = Path(tempfile.mkdtemp(prefix="p5tmp_", dir=str(ROOT / "build")))
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["XRKIT_PAGE_TRACE"] = "1"
    env["TEMP"] = str(tmp)
    env["TMP"] = str(tmp)
    env["PYTHONUTF8"] = "1"
    started = time.time()
    try:
        proc = subprocess.run([str(EXE), "--smoke"], cwd=str(EXE.parent), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=TIMEOUT)
        rc = proc.returncode
        out = proc.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/IM", "xrkit_app.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rc, out = None, "[超时]"
    seconds = time.time() - started

    shell_trace = tmp / "page_trace_shell.txt"
    engine_trace = tmp / "page_trace_engine.txt"
    text = ""
    if shell_trace.is_file():
        text = shell_trace.read_text(encoding="utf-8", errors="replace")
        shutil.copy2(shell_trace, OUT / ("run%02d_shell.txt" % i))
    if engine_trace.is_file():
        shutil.copy2(engine_trace, OUT / ("run%02d_engine.txt" % i))

    fails = [l.strip() for l in out.splitlines() if "SHELL-SMOKE fail" in l]
    ok_line = next((l.strip() for l in out.splitlines() if "SHELL-SMOKE ok" in l), "")
    place_null = [l for l in text.splitlines() if l.startswith("shell|place_null")]
    stale = [l for l in text.splitlines() if "root_stale=1" in l]
    in_reg = [l for l in text.splitlines() if "root_in_reg=1" in l]
    p5_fail = [f for f in fails if "P5" in f]
    rec = {
        "run": i, "rc": rc, "seconds": round(seconds, 1),
        "ok": ok_line, "fails": len(fails), "p5_fails": len(p5_fail),
        "all_fail_lines": fails[:12],
        "place_null": len(place_null), "root_stale1": len(stale), "root_in_reg1": len(in_reg),
        "p5_fail_lines": p5_fail[:4],
        "all_fail_lines": fails[:12],
        "place_null_sample": place_null[:3],
        "stale_sample": stale[:3],
        "tail": [l for l in out.splitlines() if "SHELL-SMOKE note" in l][-3:],
    }
    shutil.rmtree(tmp, ignore_errors=True)
    return rec


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    if not EXE.is_file():
        print("找不到 exe：%s" % EXE)
        return 1
    records = []
    print("P5 判决性实验：%d 次（每次私有 TEMP + XRKIT_PAGE_TRACE=1）" % N, flush=True)
    for i in range(1, N + 1):
        rec = run_once(i)
        records.append(rec)
        print("  run%02d rc=%s %.0fs fails=%d p5=%d place_null=%d root_stale1=%d in_reg1=%d"
              % (i, rec["rc"], rec["seconds"], rec["fails"], rec["p5_fails"],
                 rec["place_null"], rec["root_stale1"], rec["root_in_reg1"]), flush=True)
        (OUT / "summary.json").write_text(json.dumps(records, ensure_ascii=False, indent=1),
                                          encoding="utf-8")
    total = lambda k: sum(r[k] for r in records)
    print()
    print("== 汇总 ==")
    print("  runs=%d 退出码非 0 的=%d  总 fails=%d  P5 fails=%d"
          % (len(records), sum(1 for r in records if r["rc"] != 0), total("fails"), total("p5_fails")))
    print("  place_null 总计=%d   root_stale=1 总计=%d   root_in_reg=1 总计=%d"
          % (total("place_null"), total("root_stale1"), total("root_in_reg1")))
    print()
    if total("root_stale1") > 0:
        print("⇒ 结论：地址复用**成立**（根地址被已销毁的门面控件占用过）⇒ 修法：把容器登记成句柄")
    elif total("place_null") > 0:
        print("⇒ 结论：空父静默返回**确实发生**，但 root_stale=0 ⇒ 成因不是地址复用（另查 root 为何为空）")
    else:
        print("⇒ 结论：本轮未观测到 place_null / root_stale ⇒ '地址复用'假设**被证伪**（或本轮 P5 全绿）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
