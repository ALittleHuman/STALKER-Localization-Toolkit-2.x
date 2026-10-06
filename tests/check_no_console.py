#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""无控制台窗口门禁（用户 2026-09-27：「不要打开cmd」）。

判据（产品代码里**不许**出现会弹控制台窗口的启动方式）：
  ① `std::system(` —— 走 `cmd.exe /c`，在 WIN32 子系统的 GUI 里会闪黑窗；
     必须改用 `CreateProcessW` + `CREATE_NO_WINDOW`（本仓库的做法见 `tools/cli/build_cmd.cpp`）。
  ② 文件里出现 `new QProcess(` 却**没有** `setCreateProcessArgumentsModifier` ——
     子进程若是控制台程序（ffmpeg、CLI），Qt 会新建控制台窗口；必须加 `CREATE_NO_WINDOW`。
  ③ ★**产品 exe 自己的 PE 子系统必须是 GUI(2)** —— 用户 2026-09-28：「软件打开弹出cmd的问题，
     也一直没修。」根因：只有构建器设了 `WIN32_EXECUTABLE`，主程序 `xrkit_app` 是 Console(3)。
     ⇒ 这条判据**直接读 PE 头**（真实字节），不依赖源码里写了什么。`xrkit_cli` 是命令行工具，
     按设计保持 Console(3)，**不查它**。

豁免（每条写明理由，**降级要可见**）：测试夹具里的 QProcess（离屏自检用，用户看不见窗口）。

输出：`NOCONSOLE ok` / `NOCONSOLE fail <详情>`；退出码 0/1。
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 测试夹具：离屏自检里起的 ffmpeg/CLI，用户看不到窗口 ⇒ 允许不带标志
ALLOW_FILES_NO_FLAG = {"src/shell/smoke.cpp"}

# ★★ 2026-10-06（审计处理批·0-8）：修前这里只收 `*.cpp`。
#   判据是"**产品代码里不许出现会弹控制台的启动方式**"，而 `std::system(` / `new QProcess(`
#   写在头文件里一样会被编进产品（本仓大量实现在头里的 inline 就有先例）⇒ 只扫 .cpp
#   等于判据**整类输入不可判红**（实测：`src/engine/_no_console_probe.h` 里放
#   `std::system("whoami")` 判据照样 `NOCONSOLE ok`）。
#   现在按"C++ 源码文件集"收：.cpp/.cc/.cxx/.h/.hpp/.hh/.hxx/.inl/.ipp。
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp")


def active_code(text: str) -> str:
    """只留活代码：去掉行尾 `//` 注释（与其它静态锁同口径）。

    ★ 为什么块注释也一并剥掉（0-8 顺手）：文件集扩到头文件以后，头文件里的**文档注释**
      更容易出现 `std::system(` 这种反例字样（本仓的注释即证据文化），只剥行尾注释会把
      "注释里提到"判成"代码里用了" —— 判据要判得**准**，不然扩大文件集只会引入假红。
    """
    out = []
    depth = 0
    for line in text.splitlines():
        code = line
        kept = []
        i = 0
        while i < len(code):
            two = code[i:i + 2]
            if depth > 0:
                if two == "*/":
                    depth -= 1
                    i += 2
                    continue
                i += 1
                continue
            if two == "/*":
                depth += 1
                i += 2
                continue
            if two == "//":
                break
            # 字符串 / 字符字面量的**内容**屏蔽成空格（分隔符保留）：
            # 里面的 `/*`／`std::system(` 都不是代码，判据不许拿它当证据。
            if code[i] in "\"'":
                quote = code[i]
                kept.append(quote)
                i += 1
                while i < len(code):
                    if code[i] == "\\":
                        kept.append(" ")
                        if i + 1 < len(code):
                            kept.append(" ")
                            i += 2
                            continue
                        i += 1
                        continue
                    if code[i] == quote:
                        kept.append(quote)
                        i += 1
                        break
                    kept.append(" ")
                    i += 1
                continue
            kept.append(code[i])
            i += 1
        out.append("".join(kept))
    return "\n".join(out)


def scan() -> list:
    offenders = []
    roots = [ROOT / "src", ROOT / "tools"]
    files = []
    for r in roots:
        files += [p for p in r.rglob("*") if p.suffix.lower() in SOURCE_SUFFIXES]
    for src in sorted(files):
        rel = str(src.relative_to(ROOT)).replace("\\", "/")
        code = active_code(src.read_text(encoding="utf-8", errors="replace"))
        if re.search(r"\bstd::system\s*\(", code):
            offenders.append("%s: 用了 std::system( ⇒ 会弹 cmd 窗口" % rel)
        if "new QProcess(" in code and "setCreateProcessArgumentsModifier" not in code:
            if rel not in ALLOW_FILES_NO_FLAG:
                offenders.append("%s: new QProcess( 但没有 setCreateProcessArgumentsModifier" % rel)
    return offenders


def pe_subsystem(path: Path):
    """读 PE 可选头的 Subsystem（2=Windows GUI / 3=Console）。取不到返回 None。"""
    try:
        data = path.read_bytes()[:0x600]
    except OSError:
        return None
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    opt = pe + 4 + 20
    return struct.unpack_from("<H", data, opt + 0x44)[0]


def scan_exes() -> list:
    """产品 exe 的子系统判据：GUI 程序的 exe 必须是 2（用户看不见黑框才算数）。"""
    offenders = []
    checked = []
    for name in ("xrkit_app.exe", "xrkit_build_gui.exe"):
        for preset in ("msvc-shell", "msvc-release"):
            p = ROOT / "build" / preset / "src" / "shell" / name
            if not p.exists():
                continue
            sub = pe_subsystem(p)
            checked.append("%s=%s" % (name, sub))
            if sub != 2:
                offenders.append("%s: PE 子系统=%s（GUI 程序必须是 2，否则双击弹黑框）"
                                 % (p.relative_to(ROOT), sub))
    if not checked:
        offenders.append("一个产品 exe 都没找到（先构建 msvc-shell 预设）—— 这条判据没被满足")
    return offenders, checked


def main() -> int:
    offenders = scan()
    exe_offenders, checked = scan_exes()
    offenders += exe_offenders
    if offenders:
        print("NOCONSOLE fail %d 处会弹控制台窗口：" % len(offenders))
        for o in offenders[:8]:
            print("             " + o)
        return 1
    print("NOCONSOLE ok 子进程启动无控制台窗口；产品 exe 子系统 %s（豁免 %d 个测试夹具文件）"
          % (", ".join(checked), len(ALLOW_FILES_NO_FLAG)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
