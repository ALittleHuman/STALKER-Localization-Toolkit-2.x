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
import hashlib
import re
import shutil
import sys
import tempfile
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
    """★ 2026-10-23：包选择收敛到 `tests/package_select.py`（修前这里是"跨标记最大序号"）。"""
    from package_select import resolve_current_package
    pkg, _marker, _index, reason = resolve_current_package()
    say("  [选包] %s ⇒ %s" % (reason, pkg if pkg is not None else "<无>"))
    return pkg


def read_version_txt(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    # ★ `version.txt` 是 PowerShell 5.1 的 `Set-Content -Encoding UTF8` 写的 ⇒ **带 BOM**
    #   ⇒ 用 `utf-8-sig` 读，否则第一行的键会变成 `"\ufeffversion"`（本批实测踩到：
    #   ③ 报 `version=「」` 而 index/commit 都对 —— 看着像"包里的版本号是空的"）。
    for row in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
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
    # ★★ 2026-10-23（口径更新，必须写清为什么）：**期望值改取 `current`**，不再取"tree 最后成功发包"。
    #   事实：`dist/` 现在会同时留**冻结参照包**（BETA.258）与**当前包**（BETA.1）。
    #   tree-last 跨标记取最大序号 ⇒ 期望变成 BETA.258，而 current 是 BETA.1
    #   ⇒ 本条 ③ 会拿"当前包"去比"另一个包的号"（实测 4 条红，全是这个口径错位，不是产品问题）。
    #   为什么现在敢读 `current`：它是"这一轮要验的那一个"，而且
    #     ① 唯一会静默改它的诊断写入者（`xrkit_build_gui --layout-report`）已于 2026-10-23 修掉，
    #        并新增门禁第 119 步 `layout-readonly` 锁住；
    #     ② `run_ci.py` 收尾逐字节恢复三件套并点名改动者。
    #   旧口径"不读 current.index"的理由（GUI/门禁会推它）已经不成立。
    from package_select import current_identity, tree_last_release
    version, marker, index = current_identity()
    if not marker or index <= 0:
        check(False, "version.json 的 `current` 有可用的 标记+序号（期望值从它来）",
              "current=%r" % ((version, marker, index),))
        say("VERSION-IDENTITY: FAIL")
        return 1
    want_version = ("%s %s.%d" % (version, marker, index)) if marker else version
    say("  真来源：version.json 的 `current` = %s（序号 %d）" % (want_version, index))
    tv, tm, ti = tree_last_release()
    if tm and ti > 0 and (tm, ti) != (marker, index):
        say("  注：tree 里最后一次成功发包是 %s.%d（与 current 不同 —— 那是**冻结参照包**）；"
            "本轮只验 current" % (tm, ti))

    # ① version.h
    header_text = VERSION_H.read_text(encoding="utf-8")
    problems = header_problems(header_text, marker, index)
    check(not problems, "① version.h 的 kPrerelease/kBuildCount == current 的期望值",
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
                  "③ 当前包 %s 的 version.txt == current 的期望值" % pkg.relative_to(ROOT).as_posix(),
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

    # ④ ★★ 2026-10-23（Q2）：**包内 exe 的 sha256** 硬不变量。
    #   口径：
    #     · `version.txt` 里必须有 `exe_sha256=`（由 `--build-package` 在包落到最终目录后写，唯一写入者）；
    #     · 本判据**独立再算一遍**包内 exe 的哈希（hashlib，不复用 C++ 实现），与字段逐字比；
    #     · 老包（BETA.1 / BETA.258 等）**没有这个字段** ⇒ 本脚本退 **3**（"不具备新不变量"），
    #       由门禁按档位处置：release 档 FAIL、其它档记未覆盖（NOT_COVERED）—— **不冒充通过**。
    pkg_for_sha = newest_package()
    missing_field = False
    if pkg_for_sha is not None:
        fields_sha = read_version_txt(pkg_for_sha / "version.txt")
        want_sha = fields_sha.get("exe_sha256", "").strip().lower()
        exe_sha_path = pkg_for_sha / "xrkit_app.exe"
        if "exe_sha256" not in fields_sha:
            missing_field = True
            NOTES.append("包 %s 没有 `exe_sha256=`（Q2 之前打的包）⇒ ④ 未覆盖"
                         % pkg_for_sha.relative_to(ROOT).as_posix())
            say("  NOT-COVERED %s/version.txt 没有 `exe_sha256=`（Q2 之前打的包）⇒ ④ 未执行"
                % pkg_for_sha.relative_to(ROOT).as_posix())
        elif not exe_sha_path.is_file():
            check(False, "④ %s 里有 xrkit_app.exe（算 sha256 需要它）"
                  % pkg_for_sha.relative_to(ROOT).as_posix())
        else:
            got_sha = hashlib.sha256(exe_sha_path.read_bytes()).hexdigest()
            check(bool(want_sha) and got_sha == want_sha,
                  "④ 包内 exe 现算的 sha256 == version.txt 的 `exe_sha256=`（**独立第三只眼**）",
                  "现算=%s / version.txt=%s" % (got_sha[:16] + "…", (want_sha or "（空）")[:16] + "…"))
            # 同一个包的两个字段必须互相同源：version= 里的「标记.序号」== index=
            check(fields_sha.get("version", "") == want_version
                  and fields_sha.get("index", "") == str(index),
                  "④ 同一份 version.txt 的 `version=` / `index=` 与 current 一致",
                  "version=「%s」index=「%s」" % (fields_sha.get("version", ""),
                                                 fields_sha.get("index", "")))
            say("  原始读数：%s → sha256=%s（version.txt: %s）"
                % (exe_sha_path.relative_to(ROOT).as_posix(), got_sha[:16] + "…", want_sha[:16] + "…"))
            # 反例自证：**改包内 exe 一个字节**，本判据必须红（在临时副本上做，不动产物）
            tmp = Path(tempfile.mkdtemp(prefix="sha_sc_", dir=str(ROOT / "build")))
            try:
                fake = tmp / "xrkit_app.exe"
                shutil.copy2(exe_sha_path, fake)
                data = bytearray(fake.read_bytes())
                data[len(data) // 2] ^= 0x01
                fake.write_bytes(data)
                flipped = hashlib.sha256(fake.read_bytes()).hexdigest()
                check(flipped != want_sha,
                      "自证：包内 exe 改一个字节 ⇒ 现算的 sha256 必须与 `exe_sha256=` 不同")
            finally:
                shutil.rmtree(tmp, ignore_errors=True)

    # ⑤ 自证：分析器不是恒真式
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
    if missing_field:
        # ★ Q2：老包不具备新不变量 ⇒ **第三态**（门禁 release 档 FAIL、其它档记未覆盖）
        say("VERSION-IDENTITY: NOT-COVERED（包缺 `exe_sha256=`，Q2 之前打的）")
        return 3
    say("VERSION-IDENTITY: PASS%s" % ("（③ 未执行，见台账）" if NOTES else ""))
    return 2 if NOTES else 0


if __name__ == "__main__":
    sys.exit(main())
