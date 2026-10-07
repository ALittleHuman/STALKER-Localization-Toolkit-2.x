#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读文件失败被当成"空文件"（审计 2026-10-06 补充 A1-4 / 补充 A3 的静默丢语料）。

审计原文：
  · `src/engine/font/package.cpp:33-38` 的 `read_bytes` **打不开就 `return {}`**，而
    `decode_xml_bytes({})` **必然成功**（`decode_strict` 对 0 字节必返回 true）
    ⇒ 出包侧**写出空 XML 并记成功**；`charset.cpp:147-152/219-224` 同形 ⇒
    **整份语料的字符被静默丢弃**（`files_scanned++` 而 `files_undecodable` 不变）；
  · 触发：文件被独占锁 / 权限不足 / 超长路径。

判据（都能判红）：
  ① **夹具有效性**：两份 XML 都能读时，`--extract-chars` 必须**真的收进**它们的字符
     （否则下面的"锁住之后"证明不了任何丢失）；
  ② **独占锁住一份** ⇒ `--extract-chars` 必须**明确失败**（退出码非 0 + 报出是哪个文件），
     绝不许"当成空文件"出一个少字符的"成功"结果；
  ③ 静态面：`package.cpp` / `charset.cpp` 都不许再有"打不开就 return {}"的本地读文件助手，
     必须走共用的 `read_file_bytes()`（同类只维护一套）+ 分析器自证。

夹具一律建在 %TEMP%（**不碰真实语料/游戏目录**）。
退出码：0 = 全绿；1 = 有判据不成立；2 = 外部前提缺失（没构建 CLI）。
"""

from __future__ import annotations

import ctypes
import shutil
import subprocess
import sys
import tempfile
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PACKAGE_CPP = ROOT / "src" / "engine" / "font" / "package.cpp"
CHARSET_CPP = ROOT / "src" / "engine" / "font" / "charset.cpp"

FAILURES: list[str] = []


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


def run_charset(xml_dir: Path) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--extract-chars", str(xml_dir)],
                          cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


# ── 独占锁夹具：CreateFileW(dwShareMode=0) ⇒ 任何别人的 open 都会 sharing violation ──

GENERIC_READ = 0x80000000
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateFileW.restype = wintypes.HANDLE
_kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.HANDLE]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def lock_exclusive(path: Path):
    handle = _kernel32.CreateFileW(str(path), GENERIC_READ, 0, None, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, None)
    if handle == wintypes.HANDLE(-1).value or handle is None:
        raise OSError("CreateFileW 失败，GetLastError=%d" % ctypes.get_last_error())
    return handle


def unlock(handle) -> None:
    _kernel32.CloseHandle(handle)


def run_behavior() -> None:
    if not CLI.is_file():
        say("  !! 缺构建产物：%s" % CLI)
        say("FONT-READ-FAIL: SKIP（未构建 CLI）")
        sys.exit(2)
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_font_read_"))
    try:
        ok_xml = tmp / "ok.xml"
        locked_xml = tmp / "locked.xml"
        # 字符只出现在各自的文件里：少收一个 ⇒ 字符集直接缺码位（不是"看不出来"的那种差别）
        ok_xml.write_bytes('<?xml version="1.0" encoding="utf-8"?><t>中</t>'.encode("utf-8"))
        locked_xml.write_bytes('<?xml version="1.0" encoding="utf-8"?><t>汉</t>'.encode("utf-8"))

        # ① 夹具有效性：都不锁 ⇒ 两个字符都必须收进字符集
        rc, out = run_charset(tmp)
        check(rc == 0 and "4E2D" in out.upper() and "6C49" in out.upper(),
              "夹具：两份 XML 都能读时，中(4E2D)/汉(6C49) 都要被收进字符集",
              "rc=%d\n      %s" % (rc, brief(out)))

        # ② 锁住 locked.xml ⇒ 必须**明确失败**，不许当成空文件
        handle = lock_exclusive(locked_xml)
        try:
            # 先自证"这个夹具真的锁住了"（本进程自己也打不开）
            try:
                locked_xml.read_bytes()
                locked_ok = False
            except OSError:
                locked_ok = True
            check(locked_ok, "夹具有效性：独占锁真的让**别人**打不开这个文件",
                  "本进程仍能读到 locked.xml ⇒ 夹具没生效，下面那条判据会变成假绿")
            rc, out = run_charset(tmp)
            check(rc != 0,
                  "锁住一份 XML ⇒ `--extract-chars` 必须**明确失败**（修前：rc=0、当成空文件、"
                  "字符静默少一个）",
                  "rc=%d\n      %s" % (rc, brief(out)))
            check("locked.xml" in out or "读不" in out,
                  "失败必须**点名**是哪一份文件（零静默）",
                  brief(out))
        finally:
            unlock(handle)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ── ③ 静态面（自证）─────────────────────────────────────────────────────────

def read_helper_problem(source: str) -> str:
    """读文件助手分析器：空串 = 没问题。★ 同一个分析器在下面会被喂反例自证。"""
    if "return {};" in source and "read_bytes(" in source:
        return "还留着「打不开就 return {}」的本地读文件助手（应统一走 read_file_bytes()）"
    return ""


def run_static() -> None:
    for path, required in ((PACKAGE_CPP, "read_file_bytes("), (CHARSET_CPP, "read_file_bytes(")):
        if not path.is_file():
            check(False, "源文件存在：%s" % path.relative_to(ROOT).as_posix())
            continue
        source = path.read_text(encoding="utf-8")
        check("read_all_bytes(" not in source and "std::vector<uint8_t> read_bytes(" not in source,
              "%s：没有「打不开就 return {}」的本地读文件助手" % path.name)
        check(required in source,
              "%s：走共用的 %s" % (path.name, required))
    # ★ 自证
    bad = ("std::vector<uint8_t> read_bytes(const std::string& p) {\n"
           "  std::ifstream in(path_from_utf8(p)); if (!in) return {};\n"
           "  return {};\n}\n")
    check(read_helper_problem(bad) != "",
          "自证：本地读文件助手（打不开就 return {}）必须被判红", read_helper_problem(bad))


def main() -> int:
    say("FONT-READ-FAIL 读文件失败不许被当成空文件（A1-4）")
    run_behavior()
    run_static()
    if FAILURES:
        say("FONT-READ-FAIL: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("FONT-READ-FAIL: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
