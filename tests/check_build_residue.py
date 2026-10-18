# -*- coding: utf-8 -*-
"""判据 B-6：**构建残留**不许进仓库；机器上的残留要能一眼看见并能清（审计 B-6）。

事实（审计原话 + 我的实测）
--------------------------
审计点名：`dist/FAIL/`、`build/` 下大量临时脚本、`__pycache__` 留存。
实测（2026-10-18，`ec17370`）：`dist/FAIL` **39** 个目录、`build/logs/_*` **943** 个文件、
`build/commit_msg*.txt` **13** 个 —— 但 **`git ls-files build dist` = 0**：
它们是**未跟踪的运行期产物**（`.gitignore` 的 `/build/`、`/dist/` 挡住了）。

⇒ 判据**分两层**（这是本条的要害，别写成"开发机 `build/` 必须干净"）：

* **A 仓库层（可判红）**：仓库里**不许跟踪**任何 `build/`、`dist/` 下的文件；
  `.gitignore` 必须**真的挡住**这两处（规则在、`git check-ignore` 也认）。
  这一层是"仓库卫生"，与开发机状态无关 —— 有人 `git add -f` 一个残留文件就红。
* **B 机器层（只报数，不判红）**：`dist/FAIL/`、`build/logs/_*`、`commit_msg*.txt` 的**计数**
  打印出来 ＋ 指路 `scripts/clean.ps1`。**不判红**的理由：批次进行中必然有残留
  （取证脚本/日志就在里面），判红等于逼人删掉证据 —— 那会把"零静默"变成"零材料"。
* **C 清理脚本的锁**：`scripts/clean.ps1` 在、**默认只列不删**（`-Apply` 才删）、
  白名单只三处、且**不碰** `dist\\<版本>`；`.ps1` 必须有 UTF-8 BOM（PS 5.1 读中文的前提）。

退出码：0=绿 / 1=红 / 2=缺输入（git 不可用或脚本缺失）。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLEAN = ROOT / "scripts" / "clean.ps1"
GITIGNORE = ROOT / ".gitignore"

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


def git(args: list, timeout: int = 300) -> tuple:
    proc = subprocess.run(["git", "-C", str(ROOT)] + args, capture_output=True, timeout=timeout)
    return proc.returncode, (proc.stdout or b"").decode("utf-8", "replace")


def main() -> int:
    print("== B-6：构建残留 —— 仓库层判红、机器层报数、清理脚本可核 ==")
    if not GITIGNORE.is_file():
        print("BUILD-RESIDUE skip 缺 .gitignore")
        return 2
    rc, _ = git(["rev-parse", "--is-inside-work-tree"])
    if rc != 0:
        print("BUILD-RESIDUE skip 这个目录不是 git 工作树")
        return 2

    # ── A 仓库层（可判红）──────────────────────────────────────────
    rc, out = git(["ls-files", "build", "dist"])
    tracked = [l for l in out.splitlines() if l.strip()]
    want(rc == 0 and not tracked, "A1 仓库里**没有**跟踪 `build/`、`dist/` 下的文件",
         ("跟了 %d 个：%s" % (len(tracked), tracked[:3])) if tracked else "实测 0 个")

    ign = GITIGNORE.read_text(encoding="utf-8", errors="replace")
    pats = [l.strip() for l in ign.splitlines() if l.strip() and not l.strip().startswith("#")]
    has_build = any(re.fullmatch(r"/?build/?|/?build/\*\*|\*\*/build/", p) for p in pats)
    has_dist = any(re.fullmatch(r"/?dist/?|/?dist/\*\*|\*\*/dist/", p) for p in pats)
    want(has_build, "A2 `.gitignore` 里有挡住 `build/` 的规则")
    want(has_dist, "A2b `.gitignore` 里有挡住 `dist/` 的规则")
    # ★ 逐个路径问（`git check-ignore -q` 一次给两个路径时，退出码语义与"全都被忽略"不同，
    #   本判据要的是"两处都真的挡得住" ⇒ 分开问，谁没被挡就点名谁）。
    rc_build, _ = git(["check-ignore", "-q", "build/probe.bin"])
    rc_dist, _ = git(["check-ignore", "-q", "dist/probe.bin"])
    want(rc_build == 0 and rc_dist == 0,
         "A3 `git check-ignore` 也认（不只是写了规则、实际挡不住）",
         "build=%s dist=%s（0=挡住）" % (rc_build, rc_dist))

    # ── B 机器层（只报数）─────────────────────────────────────────
    def count_items(path: Path, pattern: str | None, want_dir: bool) -> int:
        if not path.is_dir():
            return 0
        if want_dir:
            return len([p for p in path.iterdir() if p.is_dir()])
        items = [p for p in path.glob(pattern or "*") if p.is_file()]
        return len(items)

    n_fail = count_items(ROOT / "dist" / "FAIL", None, True)
    n_scrap = count_items(ROOT / "build" / "logs", "_*", False)
    n_msg = count_items(ROOT / "build", "commit_msg*.txt", False)
    print("   \u00b7 机器层现状（**只报数**，不判红）：dist/FAIL %d 个目录 · "
          "build/logs/_* %d 个文件 · commit_msg*.txt %d 个" % (n_fail, n_scrap, n_msg))
    print("     → 要清就 `pwsh -File scripts\\clean.ps1`（默认只列）／加 `-Apply` 才删；"
          "先确认批次报告要引用的取证材料已复制到交接区")

    # ── C 清理脚本的锁 ───────────────────────────────────────────
    if not CLEAN.is_file():
        want(False, "C1 `scripts/clean.ps1` 在（残留要有地方清）")
    else:
        raw = CLEAN.read_bytes()
        has_bom = len(raw) >= 3 and raw[0] == 0xEF and raw[1] == 0xBB and raw[2] == 0xBF
        text = raw.decode("utf-8-sig", errors="replace")
        want(has_bom, "C0 `.ps1` 带 UTF-8 BOM（PS 5.1 按 GBK 读中文会读坏）")
        want("[switch]$Apply" in text, "C1 默认**只列不删**（`-Apply` 才真删）")
        for needle, label in (("dist\\FAIL", "dist\\FAIL"),
                              ("'_*'", "build\\logs\\_*"),
                              ("commit_msg*.txt", "commit_msg*.txt")):
            want(needle in text, "C2 白名单里有 `%s`" % label)
        want("没有碰 dist" in text, "C3 脚本自己写明**不碰** `dist\\<版本>` 正式产物")

    print("-" * 66)
    if PROBLEMS:
        print("BUILD-RESIDUE fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BUILD-RESIDUE ok %d 项（仓库里零跟踪残留、.gitignore 真挡得住；机器层只报数；"
          "清理脚本可核）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
