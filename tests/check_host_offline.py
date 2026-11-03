# -*- coding: utf-8 -*-
"""宿主"离线"判据（HA/DSH 2026-11-03）：

  1. 宿主源码**不许**出现任何网络栈 API：winhttp / WinHttp* / InternetOpen / URLDownloadToFile；
  2. 「查看最新版本（打开发布页）」入口存在，且**只**用 QDesktopServices::openUrl；
  3. URL 固定、HTTPS、指向 releases（防被人改成可变/明文地址）。

插件 SDK（plugins/_sdk）允许出现 winhttp —— 它不是宿主，且不随包。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST_DIRS = [ROOT / "src" / "shell", ROOT / "src" / "engine", ROOT / "src" / "core",
             ROOT / "include"]
BANNED = ("winhttp", "internetopen", "urldownloadtofile")
EXPECTED = "https://github.com/ALittleHuman/STALKER-Localization-Toolkit-2.x/releases"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 宿主离线判据（无网络栈 API；发布页只开浏览器）==")
    hits = []
    for base in HOST_DIRS:
        for path in base.rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            text = path.read_text(encoding="utf-8", errors="replace").lower()
            for word in BANNED:
                if word in text:
                    hits.append("%s（%s）" % (path.relative_to(ROOT), word))
    if hits:
        fail("宿主源码出现网络栈 API：%s" % ", ".join(hits[:5]))
    else:
        ok("宿主源码没有 winhttp / InternetOpen / URLDownloadToFile")

    header = ROOT / "src" / "shell" / "release_page.h"
    if not header.is_file():
        fail("缺 src/shell/release_page.h（发布页 URL 唯一来源）")
    else:
        text = header.read_text(encoding="utf-8")
        if EXPECTED in text:
            ok("发布页 URL 固定在 release_page.h")
        else:
            fail("release_page.h 的 URL 不是约定的发布页")

    hub = ROOT / "src" / "shell" / "hub.cpp"
    body = hub.read_text(encoding="utf-8") if hub.is_file() else ""
    if "查看最新版本（打开发布页）" in body and "QDesktopServices::openUrl" in body:
        ok("菜单项「查看最新版本（打开发布页）」只调 QDesktopServices::openUrl")
    else:
        fail("菜单项缺失或没有走 QDesktopServices::openUrl")
    if re.search(r"check(Update|_update)|检查更新", body):
        fail("hub.cpp 出现「检查更新」口径（会误导用户以为程序在比对版本）")
    else:
        ok("没有「检查更新」这类误导文案")

    if PROBLEMS:
        print("HOST-OFFLINE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("HOST-OFFLINE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
