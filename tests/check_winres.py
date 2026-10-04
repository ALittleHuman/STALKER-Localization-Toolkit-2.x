#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Windows 版本资源（VERSIONINFO）门禁：exe 里的元数据必须**非空**，且与 `core::version_string()` 同源。

为什么单列一条（用户 2026-10-04）：当前 exe "完全没有元数据、也没有签名" ⇒ 杀软误报。
判据（每一条都能判红）：
  ① 八个字段一个都不许空（CompanyName / ProductName / FileDescription / FileVersion /
     ProductVersion / OriginalFilename / InternalName / LegalCopyright）——
     资源没挂上、或挂成了**另一个目标**的资源，都在这里红；
  ② `FileVersion` == `xrkit_cli.exe --version` 打印的 `core::version_string()` 的**版本号部分**：
     谁手写一个旧版本串进 `.rc` ⇒ 立刻红（`.rc` 与 `version.h` 的漂移另由
     `tools/gen_winres.py --check` 拦住，两条锁一前一后）；
  ③ `ProductVersion` == 完整的 `core::version_string()`（含预发布标记，如 "2.0.0 ALPHA.0"）；
  ④ 定长块 `FILEVERSION` == (主,次,补,构建号)（数字部分也同源，不只是字符串）；
  ⑤ `OriginalFilename` == 该 exe 自己的文件名（**两个目标不许互相挂错**）。

读 VersionInfo 走 ctypes 直调 Win32（GetFileVersionInfoSizeW / GetFileVersionInfoW / VerQueryValueW）：
不依赖 PowerShell 的输出格式，也不受控制台码页影响 —— 中文描述必须逐字比（这是判据，不是展示）。

用法：
    python tests/check_winres.py <取版本号的 xrkit_cli.exe> <要检查的 exe> [<要检查的 exe> ...]
    第一个参数既用来问 `--version`（拿到编译进二进制的 core::version_string()），也一并受检。
