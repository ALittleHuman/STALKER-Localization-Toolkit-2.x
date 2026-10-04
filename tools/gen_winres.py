#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 `include/xrkit/core/version.h` 生成 Windows 版本资源（VERSIONINFO）→ `assets/xrkit_winres.rc`。

为什么必须生成，不许手写（用户硬口径）：
  · **版本只有一个事实来源**：`version.h` 里那五个常量（kVersionMajor/kVersionMinor/
    kVersionPatch/kPrerelease/kBuildCount）就是 `core::version_string()` 的来源 ——
    在 `.rc` 里再抄一遍版本号，迟早有人只改一处（改了 version.h 忘了 .rc ⇒
    资源里写着旧版本号，正是"手写旧版本串"那种事故）；
  · 所以 `.rc` 里**一个版本数字都不许手写**：本脚本读 version.h、算出版本串、
    填进 .rc；`.rc` 生成物入库，门禁跑 `--check` 逐字节比对（漂移＝FAIL，不是 SKIP）。

    python tools/gen_winres.py            # 生成
    python tools/gen_winres.py --check    # 只校验磁盘上的 .rc（门禁用）

★ 编码（2026-10-04 实测三轮，**别改成"看起来更规范"的写法**）：
  本文件按 **UTF-8 带 BOM + LF** 生成。为什么必须是**带 BOM**：
    · CMake 的 RC 规则不是直接调 rc.exe：生成出来的是
      `cmcldeps.exe RC <src> <dep> <out> "<前缀>" cl.exe rc.exe …`（见 build/msvc-shell 的
      rules.ninja），而**这条链不认 `#pragma code_page(65001)`** —— UTF-8 无 BOM 的中文注释
      会被按系统 ANSI(936/GBK) 解码，双字节把行尾换行/引号吃掉 ⇒ 报
      `C4819 …当前代码页(936)…` + `C2001 常量中有换行符` + `C1018 意外的 #elif`
      （C 系列 + 936 码页 ⇒ 是 cl 那条路在读；报错行还落在别的注释行上，极具误导）。
      实测（`build/_rc_probe/repro.py`，同一个文件、同一套参数）：**直跑 rc.exe 绿（exit 0），
      走 cmcldeps 红（exit 2）**。
    · 加 BOM 之后**两边都绿**（`build/_rc_probe/cand2.py` 的 B/C/D 三组）：cmcldeps exit 0、
      `.res` 900 B、链出的 exe 里 FileDescription 的中文逐字正确。
    · 同样实测绿的还有 UTF-16LE + BOM；但那样文件在 git 里算二进制、diff 看不见 ⇒ 不用。
    · `.editorconfig` 里给这个文件**单列**了 `charset = utf-8-bom`：全局那条是 `utf-8`（无 BOM），
      而遵守 .editorconfig 的编辑器会把 BOM 抹掉。BOM 丢了构建就会红，所以先在这里挡住。
  BOM 由本脚本写死并**断言**（`_assert_bom`）：它是判据的一部分，不是排版偏好。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
VERSION_H = ROOT / "include" / "xrkit" / "core" / "version.h"
OUT = ROOT / "assets" / "xrkit_winres.rc"

# 署名（**不是**冒充别的公司：这是本项目自己的名号；用户口径"若不确定写什么就先用中性且真实的"）
COMPANY = "STALKER Toolkit"
PRODUCT = "STALKER 汉化工具集"
COPYRIGHT = "Copyright (C) STALKER Toolkit"
COMMENTS = "非官方汉化工具，与 GSC Game World 无关"

# 每个 exe 一份（描述里的版本号由 version.h 现算，见 file_description()）：
#   (CMake 定义宏, exe 文件名, 描述主体)
TARGETS = [
    ("XRKIT_WINRES_CLI", "xrkit_cli.exe", "引擎命令行"),
    ("XRKIT_WINRES_APP", "xrkit_app.exe", "图形外壳"),
    ("XRKIT_WINRES_GUI", "xrkit_build_gui.exe", "构建器"),
]


def fail(msg: str) -> None:
    raise SystemExit("GEN-WINRES fail 生成版本资源失败：%s" % msg)


# ── 读 version.h（唯一事实来源）────────────────────────────────────────────
def read_int(text: str, name: str) -> int:
    m = re.search(r"inline\s+constexpr\s+int\s+%s\s*=\s*(\d+)\s*;" % re.escape(name), text)
    if m is None:
        # 读不到就报错，**不许静默取默认值**（否则资源会静默写上一个假版本号）
        fail("在 %s 里读不到 %s（读不到就报错，不许静默降级）"
             % (VERSION_H.relative_to(ROOT), name))
    return int(m.group(1))


