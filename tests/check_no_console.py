#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""无控制台窗口门禁（用户 2026-09-27：「不要打开cmd」）。

判据（产品代码里**不许**出现会弹控制台窗口的启动方式）：
  ① `std::system(` —— 走 `cmd.exe /c`，在 WIN32 子系统的 GUI 里会闪黑窗；
     必须改用 `CreateProcessW` + `CREATE_NO_WINDOW`（本仓库的做法见 `tools/cli/build_cmd.cpp`）。
  ② 文件里出现 `new QProcess(` 却**没有** `setCreateProcessArgumentsModifier` ——
     子进程若是控制台程序（ffmpeg、CLI），Qt 会新建控制台窗口；必须加 `CREATE_NO_WINDOW`。

豁免（每条写明理由，**降级要可见**）：测试夹具里的 QProcess（离屏自检用，用户看不见窗口）。

输出：`NOCONSOLE ok` / `NOCONSOLE fail <详情>`；退出码 0/1。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 测试夹具：离屏自检里起的 ffmpeg/CLI，用户看不到窗口 ⇒ 允许不带标志
ALLOW_FILES_NO_FLAG = {"src/shell/smoke.cpp"}


def active_code(text: str) -> str:
    """只留活代码：去掉行尾 `//` 注释（与其它静态锁同口径）。"""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def scan() -> list:
    offenders = []
    roots = [ROOT / "src", ROOT / "tools"]
    files = []
    for r in roots:
        files += [p for p in r.rglob("*.cpp")]
    for src in sorted(files):
        rel = str(src.relative_to(ROOT)).replace("\\", "/")
        code = active_code(src.read_text(encoding="utf-8", errors="replace"))
        if re.search(r"\bstd::system\s*\(", code):
            offenders.append("%s: 用了 std::system( ⇒ 会弹 cmd 窗口" % rel)
        if "new QProcess(" in code and "setCreateProcessArgumentsModifier" not in code:
            if rel not in ALLOW_FILES_NO_FLAG:
                offenders.append("%s: new QProcess( 但没有 setCreateProcessArgumentsModifier" % rel)
    return offenders


def main() -> int:
    offenders = scan()
    if offenders:
        print("NOCONSOLE fail %d 处会弹控制台窗口：" % len(offenders))
        for o in offenders[:8]:
            print("             " + o)
        return 1
    print("NOCONSOLE ok 子进程启动全部无控制台窗口（豁免 %d 个测试夹具文件）"
          % len(ALLOW_FILES_NO_FLAG))
    return 0


if __name__ == "__main__":
    sys.exit(main())