退出码：0 = 全过；1 = 判据不通过；2 = exe 缺失 / 跑不起来（外部前提，由 run_ci 按档位记 FAIL 或 SKIP）
输出：`WINRES ok ...`（含实测字段值）/ `WINRES fail <详情>`
"""

from __future__ import annotations

import ctypes
import re
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

REQUIRED = ("CompanyName", "ProductName", "FileDescription", "FileVersion",
            "ProductVersion", "OriginalFilename", "InternalName", "LegalCopyright")

_version = ctypes.WinDLL("version")
_version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
_version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
_version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                         ctypes.c_void_p]
_version.GetFileVersionInfoW.restype = wintypes.BOOL
_version.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                    ctypes.POINTER(ctypes.c_void_p),
                                    ctypes.POINTER(ctypes.c_uint)]
_version.VerQueryValueW.restype = wintypes.BOOL


def _load(path: Path):
    """把 exe 的版本资源整体读进内存（返回 buffer；没有资源返回 None）。

    ★ buffer 一律显式 `ctypes.cast(..., c_void_p)` 再传：`create_string_buffer` 的
      c_char 数组能不能自动喂给 `c_void_p` 形参随 ctypes 版本而异，显式 cast 最稳。
    """
    handle = wintypes.DWORD(0)
    size = _version.GetFileVersionInfoSizeW(str(path), ctypes.byref(handle))
    if size == 0:
        return None
    buf = ctypes.create_string_buffer(size)
    if not _version.GetFileVersionInfoW(str(path), 0, size, ctypes.cast(buf, ctypes.c_void_p)):
        return None
    return buf


def read_strings(path: Path) -> dict[str, str] | None:
    """读 StringFileInfo 那一块；读不到（没资源 / 不是 PE）返回 None。"""
    buf = _load(path)
    if buf is None:
        return None
    ptr = ctypes.c_void_p()
    length = ctypes.c_uint()
    if not _version.VerQueryValueW(buf, "\\VarFileInfo\\Translation",
                                   ctypes.byref(ptr), ctypes.byref(length)) or length.value < 4:
        return None
    pair = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_ushort * 2)).contents
    out: dict[str, str] = {}
    for name in REQUIRED:
        sub = "\\StringFileInfo\\%04x%04x\\%s" % (pair[0], pair[1], name)
        ptr = ctypes.c_void_p()
        length = ctypes.c_uint()
        if not _version.VerQueryValueW(buf, sub, ctypes.byref(ptr), ctypes.byref(length)):
            out[name] = ""
            continue
        # 串值的长度是**字符数**（含结尾 NUL）；wstring_at 要的是地址整数
        out[name] = ctypes.wstring_at(ptr.value, length.value).rstrip("\x00")
    return out


def read_numeric(path: Path) -> tuple[int, int, int, int] | None:
    """读定长块 VS_FIXEDFILEINFO 的 FILEVERSION（4 段数字），读不到返回 None。"""
    buf = _load(path)
    if buf is None:
        return None
    ptr = ctypes.c_void_p()
    length = ctypes.c_uint()
    if not _version.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length)) or \
            length.value < 24:
        return None
    words = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint * 6)).contents
    ms, ls = words[2], words[3]
    return ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF


def compiled_version(cli: Path) -> tuple[str, int]:
    """问 CLI 要**编译进二进制的那份**版本串（core::version_string() + kBuildCount）。"""
    proc = subprocess.run([str(cli), "--version"], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=60)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    if proc.returncode != 0:
        raise RuntimeError("`%s --version` 退出码 %d：%r" % (cli.name, proc.returncode, text[:120]))
    m = re.match(r"^(?P<full>.+?) build=(?P<build>\d+)\s*$", text)
    if m is None:
        raise RuntimeError("`%s --version` 输出认不出来：%r" % (cli.name, text[:120]))
    return m.group("full"), int(m.group("build"))


def main() -> int:
    argv = sys.argv[1:]
    if len(argv) < 2:
        print("WINRES fail 用法：check_winres.py <取版本号的 cli.exe> <要检查的 exe> [...]")
        return 2
    paths = [Path(a) for a in argv]
    missing = [p for p in paths if not p.is_file()]
    if missing:
        print("WINRES fail 找不到 exe（先构建）：%s" % ", ".join(str(p) for p in missing))
        return 2
    try:
        full, build = compiled_version(paths[0])
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print("WINRES fail %s" % exc)
        return 2
    semver = full.split(" ")[0]
    numbers = [int(x) for x in semver.split(".")]
    want_numeric = (numbers[0], numbers[1], numbers[2], build)

    problems: list[str] = []
    for path in paths:
        got = read_strings(path)
        if got is None:
            problems.append("%s: 读不到 StringFileInfo（这台 exe **没有版本资源**）" % path.name)
            continue
        blank = [name for name in REQUIRED if not got[name].strip()]
        if blank:
            problems.append("%s: 这些字段是空的：%s" % (path.name, ", ".join(blank)))
        if got["FileVersion"].strip() != semver:
            problems.append("%s: FileVersion=%r 与 core::version_string() 的版本号部分 %r 不一致"
                            "（手写旧版本串 / .rc 与 version.h 漂移）"
                            % (path.name, got["FileVersion"], semver))
        if got["ProductVersion"].strip() != full:
            problems.append("%s: ProductVersion=%r 与 core::version_string() %r 不一致"
                            % (path.name, got["ProductVersion"], full))
        numeric = read_numeric(path)
        if numeric != want_numeric:
            problems.append("%s: 定长块 FILEVERSION=%s 与 version.h 的 %s 不一致"
                            % (path.name, numeric, want_numeric))
        if got["OriginalFilename"].strip().lower() != path.name.lower():
            problems.append("%s: OriginalFilename=%r 不是本 exe 的名字（**挂错目标的资源**）"
                            % (path.name, got["OriginalFilename"]))
        print("WINRES ok %s ProductName=%s | FileDescription=%s | FileVersion=%s | "
              "ProductVersion=%s | OriginalFilename=%s"
              % (path.name, got["ProductName"], got["FileDescription"], got["FileVersion"],
                 got["ProductVersion"], got["OriginalFilename"]))

    if problems:
        print("WINRES fail %d 处：" % len(problems))
        for item in problems:
            print("            " + item)
        return 1
    print("WINRES ok 版本资源与 core::version_string()=%r 同源（%d 个 exe 全过）" % (full, len(paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
