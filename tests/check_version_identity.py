#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""版本号只许一份真来源（审计 2026-10-06 补充 H1-2；用户 2026-10-07 拍板"候选 A"）。

事实（修前，已实测）：
  · `version.json` 与 `include/xrkit/core/version.h` 是**两个**"唯一事实来源"，且**没有任何
    检查比对**它们：exe 的资源与 `core::version_string()` 编自 version.h 的常量
    （`kPrerelease = "ALPHA.0"` / `kBuildCount = 0`，**从来没被写回**过），包目录名与
    `version.txt` 来自 version.json；现有的两条锁（`.rc ↔ version.h`、exe 资源 ↔
    `core::version_string()`）**都在各自源内自比**。
  · 后果（对外可见）：同一个交付物里 ——
      `dist\\2.0.0\\ALPHA\\60\\xrkit_app.exe` 的属性页 = `ProductVersion=2.0.0 ALPHA.0`、
      `FileVersion=2.0.0.0`，而同一个包的 `version.txt` = `version=2.0.0 ALPHA.60`。
    实测原始行（本判据修前跑）：`EXEVERSION product=2.0.0 ALPHA.0 file=2.0.0 numeric=2.0.0.0
    path=dist\\2.0.0\\ALPHA\\60\\xrkit_app.exe` vs `version=2.0.0 ALPHA.60`。

口径（用户拍板）：
  · **真来源 = `version.json` 的 tree**（最后一次成功发包：版本+标记+序号）；`version.h` 是派生；
  · 判据只对 **包内 exe** + `version.h`/`.rc` 断言，**不**对 build 目录的 exe 断言
    （开发构建用缺省值合法；今天门禁全绿正是因为缺了"包内 exe"这一段）；
  · 期望值**从 tree 现算**（不写死串 —— G1-2 的教训：期望值抄被测产物 = 假绿）；
  · **不读 `current.index`** —— 它是"下一个号"，门禁与构建 GUI 一动就把它推一格（实测 59→60）。

判据（每条都能判红）：
  ① `version.h`：`kPrerelease == "<标记>.<N>"` 且 `kBuildCount == N`（N / 标记来自 tree）；
  ② `assets/xrkit_winres.rc`：`ProductVersion == v`、`FILEVERSION` 第 4 段 == N；
  ③ **包内 exe**（仅对 `version.txt` 里声明了 `exe_identity=` 的序号）：`ProductVersion == v`
     且 `numeric` 第 4 段 == N —— 判据自己读 exe 的 VERSIONINFO，**不信任那行自报**；
  ④ 自证：喂"kBuildCount = N+1 的假 version.h""ProductVersion 不同的假 exe 文本"必须报红。

