#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""包内平台插件 + "从产物里真跑一次自检"（用户 2026-10-07 裁决）。

事实（逐条已复核）：
  · `windeployqt` 只会铺 `qwindows.dll`（它按 app 的实际调用挑），而 `xrkit_app.exe --smoke` /
    `--layout-report` 走**离屏**；修前 `scripts/package.ps1` 的处置是"跑判据前**临时**拷
    `qoffscreen.dll` 进去、跑完在 `finally` 里**删掉**（注释自述"保持包精简"）
    ⇒ **产物本身不可自检**：拿包内 exe 直接跑 `--smoke` 会 fail-fast
    `0xC0000409`（WER: BEX64 / 故障模块 Qt6Core.dll / P7=0x267a8）。实测 ALPHA.57/58/59
    是**同一个签名**（包内 exe 与 build 目录 exe 的 SHA256 完全相同、Qt/Qt插件逐字节相同，
    只差那个插件 ⇒ 补上它后同一份包布局 `SHELL-SMOKE ok checks=713 failed=0`）。
  · 同族假判据：结论行原来只 grep `SHELL-SMOKE ok`，而 smoke 的收尾行**恒**以 `ok` 开头
    （`SHELL-SMOKE ok checks=741 failed=1` 也是 "ok"，本批实测见过）⇒ 有失败也读成通过。
  · 用户 2026-10-07 裁决：「部署舞台必须同时带 `qwindows.dll` 与 `qoffscreen.dll`」，
    保留/删除由"判据＝从产物里真跑一次"定 ⇒ **保留**（109 KB）。

判据（三条、都能判红）：
  ① 静态：`scripts/package.ps1` 必须**把 qoffscreen.dll 拷进舞台**、**不许**再有"跑完删掉它"
     的语句、必须断言产物里两份平台插件都在、且结论行判据必须是
     `SHELL-SMOKE ok checks=N failed=0`（不是只 grep "ok"）；
  ② 产物：最新的 `dist\\<主版本>\\<标记>\\<序号>` 里 `platforms\\` 必须同时有
     qwindows.dll + qoffscreen.dll；
  ③ 真跑：在**那个产物目录里**跑 `xrkit_app.exe --smoke`（`QT_QPA_PLATFORM=offscreen`，
     子进程环境，不设持久变量）⇒ 退出码 0 且结论行 `failed=0`。
  ④ 自证：喂"删掉 qoffscreen 的旧 package.ps1""只 grep ok 的旧判据"必须报红。

为什么 ②③ 允许"本次没有产物"：`/dist/` 是 gitignore 的**运行期产物**，干净检出/首次门禁时
本来就没有包。这条**不是**"没人管"—— 同一套判据已经写进 `package.ps1`，**每次打包都会真跑**
（跑不过就 throw、包根本不会落地）。没有产物时本脚本打一条**台账**（可见、release 不阻断），
静态面①仍然照常判红。

退出码：0 = 绿；1 = 有判据不成立；2 = 本次没有产物可跑（调用方记台账）。
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE_PS1 = ROOT / "scripts" / "package.ps1"

FAILURES: list[str] = []
NOTES: list[str] = []


def say(line: str) -> None:
    print(line, flush=True)


def check(ok: bool, label: str, detail: str = "") -> None:
    if ok:
        say("  \u2713 %s" % label)
    else:
        FAILURES.append(label)
        say("  \u2717 %s%s" % (label, ("\n      " + detail) if detail else ""))


def brief(text: str, lines: int = 3) -> str:
    rows = [row for row in text.splitlines() if row.strip()]
    return "\n      ".join(rows[:lines])


# ── ① 静态面（纯文本分析器，可自证）────────────────────────────────────────

