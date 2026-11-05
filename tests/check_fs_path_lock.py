# -*- coding: utf-8 -*-
"""静态锁：`fs::path(窄字符串)` 必须绝迹（DSH 2026-11-05）。

同一个坑咬了两次：
  ① `core::sha256_file()` 用 `fopen(path)`（ANSI）⇒ 中文路径打不开；
  ② 日志崩溃保留名 `fs::path("…_异常退出")` ⇒ 抛 "No mapping for the Unicode character…"。
根因都是 **`fs::path(std::string)` 按 ACP 解释**，必须走 `path_from_utf8()`。

判据：下面这些"从窄字符串直接构造 path"的写法，在 `src/` 与 `tools/cli/` 里出现即红。
（合法例外：参数本身就是 `std::filesystem::path` 的场景，不在此列表里。）
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCAN_DIRS = [ROOT / "src", ROOT / "tools" / "cli"]
# 逐条列出"曾经咬过人"的形态；新增窄构造时把它加进来（而不是放宽判据）
BANNED = (
    "fs::path(operands[",
    "std::filesystem::path(operands[",
    "fs::path(dir)",
    "fs::path(log_path)",
    "std::filesystem::path(dir)",
    "std::filesystem::path(log_path)",
    "fs::path(argv",
    "std::filesystem::path(argv",
)
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 静态锁：fs::path(窄字符串) 绝迹（必须 path_from_utf8） ==")
    hits = []
    scanned = 0
    for base in SCAN_DIRS:
        for f in base.rglob("*"):
            if f.suffix not in (".cpp", ".h", ".hpp"):
                continue
            scanned += 1
            text = f.read_text(encoding="utf-8", errors="replace")
            for bad in BANNED:
                if bad in text:
                    hits.append("%s（%s）" % (f.relative_to(ROOT), bad))
    (ok if scanned > 50 else fail)("扫描了 %d 个源文件" % scanned)
    if hits:
        for h in hits[:10]:
            fail("窄构造残留：" + h)
    else:
        ok("没有 fs::path(窄字符串) 的残留")
    uses = 0
    for base in SCAN_DIRS:
        for f in base.rglob("*"):
            if f.suffix not in (".cpp", ".h", ".hpp"):
                continue
            uses += f.read_text(encoding="utf-8", errors="replace").count("path_from_utf8(")
    (ok if uses >= 10 else fail)("`path_from_utf8(` 仍在广泛使用（%d 处）—— 不是把调用点删了糊弄" % uses)
    if PROBLEMS:
        print("FS-PATH-LOCK: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FS-PATH-LOCK: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