退出码：0 = 绿；1 = 有判据不成立；2 = 本次没有"带身份声明的新包"（调用方记台账）。
"""

from __future__ import annotations

import ctypes
import json
import re
import sys
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "include" / "xrkit" / "core" / "version.h"
WINRES_RC = ROOT / "assets" / "xrkit_winres.rc"
STORE = ROOT / "version.json"
DIST = ROOT / "dist"

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


# ── 读 exe 的 VERSIONINFO（独立于 CLI/C++ 的第二份读法：ctypes 直读 Version API）──

_ver = ctypes.WinDLL("version")
_ver.GetFileVersionInfoSizeW.restype = wintypes.DWORD
_ver.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
_ver.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                     ctypes.c_void_p]
_ver.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
                                ctypes.POINTER(wintypes.UINT)]


def read_strings(exe: Path) -> dict[str, str] | None:
    """读 StringFileInfo + 定长块 FILEVERSION。

    ★ 两个坑（都在本批实测踩过，别照抄网上那种"string_at(ptr, len)"的写法）：
      · 串值的 `puLen` 是**字符数**（含结尾 NUL），不是字节数 —— 按字节读会得到
        `2.0.0 A` 这种被砍一半的串（实测）；
      · `\\` 那块是 `VS_FIXEDFILEINFO`：`dwSignature`/`dwStrucVersion` 占前 8 字节，
        FILEVERSION 的 MS 在**偏移 8**（`words[2]`），不是 0。
    """
    handle = wintypes.DWORD(0)
    size = _ver.GetFileVersionInfoSizeW(str(exe), ctypes.byref(handle))
    if size == 0:
        return None
    block = ctypes.create_string_buffer(size)
    if not _ver.GetFileVersionInfoW(str(exe), 0, size, ctypes.cast(block, ctypes.c_void_p)):
        return None

    ptr = ctypes.c_void_p()
    length = wintypes.UINT(0)
    if not _ver.VerQueryValueW(block, "\\VarFileInfo\\Translation", ctypes.byref(ptr),
                               ctypes.byref(length)) or length.value < 4:
        return None
    pair = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_ushort * 2)).contents

    fields: dict[str, str] = {}
    for name in ("ProductVersion", "FileVersion", "ProductName", "OriginalFilename"):
        ptr = ctypes.c_void_p()
        length = wintypes.UINT(0)
        sub = "\\StringFileInfo\\%04x%04x\\%s" % (pair[0], pair[1], name)
        if not _ver.VerQueryValueW(block, sub, ctypes.byref(ptr), ctypes.byref(length)):
            fields[name] = ""
            continue
        fields[name] = ctypes.wstring_at(ptr.value, length.value).rstrip("\x00")
    ptr = ctypes.c_void_p()
    length = wintypes.UINT(0)
    if _ver.VerQueryValueW(block, "\\", ctypes.byref(ptr), ctypes.byref(length)) \
            and length.value >= 24:
        words = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint * 6)).contents
        ms, ls = words[2], words[3]
        fields["numeric"] = "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    else:
        fields["numeric"] = ""
    return fields


# ── 分析器（纯文本 → 问题清单；下面会被喂反例自证）────────────────────────────

def header_problems(header_text: str, prerelease: str, index: int) -> list[str]:
    problems: list[str] = []
    want_pre = prerelease + "." + str(index) if prerelease else ""
    match = re.search(r'kPrerelease\s*=\s*"([^"]*)"', header_text)
    if match is None:
        problems.append("version.h 里读不到 kPrerelease")
    elif match.group(1) != want_pre:
        problems.append("version.h 的 kPrerelease=「%s」≠ tree 现算的「%s」"
                        % (match.group(1), want_pre))
    match = re.search(r"kBuildCount\s*=\s*(\d+)", header_text)
    if match is None:
        problems.append("version.h 里读不到 kBuildCount")
    elif int(match.group(1)) != index:
        problems.append("version.h 的 kBuildCount=%s ≠ tree 的最后一次发包序号 %d"
                        % (match.group(1), index))
    return problems


def rc_problems(rc_text: str, want_version: str, index: int) -> list[str]:
    problems: list[str] = []
    match = re.search(r'VALUE\s+"ProductVersion",\s*"([^"]*)"', rc_text)
    if match is None:
        problems.append(".rc 里读不到 ProductVersion")
    elif match.group(1) != want_version:
        problems.append(".rc 的 ProductVersion=「%s」≠ 期望「%s」" % (match.group(1), want_version))
    match = re.search(r"FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)", rc_text)
    if match is None:
        problems.append(".rc 里读不到 FILEVERSION")
    elif int(match.group(4)) != index:
        problems.append(".rc 的 FILEVERSION 第 4 段=%s ≠ 序号 %d" % (match.group(4), index))
    return problems


def exe_problems(fields: dict, want_version: str, index: int) -> list[str]:
    problems: list[str] = []
    if fields.get("ProductVersion") != want_version:
        problems.append("exe 的 ProductVersion=「%s」≠ 期望「%s」"
                        % (fields.get("ProductVersion"), want_version))
    numeric = fields.get("numeric", "").split(".")
    if len(numeric) != 4 or numeric[3] != str(index):
        problems.append("exe 的数值版本=「%s」↛ 第 4 段应为 %d"
                        % (fields.get("numeric"), index))
    return problems


# ── 主流程 ─────────────────────────────────────────────────────────────────

def last_release() -> tuple[str, str, int] | None:
    """从 tree 里取**最后一次成功发包**（版本、标记、序号）。★ 故意不读 current.index。"""
    store = json.loads(STORE.read_text(encoding="utf-8"))
    best = None
    for version, markers in (store.get("tree") or {}).items():
        for marker, records in (markers or {}).items():
            for index_text in (records or {}):
                try:
                    index = int(index_text)
                except ValueError:
                    continue
                if best is None or index > best[2]:
                    best = (version, marker, index)
    return best


def newest_package() -> Path | None:
    if not DIST.is_dir():
        return None
    best: tuple[int, Path] | None = None
    for index_dir in DIST.glob("*/*/*"):
        if not index_dir.is_dir() or not index_dir.name.isdigit():
            continue
        score = int(index_dir.name)
        if best is None or score > best[0]:
            best = (score, index_dir)
    return best[1] if best else None


def read_version_txt(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for row in path.read_text(encoding="utf-8", errors="replace").splitlines():
        key, sep, value = row.partition("=")
        if sep:
            out[key.strip()] = value.strip()
    return out


def main() -> int:
    say("VERSION-IDENTITY 版本号只许一份真来源（tree → version.h → .rc → 包内 exe）")
    if not STORE.is_file():
        check(False, "version.json 在仓库里")
        say("VERSION-IDENTITY: FAIL")
        return 1
    found = last_release()
    if found is None:
        check(False, "version.json 的 tree 里有至少一次成功发包（否则期望值无从现算）",
              "tree 是空的")
        say("VERSION-IDENTITY: FAIL")
        return 1
    version, marker, index = found
    want_version = ("%s %s.%d" % (version, marker, index)) if marker else version
    say("  真来源：version.json 的 tree 最后一次成功发包 = %s（序号 %d）" % (want_version, index))

    # ① version.h
    header_text = VERSION_H.read_text(encoding="utf-8")
    problems = header_problems(header_text, marker, index)
    check(not problems, "① version.h 的 kPrerelease/kBuildCount == tree 现算的期望值",
          "\n      ".join(problems))

    # ② .rc
    if not WINRES_RC.is_file():
        check(False, "assets/xrkit_winres.rc 在仓库里")
    else:
        problems = rc_problems(WINRES_RC.read_text(encoding="utf-8"), want_version, index)
        check(not problems, "② .rc 的 ProductVersion/FILEVERSION 与 version.h 同源",
              "\n      ".join(problems))

    # ③ 包内 exe（声明制：只有带 `exe_identity=` 的包才判；修前打的旧序号记台账）
    pkg = newest_package()
    if pkg is None:
        NOTES.append("包内 exe 的版本身份（%s 下没有任何包）" % DIST.relative_to(ROOT))
        say("  LEDGER 本次没有 dist 产物 ⇒ ③ 未执行")
    else:
        fields_txt = read_version_txt(pkg / "version.txt")
        if "exe_identity" not in fields_txt:
            NOTES.append("序号 %s 的包没有 `exe_identity=` 声明（修前打的）⇒ ③ 未执行" % pkg.name)
            say("  LEDGER %s/version.txt 没有 `exe_identity=` 声明（修前打的包）⇒ ③ 未执行"
                % pkg.relative_to(ROOT).as_posix())
        else:
            pkg_version = fields_txt.get("version", "")
            pkg_index = fields_txt.get("index", "")
            check(pkg_version == want_version and pkg_index == str(index),
                  "③ 最新包 %s 的 version.txt == tree 现算的期望值" % pkg.relative_to(ROOT).as_posix(),
                  "version=「%s」(期望「%s」) index=「%s」(期望 %d)"
                  % (pkg_version, want_version, pkg_index, index))
            exe = pkg / "xrkit_app.exe"
            fields = read_strings(exe) if exe.is_file() else None
            if fields is None:
                check(False, "③ %s 里能读到 xrkit_app.exe 的 VERSIONINFO"
                      % pkg.relative_to(ROOT).as_posix())
            else:
                problems = exe_problems(fields, want_version, index)
                check(not problems,
                      "③ 包内 exe 的 ProductVersion/数值版本第 4 段 == 期望（**现读 exe**，"
                      "不信任 version.txt 自报）",
                      "\n      ".join(problems))
                say("  原始读数：%s → product=「%s」numeric=%s（包内 version.txt: %s）"
                    % (exe.relative_to(ROOT).as_posix(), fields.get("ProductVersion"),
                       fields.get("numeric"), pkg_version))

    # ④ 自证：分析器不是恒真式
    bad_header = header_text.replace("kBuildCount = %d" % index, "kBuildCount = %d" % (index + 1))
    check(bool(header_problems(bad_header, marker, index)),
          "自证：kBuildCount 改成 N+1 的假 version.h 必须被判红")
    bad_fields = {"ProductVersion": "2.0.0 ALPHA.0", "numeric": "2.0.0.0"}
    check(bool(exe_problems(bad_fields, want_version, index)),
          "自证：ProductVersion/数值版本对不上的假读数必须被判红")
    check(bool(rc_problems('FILEVERSION 2,0,0,0\n VALUE "ProductVersion", "2.0.0 ALPHA.0"',
                           want_version, index)),
          "自证：旧形态的 .rc 必须被判红")

    if FAILURES:
        say("VERSION-IDENTITY: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("VERSION-IDENTITY: PASS%s" % ("（③ 未执行，见台账）" if NOTES else ""))
    return 2 if NOTES else 0


if __name__ == "__main__":
    sys.exit(main())