def package_ps1_problems(text: str) -> list[str]:
    """返回问题清单（空 = 全绿）。★ 下面会被喂反例自证。"""
    problems: list[str] = []
    lines = text.splitlines()

    if "qoffscreen" not in text:
        problems.append("package.ps1 里根本没有 `qoffscreen`：部署舞台不会带离屏平台插件，"
                        "`--smoke` 会让产物自身 fail-fast（0xC0000409）")
        return problems

    for index, line in enumerate(lines, 1):
        if "Remove-Item" in line and "qoffscreen" in line:
            problems.append("第 %d 行仍在删除 qoffscreen.dll（用户 2026-10-07 裁决：随包携带）：%s"
                            % (index, line.strip()))

    copied = [line for line in lines
              if "Copy-Item" in line and ("qoffscreen" in line or "$off" in line)]
    if not copied:
        problems.append("package.ps1 里没有把 qoffscreen.dll 拷进舞台的 `Copy-Item` 语句")

    if "-not (Test-Path (Join-Path $stagePlat" not in text:
        problems.append("package.ps1 没有断言**产物里**两份平台插件都在"
                        "（`Test-Path (Join-Path $stagePlat …)`）")
    for plat in ("qwindows.dll", "qoffscreen.dll"):
        if ("'%s'" % plat) not in text:
            problems.append("package.ps1 的平台插件断言清单里缺 %s" % plat)

    if not re.search(r"SHELL-SMOKE ok checks=\\d\+ failed=0|SHELL-SMOKE ok checks=\(\\d\+\) failed=0",
                     text):
        problems.append("package.ps1 的结论行判据没有收窄到 `failed=0`（只 grep `SHELL-SMOKE ok` "
                        "会把 `SHELL-SMOKE ok checks=N failed=1` 读成通过）")

    # ★ 契约声明：包自己在 version.txt 里声明"带哪些平台插件"。门禁据此对**这个序号**
    #   执行产物面判据；声明本身也必须由 package.ps1 产出（删掉它 ⇒ 产物面判据永远不跑 = 假绿）。
    if "platform_plugins=qwindows,qoffscreen" not in text:
        problems.append("package.ps1 没有把契约声明 `platform_plugins=qwindows,qoffscreen` 写进 "
                        "version.txt —— 门禁的产物面判据就没有执行依据（=永远不跑）")
    return problems


def run_static() -> None:
    if not PACKAGE_PS1.is_file():
        check(False, "脚本存在：scripts/package.ps1")
        return
    text = PACKAGE_PS1.read_text(encoding="utf-8")
    problems = package_ps1_problems(text)
    check(not problems, "package.ps1：随包携带 qoffscreen + 产物侧断言 + 结论行要求 failed=0",
          "\n      ".join(problems))

    # ★ 自证：修前那种"跑完删掉"的写法必须被判红
    old_shape = ("$off = Join-Path $qt 'plugins\\platforms\\qoffscreen.dll'\n"
                 "if (Test-Path $off) { Copy-Item -LiteralPath $off -Destination $stagePlat }\n"
                 "Remove-Item -Force (Join-Path $stagePlat 'qoffscreen.dll')\n"
                 "$line = Select-String -Path $log -Pattern 'SHELL-SMOKE ok'\n")
    bad = package_ps1_problems(old_shape)
    check(bool(bad), "自证：修前那种「跑完删掉 qoffscreen + 只 grep ok」的写法必须被判红",
          str(bad))
    check(bool(package_ps1_problems("# 与 qoffscreen 无关的一份脚本\nWrite-Host 'hi'\n")),
          "自证：完全不提 qoffscreen 的脚本必须被判红")


# ── ②③ 产物面 ─────────────────────────────────────────────────────────────

def newest_package() -> Path | None:
    base = ROOT / "dist"
    if not base.is_dir():
        return None
    best: tuple[int, Path] | None = None
    for index_dir in base.glob("*/*/*"):
        if not index_dir.is_dir() or not index_dir.name.isdigit():
            continue
        score = int(index_dir.name)
        if best is None or score > best[0]:
            best = (score, index_dir)
    return best[1] if best else None


SMOKE_RE = re.compile(r"SHELL-SMOKE ok checks=(\d+) failed=0")


