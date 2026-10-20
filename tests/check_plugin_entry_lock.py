#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""插件加载**入口唯一性**静态锁（P2b；决策 1「C ABI 退净」之后的新形态）。

口径（用户红线「GUI 的功能，CLI 肯定要全部拥有」+ 规格"只许一份实现"）：
  ① 两个前端（`src/shell/hub.cpp` / `tools/cli/main.cpp`）都必须走**同一个**统一入口
     `load_all_plugins_from_dir(...)`；
  ② `load_python_plugins_from_dir`（真正的加载实现）**只许**由统一入口自己
     （`src/engine/plugin/loader.cpp`）与宿主探针（`src/engine/plugin/py_host.cpp`）调用 ——
     别处再调一次就是"第二条加载路径"；
  ③ ★★ 决策 1（用户 2026-10-18/19）**新增**：C ABI 那套符号必须真的不在默认构建里 ——
     全仓代码（去注释后）不许再出现 `load_plugins_from_dir` / `is_plugin_dll_name` /
     `unload_all` / `loaded_count` / `PluginApiAdapter` / `stk_api` / `STK_PLUGIN_ENTRY`。
     （`abi.h`、`abi_host.*`、`contract.*`、`plugin_ui.*` 之外的东西都已删；
      这条锁防的是"有人又把它加回来"。）

为什么必须静态锁：这类分叉**测不出来**（两边各自跑各自的用例都绿），只有"调用点在哪"能判。

判据按**每一处**报 `文件:行号`（不写"文件里出现过"），去注释后判、保留行号。

输出：`PLUGINENTRY ok` / `PLUGINENTRY fail <详情>`；退出码 0/1。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from _cpp_source import strip_comments  # noqa: E402

FRONTS = {
    "src/shell/hub.cpp": "Unity",
    "tools/cli/main.cpp": "CLI",
}
UNIFIED = "load_all_plugins_from_dir"
PY_ONLY = "load_python_plugins_from_dir"
UNIFIED_OWNER = "src/engine/plugin/loader.cpp"
PY_OWNER = "src/engine/plugin/py_host.cpp"
# C ABI 退净后**不许再出现**的符号（决策 1 的验收项）
ABI_BANNED = ["load_plugins_from_dir", "is_plugin_dll_name", "unload_all", "loaded_count",
              "PluginApiAdapter", "stk_api", "STK_PLUGIN_ENTRY", "stk_plugin_init"]
SCANNED = list(FRONTS) + [UNIFIED_OWNER, PY_OWNER]

FAILS: list[str] = []


def _count(text: str, name: str) -> list:
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    return [text.count("\n", 0, m.start()) + 1 for m in pattern.finditer(text)]


def _code(rel: str) -> str:
    return strip_comments((ROOT / rel).read_text(encoding="utf-8", errors="replace"))


def check_fronts() -> None:
    for rel, tag in FRONTS.items():
        code = _code(rel)
        n = _count(code, UNIFIED)
        if not n:
            FAILS.append("%s（%s）没走统一入口 `%s`" % (rel, tag, UNIFIED))
        for ln in _count(code, PY_ONLY):
            FAILS.append("%s:%d（%s）直接调 `%s` ⇒ 绕过了统一入口" % (rel, ln, tag, PY_ONLY))


def check_owner() -> None:
    for rel in (UNIFIED_OWNER, PY_OWNER):
        code = _code(rel)
        if not _count(code, PY_ONLY):
            FAILS.append("%s 里没有 `%s`（加载实现不见了？）" % (rel, PY_ONLY))


def check_unified_body() -> None:
    code = _code(UNIFIED_OWNER)
    pattern = re.compile(r"std::vector<RegistrationError>\s+" + UNIFIED + r"\s*\([^)]*\)\s*\{")
    m = pattern.search(code)
    if not m:
        FAILS.append("%s 里找不到 `%s` 的定义" % (UNIFIED_OWNER, UNIFIED))
        return
    depth, end = 0, len(code)
    for i in range(m.end() - 1, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = code[m.end():end]
    if PY_ONLY not in body:
        FAILS.append("`%s` 的函数体里没有 `%s`（唯一入口没接上实现）" % (UNIFIED, PY_ONLY))


def check_abi_gone() -> None:
    """全仓（src/tools/include）代码里不许再有 C ABI 符号。"""
    hits = []
    for base in ("src", "tools/cli", "include"):
        for p in sorted((ROOT / base).rglob("*")):
            if p.suffix.lower() not in (".cpp", ".h", ".hpp", ".cc", ".c"):
                continue
            try:
                code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            for name in ABI_BANNED:
                if re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", code):
                    hits.append("%s: %s" % (p.relative_to(ROOT).as_posix(), name))
    if hits:
        FAILS.append("C ABI 符号又出现了（决策 1 要求默认构建里不存在）："
                     + "；".join(hits[:8]))


def main() -> int:
    print("== 插件加载入口唯一性 ＋ C ABI 退净（决策 1）==")
    check_fronts()
    check_owner()
    check_unified_body()
    check_abi_gone()
    if FAILS:
        print("PLUGINENTRY fail %d 项：" % len(FAILS))
        for item in FAILS:
            print("  · " + item)
        return 1
    print("PLUGINENTRY ok 两端只走统一入口、实现只此一处、C ABI 符号全仓零命中")
    return 0


if __name__ == "__main__":
    sys.exit(main())
