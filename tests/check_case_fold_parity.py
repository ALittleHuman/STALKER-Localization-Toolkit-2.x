# -*- coding: utf-8 -*-
"""P17-112 判据：**小写折叠必须与 Python `str.lower()` 逐码位一致**（复审计 `01 F-10`）。

事实
----
1.x 的路径排序键是 `str(path).lower()`（Python 的 Unicode 小写映射）。
P17-44（`01 F-10`）在 2.x 里用 **Windows `LCMapStringEx`**（invariant ＋ `LCMAP_LOWERCASE`）
去"模仿"它，并留了一句"与 Python 的 simple lowercase 对西里尔/拉丁/希腊一致"。

本批把这句话放到**全域**上复核（`0x20..0x2FFFF`，跳过代理区）后**不成立**：
  · 单字符内分歧 **131276** 个码位（`U+01C5 ǅ` / `U+0524 Ԥ` / `U+03F4 ϴ` …）；
  · `U+0130 İ` 在 Python 下折成**两个**码位（`"i"` ＋ `U+0307`），Windows 保持不动。
⇒ 修法：**不再模仿** —— 表由 `tools/gen_case_fold_table.py` **从本机 CPython 导出**
（`include/xrkit/engine/text/case_fold_table.inc`），查找见 `case_fold.h`。

判据（三格，都是"跟 Python 对账"）
----------------------------------
① **表没漂**：真跑 `gen_case_fold_table.py --check` ⇒ 与磁盘上那份**逐字节**一致
   （与 `gen-drift` 同一套思路：生成物必须由生成器说了算）；
② **逐码位一致**：把 `0x20..0x2FFFF` 分批喂给 `--case-fold-probe`，**每个码位**都与
   Python 的 `chr(cp).lower()` 比（这是**穷尽**，不是抽样）；
③ **反例自证**：判据自己确认"分歧确实存在过"（否则这条锁是空转）——
   用 Python 现算 `U+0130`/`U+01C5` 与 **Windows API** 的差异，证明这格有意义。

退出码：0=绿 / 1=红 / 2=缺前提（没有 CLI 或没有生成器）。
"""

from __future__ import annotations

import ctypes
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
GENERATOR = ROOT / "tools" / "gen_case_fold_table.py"
TABLE = ROOT / "include" / "xrkit" / "engine" / "text" / "case_fold_table.inc"
LO, HI = 0x20, 0x30000          # 上界与生成器同口径（打开区间）
BATCH = 2000

PROBLEMS: list = []
SKIPPED: list = []     # ★ P17-125：本平台**没被验证**的格（不许算成"通过"）
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def windows_lower(ch: str) -> str:
    """Windows invariant 小写（复现修前那条路 —— 只用来做反例自证）。"""
    if sys.platform != "win32":
        return ch
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LCMapStringEx.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.LPCWSTR, ctypes.c_int,
                                  wintypes.LPWSTR, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_ssize_t]
    k32.LCMapStringEx.restype = ctypes.c_int
    buf = ctypes.create_unicode_buffer(16)
    n = k32.LCMapStringEx(None, 0x00000100, ch, len(ch), buf, len(buf), None, None, 0)
    return buf[:n] if n > 0 else ch


