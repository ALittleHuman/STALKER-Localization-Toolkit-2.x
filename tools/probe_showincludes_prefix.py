#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""量出 `cl.exe /showIncludes` 的**原始字节**前缀，供构建时显式喂给 CMake。

为什么需要这个脚本（2026-09-24 实测，别删）：
  · cl.exe 的提示消息是本地化的。本机（中文 MSVC 14.44）实测它以 **UTF-8 字节**输出
    `注意: 包含文件: `；
  · CMake 探测这个前缀时按**系统 ANSI 码页**解码，于是内部字符串变成乱码
    （`娉ㄦ剰: 鍖呭惈鏂囦欢: `），写进 `rules.ninja` 的 `msvc_deps_prefix`；
  · ninja 拿这个前缀与 cl 的输出**逐字节**比对 ⇒ 永不命中 ⇒ `.ninja_deps` 里一条
    头文件依赖都没有 ⇒ **改了 `.h` / 生成的 `.inc` 也不重编**，门禁拿着旧二进制判绿。
  · 在 CMake 里自己探测**没用**：`execute_process` 的输出同样会被按 ANSI 解码，绕不开。
    ⇒ 只能在这儿（Python 直接拿原始字节）量，再经**命令行**给 CMake：命令行参数走
    Windows 的宽字符接口，不经过码页，CMake 内部拿到的就是正确的 UTF-8 串。

用法：
    python tools/probe_showincludes_prefix.py                 # 打印前缀（UTF-8）
    python tools/probe_showincludes_prefix.py --define        # 打印 -DXRKIT_SHOWINCLUDES_PREFIX=…
退出码：0 = 量到了；2 = 量不到（缺 cl / 输出里找不到标记头文件）
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENV_FILE = ROOT / "build" / "msvc_env.txt"
DEFAULT_VS_ROOT = Path(os.environ.get(
    "XRKIT_VS", r"E:\Software\Tools\Visual Studio\2022\BuildTools"))
MARKER = "xrkit_probe_marker.h"


def load_env(env_file: Path, vs_root: Path) -> dict:
    """拿带 MSVC 的环境：优先用已导出的 env 文件，否则现场 call 一次 vcvars64.bat。"""
    if not env_file.is_file():
        vcvars = vs_root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if not vcvars.is_file():
            return {}
        env_file.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run('call "%s" >nul 2>&1 && set > "%s"' % (vcvars, env_file),
                       shell=True, check=False)
        if not env_file.is_file():
            return {}
    env = dict(os.environ)
    for line in env_file.read_bytes().decode("mbcs", errors="replace").splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            # 键归一成大写：`set` 输出 `Path=`，而 Windows 上 `os.environ` 的键是大写，
            # 不归一就会出现两个键、`env["PATH"]` 拿到没有 MSVC 的那份（实测踩到）。
            env[key.upper()] = value
    return env


def probe(env: dict, work: Path) -> str:
    """返回前缀（消息 + 一个空格，与 CMake 的英文口径一致；量不到返回空串）。"""
    cl = shutil.which("cl.exe", path=env.get("PATH", "")) or shutil.which("cl.exe")
    if not cl:
        return ""
    work.mkdir(parents=True, exist_ok=True)
    marker = work / MARKER
    marker.write_bytes(b"\n")
    src = work / "xrkit_probe.cpp"
    src.write_text('#include "%s"\nint xrkit_probe_marker;\n' % MARKER, encoding="utf-8")
    proc = subprocess.run([cl, "/nologo", "/showIncludes", "/c", str(src)],
                          env=env, cwd=str(work), capture_output=True)
    raw = proc.stdout + proc.stderr
    # cl 的第一行是源文件名回显，包含行在下一行 ⇒ 逐行找**完整绝对路径**，
    # 命中行的换行符之前、路径之前的那段就是前缀。
    native = str(marker).encode("utf-8")
    for line in raw.split(b"\r\n"):
        idx = line.find(native)
        if idx > 0:
            return (line[:idx].rstrip() + b" ").decode("utf-8")
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description="量 cl /showIncludes 的原始前缀")
    parser.add_argument("--env-file", default=str(DEFAULT_ENV_FILE))
    parser.add_argument("--vs-root", default=str(DEFAULT_VS_ROOT))
    parser.add_argument("--define", action="store_true",
                        help="按 cmake 命令行参数的形式打印")
    args = parser.parse_args()

    env = load_env(Path(args.env_file), Path(args.vs_root))
    prefix = probe(env, ROOT / "build" / "showincludes_probe")
    if not prefix:
        print("!! 量不到 /showIncludes 前缀", file=sys.stderr)
        return 2
    if args.define:
        sys.stdout.write("-DXRKIT_SHOWINCLUDES_PREFIX=%s" % prefix)
    else:
        sys.stdout.write(prefix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