def read_str(text: str, name: str) -> str:
    m = re.search(r'inline\s+constexpr\s+const\s+char\s*\*\s*%s\s*=\s*"((?:[^"\\]|\\.)*)"\s*;'
                  % re.escape(name), text)
    if m is None:
        fail("在 %s 里读不到 %s" % (VERSION_H.relative_to(ROOT), name))
    raw = m.group(1)
    # C 串里只会出现这两种转义；别的原样留着（真出现说明 version.h 被改坏了）
    return raw.replace('\\"', '"').replace("\\\\", "\\")


class Version:
    def __init__(self, major: int, minor: int, patch: int, prerelease: str, build: int):
        self.major = major
        self.minor = minor
        self.patch = patch
        self.prerelease = prerelease
        self.build = build

    @property
    def semver(self) -> str:
        return "%d.%d.%d" % (self.major, self.minor, self.patch)

    @property
    def short(self) -> str:
        return "%d.%d" % (self.major, self.minor)

    @property
    def full(self) -> str:
        """与 `core::version_string()` **逐字同规则**：semver [+ " " + 标记]。"""
        return self.semver if not self.prerelease else self.semver + " " + self.prerelease

    @property
    def numeric(self) -> str:
        return "%d,%d,%d,%d" % (self.major, self.minor, self.patch, self.build)


def load_version() -> Version:
    if not VERSION_H.is_file():
        fail("找不到唯一事实来源：%s" % VERSION_H)
    text = VERSION_H.read_text(encoding="utf-8")
    v = Version(read_int(text, "kVersionMajor"), read_int(text, "kVersionMinor"),
                read_int(text, "kVersionPatch"), read_str(text, "kPrerelease"),
                read_int(text, "kBuildCount"))
    # version.cpp 的 version_string_is_well_formed() 同口径：主.次.补[ 标记]
    if not re.fullmatch(r"\d+\.\d+\.\d+( [^\s].*)?", v.full):
        fail("从 version.h 算出的版本串形态不对：%r" % v.full)
    return v


def cquoted(text: str) -> str:
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def file_description(v: Version, what: str) -> str:
    """描述里的版本号是**现算的**（version.h 一份），不是抄来的常量。"""
    return "%s %s %s" % (PRODUCT, v.short, what)


BOM = b"\xef\xbb\xbf"   # UTF-8 BOM：**必须有**（cl.exe 靠它才按 UTF-8 读，见文件头）


def _assert_bom(blob: bytes) -> None:
    """硬约束自证：生成物必须**以 UTF-8 BOM 开头**。

    为什么值得写成断言而不是注释：丢了 BOM 之后中文注释会被按 ANSI(936) 解码，
    rc.exe/cl.exe 报的是 `C2001 常量中有换行符`/`C1018 意外的 #elif`，而且报在**别的行**上
    （2026-10-04 第一次接门禁就是被这个误导了半天）。让生成器自己盯着，别靠人记得。
    """
    if not blob.startswith(BOM):
        fail("生成物必须以 UTF-8 BOM 开头（实测：无 BOM 时 CMake 的 RC 链路里 cl.exe 按 "
             "ANSI/936 解码，中文注释会把换行/引号吃掉，报出误导性的 C2001/C1018）")
    # 只拦"真的用了这条指令"（行首），注释里**讲**它为什么没用是允许的、也是必要的
    for line in blob.split(b"\n"):
        if line.strip().startswith(b"#pragma code_page"):
            fail("别用 #pragma code_page：实测它在 CMake 的 RC 链路（cmcldeps/cl.exe）里不起作用，"
                 "留着会让人误以为『无 BOM 也行』")


