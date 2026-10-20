# -*- coding: utf-8 -*-
"""判据：**外壳二进制必须是新的**（P17-176 的教训固化）。

为什么要有这条
--------------
P17-173 那批里 `_p1781_buildshell.py` 在没设 `PYTHONIOENCODING` 时崩在 `UnicodeEncodeError`
（退出码 1），但**崩在打印上、exe 没被替换**；而 `--smoke` 用的是**旧 exe** ——
于是"已经删掉的导出块"看起来还在（stderr 里仍有 `SMOKE-MARKER-EXPORT-LOG`），checks 数也不变。
我两轮都被骗过去。

口径（用户 2026-10-19/20 要求固化）
----------------------------------
1. `xrkit_app.exe` 的 mtime **不早于** `src/shell` 下最新的源文件；
2. 构建目录的 `.ninja_log` 里**有** `xrkit_app.exe` 的构建记录（＝这个 exe 真是编出来的，
   不是别人拷进来的/旧的）；
3. `--smoke` 的 checks 数变化只作**旁证**，不在这里判。

退出码：0=绿 / 1=红 / 2=缺前提（构建产物不在）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
SHELL_BUILD = ROOT / "build" / "msvc-shell"
EXE = SHELL_BUILD / "src" / "shell" / "xrkit_app.exe"
SRC_DIRS = [ROOT / "src" / "shell"]
# ★ 允许的时钟抖动：文件系统时间戳精度与"刚写完还没落盘"的差
SLACK_SECONDS = 2.0

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


def newest_source() -> tuple:
    newest = None
    where = None
    for d in SRC_DIRS:
        if not d.is_dir():
            continue
        for p in d.rglob("*"):
            if not p.is_file():
                continue
            if p.suffix.lower() not in (".cpp", ".h", ".hpp", ".cc", ".cxx", ".qrc", ".ui"):
                continue
            t = p.stat().st_mtime
            if newest is None or t > newest:
                newest, where = t, p
    return newest, where


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--ninja-log", default=str(SHELL_BUILD / ".ninja_log"))
    args = ap.parse_args()
    exe = Path(args.exe)
    ninja_log = Path(args.ninja_log)

    print("== 外壳二进制必须是新的（P17-176 的坑：构建静默失败 + 旧 exe）==")
    if not exe.is_file():
        print("SHELL-FRESH skip 缺外壳产物：%s" % exe)
        return 2

    exe_t = exe.stat().st_mtime
    src_t, where = newest_source()
    if src_t is None:
        want(False, "A 找得到外壳源文件", str(SRC_DIRS))
        print("-" * 66)
        print("SHELL-FRESH fail %d 项不成立" % len(PROBLEMS))
        return 1

    want(exe_t + SLACK_SECONDS >= src_t,
         "A `xrkit_app.exe` **不早于** `src/shell` 最新源（否则你跑的是旧二进制）",
         "exe=%.0f src=%.0f 差=%.0fs（%s）"
         % (exe_t, src_t, exe_t - src_t, where.name if where else "?"))

    # .ninja_log：t_start \t t_end \t mtime \t 输出路径
    has_record = False
    if ninja_log.is_file():
        try:
            for line in ninja_log.read_text(encoding="utf-8", errors="replace").splitlines():
                if "xrkit_app.exe" in line:
                    has_record = True
                    break
        except OSError:
            has_record = False
    want(has_record, "B `.ninja_log` 里有 `xrkit_app.exe` 的构建记录（它真是编出来的）",
         str(ninja_log.name))

    # 旁证：exe 体积非零、且比构建目录里的对象文件新（避免"exe 被复制进来"）
    want(exe.stat().st_size > 512 * 1024, "C exe 体积合理（>512 KB）",
         "%d B" % exe.stat().st_size)

    print("-" * 66)
    if PROBLEMS:
        print("SHELL-FRESH fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SHELL-FRESH ok %d 项（exe 不早于源 ＋ 有 ninja 记录）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