def probe(cli: Path, chars: list) -> dict:
    """把一批码位喂给探针，返回 {cp: 折叠结果}。

    ★ 探针口径是"**逐行**读、逐行折"（`--case-fold-probe`）⇒ **一个码位一行**。
      判据第一版把它们拼成一行、再拿整串当 key ⇒ 全部查不到（194528 个"不一致"）。
      这条协议现在钉死在这里：输入 = `chr(cp) + "\\n"` 逐个。
    """
    payload = "".join(ch + "\n" for ch in chars)
    proc = subprocess.run([str(cli), "--case-fold-probe"], input=payload.encode("utf-8"),
                          capture_output=True, timeout=600)
    text = proc.stdout.decode("utf-8", "replace")
    out = {}
    for line in text.splitlines():
        parts = line.split(" ")
        # 口径：`FOLD <原文 hex> <case_fold_lower hex> <path_lower hex>`
        if len(parts) == 4 and parts[0] == "FOLD":
            src = bytes.fromhex(parts[1]).decode("utf-8", "replace")
            out[src] = (bytes.fromhex(parts[2]).decode("utf-8", "replace"),
                        bytes.fromhex(parts[3]).decode("utf-8", "replace"))
    return out


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-112 小写折叠与 Python `str.lower()` 逐码位一致（复审计 `01 F-10`）==")
    if not cli.is_file():
        print("!! 缺构建产物：%s" % cli)
        print("CASE-FOLD skip 缺 CLI")
        return 2
    if not GENERATOR.is_file() or not TABLE.is_file():
        print("!! 缺生成器或表")
        print("CASE-FOLD skip 缺生成器/表")
        return 2

    # ① 表没漂（真跑生成器）
    proc = subprocess.run([sys.executable, str(GENERATOR), "--check"],
                          capture_output=True, timeout=1800)
    out = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace").strip()
    want(proc.returncode == 0 and "CASE-FOLD-TABLE ok" in out,
         "① 表与 CPython 导出结果逐字节一致（生成物由生成器说了算）", out.splitlines()[-1][:120])

    # ② 逐码位一致（穷尽）
    cps = [cp for cp in range(LO, HI) if not (0xD800 <= cp <= 0xDFFF)]
    bad_table = []
    bad_path = []
    checked = 0
    for start in range(0, len(cps), BATCH):
        chunk = cps[start:start + BATCH]
        chars = [chr(cp) for cp in chunk]
        keep = [(cp, ch) for cp, ch in zip(chunk, chars) if ch not in ("\n", "\r")]
        got = probe(cli, [ch for _, ch in keep])
        for cp, ch in keep:
            checked += 1
            want_low = ch.lower()
            have = got.get(ch)
            if have is None:
                bad_table.append((cp, "<探针没返回>", want_low))
                continue
            folded, as_path = have
            if folded != want_low:
                bad_table.append((cp, folded, want_low))
            # ★★ 这一格才是要害：**排序键在用的那个函数**（`path_lower`）
            if as_path != want_low:
                bad_path.append((cp, as_path, want_low))
    want(not bad_table,
         "② 穷尽 0x%X..0x%X 共 %d 个码位：`case_fold_lower` == Python `str.lower()`" % (LO, HI, checked),
         ("%d 个不一致，例如 %s" % (len(bad_table),
                                  "; ".join("U+%04X C++=%r py=%r" % m for m in bad_table[:3])))
         if bad_table else "%d 个码位全部一致" % checked)
    want(not bad_path,
         "②-b **排序键在用的 `path_lower`** 也与之逐码位一致（只考表查找 = 假绿）",
         ("%d 个不一致，例如 %s" % (len(bad_path),
                                  "; ".join("U+%04X path=%r py=%r" % m for m in bad_path[:3])))
         if bad_path else "%d 个码位全部一致" % checked)

    # ③ 反例自证：证明这格有意义（修前那条路确实有分歧）
    #
    # ★★ P17-125（复审计 `P2-7`）：**非 Windows 下这格原来会"假绿"**。
    #   事实（修前）：`windows_lower()` 在非 Windows 直接 `return ch` ⇒ 于是
    #   `windows_lower(ch) != ch.lower()` 对**每个**大写字母都成立（拿 ch 跟自己的小写比）
    #   ⇒ 两个探针必然"分歧" ⇒ `len(diverged) >= 2` **空转通过**；而那句
    #   "（非 Windows ⇒ 无法自证，如实标注）" 的兜底文案**永远打印不出来**
    #   —— 因为 `diverged` 永远非空。这是"判据自称如实、实际空转"的典型。
    #   口径改为**三支**：
    #     · Windows ⇒ 真自证（必须找到 ≥2 例分歧）；
    #     · 非 Windows ⇒ **如实 SKIP**（`sys.exit` 之外的显式标记，且**不计入通过项**），
    #       并**不把 CASES 加上去** —— 免得"跳过"被算成"通过"。
    if sys.platform != "win32":
        print("   ⚠ ③ 反例自证：本机非 Windows ⇒ **如实 SKIP**（Windows API 那条路量不到）")
        print("      （这不是通过：本格在本平台**没有**被验证，别把它读成绿的）")
        SKIPPED.append("③ 反例自证（Windows API 差异）")
    else:
        diverged = []
        for cp in (0x0130, 0x01C5, 0x0524, 0x03F4):
            ch = chr(cp)
            if windows_lower(ch) != ch.lower():
                diverged.append("U+%04X" % cp)
        want(len(diverged) >= 2,
             "③ 反例自证：修前那条路（Windows API）确实与 Python 有分歧",
             "分歧样例 " + ",".join(diverged) if diverged else "（Windows 上竟找不到分歧 ⇒ 这条锁空转）")
        # ★ 不在这里 `CASES += 1`：`CASES` 是模块级名字，在 `main` 里赋值会被当成局部名
        #   ⇒ `UnboundLocalError`（本批自己踩的坑）；且 `want()` 已经计过"通过项"，
        #   再计一次是**双重计数**。模块级 `CASES` 由 `want()` 维护。

    print("-" * 66)
    if PROBLEMS:
        print("CASE-FOLD fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    if SKIPPED:
        print("CASE-FOLD ok %d 项（表由 CPython 导出且没漂 ＋ **穷尽逐码位**与 `str.lower()` 一致；"
              "复审计 `01 F-10`）—— ★ **本平台跳过 %d 项**：%s"
              % (CASES, len(SKIPPED), "、".join(SKIPPED)))
        return 0
    print("CASE-FOLD ok %d 项（表由 CPython 导出且没漂 ＋ **穷尽逐码位**与 `str.lower()` 一致 ＋ "
          "反例自证；复审计 `01 F-10`）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
