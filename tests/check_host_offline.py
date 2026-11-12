# -*- coding: utf-8 -*-
"""宿主联网边界判据（2026-11-12 起，联网已内置）。

1. 宿主 HTTP 必须走 core::http，并由 [network] enabled 统一拦截；
2. 「打开仓库页」仍只开浏览器；
3. 宿主有内置“检测更新”，不再依赖 xrkit.net_http；
4. 基础包不得出现 plugins_net/net_http.py（联网是宿主能力，不是插件文件）。
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "https://github.com/ALittleHuman/STALKER-Localization-Toolkit-2.x"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 宿主联网边界（内置 HTTP + 总开关 + 包内无联网插件）==")
    gate = ROOT / "src" / "core" / "http.cpp"
    gate_text = gate.read_text(encoding="utf-8", errors="replace") if gate.is_file() else ""
    if "network_enabled" in gate_text and "联网功能已关闭" in gate_text:
        ok("宿主 HTTP 有 [network] enabled 统一拦截")
    else:
        fail("core/http.cpp 缺少联网总开关拦截")

    header = ROOT / "src" / "shell" / "release_page.h"
    if not header.is_file():
        fail("缺 src/shell/release_page.h（仓库页 URL 唯一来源）")
    elif EXPECTED in header.read_text(encoding="utf-8"):
        ok("仓库页 URL 固定在 release_page.h")
    else:
        fail("release_page.h 的 URL 不是约定的发布页")

    hub = ROOT / "src" / "shell" / "hub.cpp"
    body = hub.read_text(encoding="utf-8") if hub.is_file() else ""
    if "打开仓库页" in body and "QDesktopServices::openUrl" in body:
        ok("菜单项「打开仓库页」只调 QDesktopServices::openUrl")
    else:
        fail("菜单项缺失或没有走 QDesktopServices::openUrl")
    if "检测更新" in body and "HostUpdate" in body:
        ok("宿主内置检测更新入口存在")
    else:
        fail("宿主内置检测更新入口缺失")

    sdk = ROOT / "plugins" / "_sdk"
    sdk_has_winhttp = sdk.is_dir() and any(
        "winhttp" in f.read_text(encoding="utf-8", errors="replace").lower()
        for f in sdk.glob("*.py"))
    if sdk_has_winhttp:
        sys.path.insert(0, str(ROOT / "tests"))
        from package_select import resolve_current_package
        pkg, _m, _i, reason = resolve_current_package()
        if pkg is None:
            fail("SDK 有 winhttp，但找不到当前包（无法证明 base 包无联网插件）")
        else:
            stray = list(pkg.rglob("net_http.py")) + [
                d for d in pkg.rglob("plugins_net") if d.is_dir()]
            if stray:
                fail("包内出现 plugins_net/net_http.py（%s）" % reason)
            else:
                ok("包内无 plugins_net/net_http.py（联网走宿主）")
    else:
        ok("plugins/_sdk 没有 winhttp（无需检查包内）")

    if PROBLEMS:
        print("HOST-OFFLINE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("HOST-OFFLINE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