# ── 生成 .rc ────────────────────────────────────────────────────────────
def build() -> bytes:
    v = load_version()
    lines: list[str] = []
    a = lines.append
    a("// ★ 生成物，不许手改：由 tools/gen_winres.py 从 include/xrkit/core/version.h 生成。")
    a("//   门禁 tests/run_ci.py 跑 `gen_winres.py --check` 逐字节比对（不一致＝FAIL，不是 SKIP）。")
    a("//   版本唯一事实来源＝version.h（也就是 core::version_string() 那一份）——")
    a("//   本文件里**没有一个手写的版本数字**：FileVersion/ProductVersion/FILEVERSION")
    a("//   以及描述里的 \"%s\" 全是生成时从 version.h 算出来的。" % v.short)
    a("//")
    a("//   ★★ 本文件是 **UTF-8 带 BOM + LF**，那个 BOM **不能删**（2026-10-04 实测）：")
    a("//   CMake 的 RC 规则中间夹着 cmcldeps/cl.exe，而 cl.exe 不认 #pragma code_page(65001) ——")
    a("//   无 BOM 时中文注释按系统 ANSI(936/GBK) 解码，双字节把行尾换行/引号吃掉，报")
    a("//   C2001「常量中有换行符」/ C1018「意外的 #elif」（报在别的注释行上，极具误导）。")
    a("//   带 BOM ⇒ cl.exe 与 rc.exe 都按 UTF-8 读，中文才逐字正确。")
    a("#include <winver.h>")
    a("")
    a("// 目标由 CMake 传进来（每个 exe 一份 FileDescription / OriginalFilename / InternalName）：")
    for macro, exe, what in TARGETS:
        a("//   %-22s ⇒ /d %s" % (exe, macro))
    a("// ★ 一个都没定义 ⇒ 直接 #error（CMake 的 target_compile_definitions 没传到 rc.exe 时，")
    a("//   必须**编不过**，不许静默挂上一份别的目标的资源）。")
    for i, (macro, exe, what) in enumerate(TARGETS):
        head = "#if defined(%s)" % macro if i == 0 else "#elif defined(%s)" % macro
        a(head)
        a("#define XRKIT_WINRES_DESC %s" % cquoted(file_description(v, what)))
        a("#define XRKIT_WINRES_FILE %s" % cquoted(exe))
        a("#define XRKIT_WINRES_INTERNAL %s" % cquoted(exe[:-4]))
    a("#else")
    a("#error 未指定 XRKIT_WINRES_CLI / XRKIT_WINRES_APP / XRKIT_WINRES_GUI（rc.exe 没收到 /d 定义）")
    a("#endif")
    a("")
    a("VS_VERSION_INFO VERSIONINFO")
    a(" FILEVERSION %s" % v.numeric)
    a(" PRODUCTVERSION %s" % v.numeric)
    a(" FILEFLAGSMASK 0x3fL")
    a(" FILEFLAGS 0x0L")
    a(" FILEOS 0x40004L")
    a(" FILETYPE 0x1L")
    a(" FILESUBTYPE 0x0L")
    a("BEGIN")
    a('    BLOCK "StringFileInfo"')
    a("    BEGIN")
    a("        // 0x0804 = 简体中文；1200 = Unicode（rc.exe 把串按 UTF-16 存进资源）")
    a('        BLOCK "080404b0"')
    a("        BEGIN")
    a('            VALUE "CompanyName", %s' % cquoted(COMPANY))
    a('            VALUE "ProductName", %s' % cquoted(PRODUCT))
    a('            VALUE "FileDescription", XRKIT_WINRES_DESC')
    a('            VALUE "FileVersion", %s' % cquoted(v.semver))
    a('            VALUE "ProductVersion", %s' % cquoted(v.full))
    a('            VALUE "OriginalFilename", XRKIT_WINRES_FILE')
    a('            VALUE "InternalName", XRKIT_WINRES_INTERNAL')
    a('            VALUE "LegalCopyright", %s' % cquoted(COPYRIGHT))
    a('            VALUE "Comments", %s' % cquoted(COMMENTS))
    a("        END")
    a("    END")
    a('    BLOCK "VarFileInfo"')
    a("    BEGIN")
    a('        VALUE "Translation", 0x804, 1200')
    a("    END")
    a("END")
    a("")
    blob = BOM + "\n".join(lines).encode("utf-8")
    _assert_bom(blob)   # ★ 硬约束自证（见文件头）：BOM 必须在
    return blob


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 Windows 版本资源（VERSIONINFO）")
    parser.add_argument("--check", action="store_true", help="只校验磁盘上的 .rc")
    args = parser.parse_args()
    want = build()
    v = load_version()
    if args.check:
        if not OUT.is_file():
            fail("产物不存在：%s" % OUT.relative_to(ROOT))
        got = OUT.read_bytes()
        if got != want:
            fail("磁盘上的 %s 与 %s 不一致（%d 字节 vs %d 字节）—— 重新生成"
                 % (OUT.relative_to(ROOT), VERSION_H.relative_to(ROOT), len(got), len(want)))
        print("GEN-WINRES ok 版本资源与 version.h 一致（FileVersion=%s / ProductVersion=%s / %d 个目标）"
              % (v.semver, v.full, len(TARGETS)))
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(want)  # ★ bytes + LF：文本模式会把 \n 翻成 \r\n
    print("GEN-WINRES ok 已写 %s（%d 字节；FileVersion=%s / ProductVersion=%s）"
          % (OUT.relative_to(ROOT), len(want), v.semver, v.full))
    return 0


if __name__ == "__main__":
    sys.exit(main())