def run_artifact() -> None:
    pkg = newest_package()
    if pkg is None:
        NOTES.append("包内平台插件/从产物里真跑自检（%s 下没有任何包；同一判据已写进 "
                     "package.ps1，每次打包都会真跑）" % (ROOT / "dist").relative_to(ROOT))
        say("  LEDGER 本次没有 dist 产物 ⇒ ②③ 未执行（判据本身写在 package.ps1 里，"
            "每次打包都会跑）")
        return
    rel = pkg.relative_to(ROOT)
    say("  产物：%s" % rel.as_posix())
    version_txt = pkg / "version.txt"
    declared = ""
    if version_txt.is_file():
        # ★ 用 `utf-8-sig`：version.txt 由 PowerShell 5.1 的 `Set-Content -Encoding UTF8` 写
        #   ⇒ **带 BOM**（不去掉的话第一行的键会变成 "\ufeffversion"；本批实测踩到过）。
        for row in version_txt.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            if row.strip().startswith("platform_plugins="):
                declared = row.strip().split("=", 1)[1].strip()
    if "qoffscreen" not in declared:
        # 修前打的包（ALPHA.57/58/59）：没有契约声明 ⇒ 只能记台账（它们确实缺 qoffscreen）。
        # 一旦包声明了（`platform_plugins=` 含 qoffscreen）就必须**成立**，没有第二条路。
        NOTES.append("序号 %s 的包没有 `platform_plugins=` 契约声明（修前打的）⇒ 产物面判据"
                     "留给下一次打包" % pkg.name)
        say("  LEDGER %s/version.txt 没有 `platform_plugins=` 声明（修前打的包）⇒ ②③ 未执行"
            % rel.as_posix())
        return

    platforms = pkg / "platforms"
    for name in ("qwindows.dll", "qoffscreen.dll"):
        check((platforms / name).is_file(),
              "产物 %s：platforms\\%s 在包里（version.txt 已声明）" % (rel.as_posix(), name),
              "目录实测：%s" % (", ".join(sorted(p.name for p in platforms.glob('*'))) or "(空/不存在)"))

    exe = pkg / "xrkit_app.exe"
    if not exe.is_file():
        check(False, "产物 %s：xrkit_app.exe 在包里" % rel.as_posix())
        return
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"   # ★ 只给子进程，不设持久变量
    env.setdefault("PYTHONUTF8", "1")
    try:
        proc = subprocess.run([str(exe), "--smoke"], cwd=str(pkg), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/IM", "xrkit_app.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        check(False, "产物 %s：`--smoke` 在 600 s 内跑完" % rel.as_posix())
        return
    except OSError as exc:
        # 不是可执行文件 / 架构不兼容 / 被杀软拦下 ⇒ 明确报错，别让 traceback 变成"判据不通过"
        check(False, "产物 %s：能起得来 `--smoke`" % rel.as_posix(), str(exc))
        return
    out = proc.stdout.decode("utf-8", errors="replace")
    line = SMOKE_RE.search(out)
    check(line is not None and proc.returncode == 0,
          "产物 %s：从**产物目录**跑 `--smoke` ⇒ `SHELL-SMOKE ok … failed=0` 且退出码 0"
          % rel.as_posix(),
          "rc=%d\n      %s" % (proc.returncode,
                               brief("\n".join(l for l in out.splitlines()
                                               if "SHELL-SMOKE" in l) or out)))
    if line is not None:
        say("  原始行：%s（exit=%d，cwd=%s）"
            % (line.group(0)[:120], proc.returncode, rel.as_posix()))


def main() -> int:
    say("PKG-PLATFORM 包内平台插件 + 从产物里真跑自检（qwindows + qoffscreen）")
    run_static()
    run_artifact()
    if FAILURES:
        say("PKG-PLATFORM: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("PKG-PLATFORM: PASS%s" % ("（②③ 未执行，见台账）" if NOTES else ""))
    return 2 if NOTES else 0


if __name__ == "__main__":
    sys.exit(main())
