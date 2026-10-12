# -*- coding: utf-8 -*-
"""P17-98 判据：插件日志缓冲的**线程安全接线** ＋ 注册表的**线程约定字据**（审计 `05 PLG-08`）。

背景（事实）
------------
审计原文：**注册表与日志缓冲缺并发保护**。实查结论（分两半，处置也不同）：

* **日志缓冲（真缺陷）**：`api.log()`（`PyPluginApi::log`）是**插件可以在工作线程里调**的入口
  （本仓的页面/任务回调就有跑在工作线程的），而缓冲的"读+清空"侧
  `take_python_log_lines()` 在主线程（插件加载之后 / 任务收尾）。
  修前它是一个**裸 `std::vector&`**：`push_back`（可能扩容、可能 `erase(begin())`）撞上 `swap`
  ⇒ **数据竞争 + 可能读悬垂**。⇒ 现在写走 `append_plugin_log_line`（带锁 + 200 行上限），
  读+清空走 `take_python_log_lines`（同一把锁里 swap 再返回）；**两个函数以外不许碰那个 vector**。
* **注册表（不是缺陷，但缺字据）**：写只发生在主线程（`Hub::load_plugins()` → 加载全部插件），
  读可以被工作线程问（`menu_items_for` / `panels_for` / `invoke`）——"**发布后只读**"那一档是安全的。
  但它**一个字都没写**，后人只能读代码猜 ⇒ 本批把约定**写成字据**（含"将来要在工作线程写就得先加锁"）。

判据（两组，都要有牙）
----------------------
① **日志缓冲**：`py_host.cpp` 里
   · 必须存在 `append_plugin_log_line`（**带锁**：函数体里出现 `lock_guard`）与
     `take_python_log_lines`（**同一把锁**里 swap）；
   · **不许**再出现"裸访问器"形状 —— `std::vector<std::string>& plugin_log_buffer()` 必须已消失；
   · `PyPluginApi::log` 里**不许**再直接 `push_back`/`erase` 那个 vector。
② **注册表字据**：`registry.h` 的 `ContextValue`/`when` 那段之后，必须写明线程约定三件事：
   「**写只发生在主线程**」「**读可以在工作线程**（发布后只读）」「**将来要在工作线程写就先加锁**」。

输出：`PLG-THREADSAFE ok …` / `PLG-THREADSAFE fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
PY_HOST = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
REGISTRY_H = ROOT / "include" / "xrkit" / "engine" / "plugin" / "registry.h"
COMPARE_H = ROOT / "include" / "xrkit" / "engine" / "xml" / "compare.h"
ABI_HOST = ROOT / "src" / "engine" / "plugin" / "abi_host.cpp"
DESIGN_DOC = ROOT / "docs" / "2.0.0-设计口径.md"


def audit_sink_contract(compare_h: str, abi_host: str, design_doc: str) -> list:
    """③ ★★ P17-99（审计 `05 PLG-04`）：ABI 明文列的 sink 键不许被静默丢；`ids_a`/`ids_b` 不许恒 0。

    * `ids_a`/`ids_b` 必须**从插件真报上来的 ID 数出来**（不能再取 `id_text_*` —— 那是文本模式的表，
      C ABI 的 sink 没有任何键写它 ⇒ 恒 0）；
    * 宿主不认识的键必须**记进 `unhandled_keys`**（零静默）；
    * 结果结构里必须有 `unhandled_keys` 这个字段；
    * **口径必须落在字据上**（`docs/2.0.0-设计口径.md` §4.5）：`row_kind` 不落位 / 但不许静默丢 /
      ID 数按插件报的算 —— 三句都在。
    """
    problems = []
    abi = strip_comments(abi_host)
    cmp = strip_comments(compare_h)
    if "unhandled_keys" not in cmp:
        problems.append("compare.h：`XmlCompareResult` 里没有 `unhandled_keys` 字段（不认识的键无处记）")
    if "unhandled_keys.push_back" not in abi:
        problems.append("abi_host.cpp：不认识的 sink 键没有记进 `unhandled_keys`（静默丢）")
    if "id_text_a.size()" in abi or "id_text_b.size()" in abi:
        problems.append("abi_host.cpp：`ids_a`/`ids_b` 又取回 `id_text_*.size()` 了"
                        "（那是**文本模式**的表，C ABI 的 sink 不写它 ⇒ 插件模式下恒 0）")
    if "ids_a_seen" not in abi or "ids_b_seen" not in abi:
        problems.append("abi_host.cpp：`ids_a`/`ids_b` 不是从插件报上来的 ID 里数的")
    if "### 4.5" not in design_doc or "row_kind" not in design_doc:
        problems.append("docs/2.0.0-设计口径.md：缺 §4.5（sink 键的认/不认口径）")
    else:
        at = design_doc.find("### 4.5")
        window = design_doc[at:at + 2400]
        for needle, why in (
                ("不落位", "没写「`row_kind` 不落位」"),
                ("静默丢", "没写「不许静默丢」"),
                ("真报上来的 ID 数", "没写「ID 数按插件真报上来的算」"),
        ):
            if needle not in window:
                problems.append("docs/2.0.0-设计口径.md §4.5：%s" % why)
    return problems


def strip_comments(text: str) -> str:
    """查**代码**时先剥注释（否则说明里引用旧代码会把自己判红 —— 本仓踩过三次）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(code: str, signature: str) -> str:
    """取**这个函数的体**（从签名后的 `{` 到配对的 `}`）—— 不能用"往后 600 字"当窗口：
    下一个函数里的 `lock_guard` 会漏进来（本判据第一版就是这么假绿的）。"""
    at = code.find(signature)
    if at < 0:
        return ""
    start = code.find("{", at)
    if start < 0:
        return ""
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[start:i + 1]
    return code[start:]


def audit_log_buffer(py_host: str) -> list:
    problems = []
    code = strip_comments(py_host)
    if "plugin_log_buffer" in code:
        problems.append("py_host.cpp：裸访问器 `plugin_log_buffer()` 又回来了 "
                        "（那就等于把 vector 直接交给两个线程）")
    write_body = function_body(code, "void append_plugin_log_line")
    if not write_body:
        problems.append("py_host.cpp：找不到 `append_plugin_log_line` 的定义")
    else:
        if "lock_guard" not in write_body:
            problems.append("py_host.cpp：`append_plugin_log_line` 里没有 `lock_guard`（写侧没锁）")
        if "erase" not in write_body:
            problems.append("py_host.cpp：`append_plugin_log_line` 里没有上限处理（`erase`）")
    read_body = function_body(code, "std::vector<std::string> take_python_log_lines")
    if not read_body:
        problems.append("py_host.cpp：找不到 `take_python_log_lines`")
    else:
        if "lock_guard" not in read_body or "swap" not in read_body:
            problems.append("py_host.cpp：`take_python_log_lines` 没在同一把锁里 `swap`（读侧没锁）")
    # `PyPluginApi::log` 里不许再直接动那个 vector
    log_body = function_body(code, "void log(const std::string& message) const")
    if not log_body:
        problems.append("py_host.cpp：找不到 `PyPluginApi::log`")
    else:
        if "push_back" in log_body or "erase" in log_body:
            problems.append("py_host.cpp：`api.log` 里还在直接 `push_back`/`erase`（没走带锁入口）")
        if "append_plugin_log_line" not in log_body:
            problems.append("py_host.cpp：`api.log` 没走 `append_plugin_log_line`")
    return problems


def audit_registry_contract(registry_h: str) -> list:
    problems = []
    at = registry_h.find("using ContextValue")
    window = registry_h[at:at + 3000] if at >= 0 else ""
    for needle, why in (
            ("写（注册/注销/回滚）只发生在主线程", "没写「写只发生在主线程」"),
            ("读可以在工作线程", "没写「读可以在工作线程（发布后只读）」"),
            ("先给注册表加锁", "没写「将来要在工作线程写，就先加锁并同步改这条注释」"),
    ):
        if needle not in window:
            problems.append("registry.h：线程约定缺一句 —— %s" % why)
    return problems


def selfcheck() -> tuple:
    good_host = (
        "void append_plugin_log_line(const std::string& line) {\n"
        "  std::lock_guard<std::mutex> lock(m);\n"
        "  b.push_back(line); if (b.size() > 200) b.erase(b.begin());\n}\n"
        "std::vector<std::string> take_python_log_lines() {\n"
        "  std::vector<std::string> out;\n"
        "  std::lock_guard<std::mutex> lock(m); out.swap(b); return out;\n}\n"
        "void log(const std::string& message) const {\n  append_plugin_log_line(line);\n}\n")
    good_reg = ("using ContextValue = std::optional<std::string>;\n"
                "// 写（注册/注销/回滚）只发生在主线程；读可以在工作线程（发布后只读）；\n"
                "// 将来要在工作线程写，就先给注册表加锁并同步改这条注释。\n")
    cases = [
        ("好形状", 0, audit_log_buffer(good_host) + audit_registry_contract(good_reg)
         + audit_sink_contract("struct X { std::vector<std::string> unhandled_keys; };",
                               "target->unhandled_keys.push_back(k); ids_a_seen.insert(x); ids_b_seen.insert(y);",
                               "### 4.5 x\nrow_kind 不落位；不许静默丢；按真报上来的 ID 数\n")),
        ("IDS 取回 id_text_*（修前形状）", 1,
         audit_sink_contract("struct X { std::vector<std::string> unhandled_keys; };",
                             "result.ids_a = result.id_text_a.size(); ids_a_seen.insert(x); ids_b_seen.insert(y);",
                             "### 4.5 x\nrow_kind 不落位；不许静默丢；按真报上来的 ID 数\n")),
        ("不认识的键静默丢（修前形状）", 1,
         audit_sink_contract("struct X { std::vector<std::string> unhandled_keys; };",
                             "ids_a_seen.insert(x); ids_b_seen.insert(y);",
                             "### 4.5 x\nrow_kind 不落位；不许静默丢；按真报上来的 ID 数\n")),
        ("口径没落字据", 1,
         audit_sink_contract("struct X { std::vector<std::string> unhandled_keys; };",
                             "target->unhandled_keys.push_back(k); ids_a_seen.insert(x); ids_b_seen.insert(y);",
                             "（没有 §4.5）\n")),
        ("裸访问器又回来了（修前形状）", 1,
         audit_log_buffer(good_host.replace("void append_plugin_log_line",
                                            "std::vector<std::string>& plugin_log_buffer() { return b; }\n"
                                            "void append_plugin_log_line"))),
        # ★ 反例要**只**破坏写侧那一处：用独立字符串拼（早先版本用 `replace` 去"没有 lock_guard 的版本"，
        #   替不中 ⇒ 得到的是"好形状" ⇒ 假红证。这是本判据自己的算术，必须写清）。
        ("写侧没锁", 1, audit_log_buffer(
            "void append_plugin_log_line(const std::string& line) {\n"
            "  b.push_back(line); if (b.size() > 200) b.erase(b.begin());\n}\n"
            "std::vector<std::string> take_python_log_lines() {\n"
            "  std::vector<std::string> out;\n"
            "  std::lock_guard<std::mutex> lock(m); out.swap(b); return out;\n}\n"
            "void log(const std::string& message) const {\n  append_plugin_log_line(line);\n}\n")),
        ("api.log 绕过入口", 1,
         audit_log_buffer(good_host.replace("  append_plugin_log_line(line);",
                                            "  b.push_back(line); if (b.size() > 200) b.erase(b.begin());"))),
        ("注册表约定没落字据", 1,
         audit_registry_contract("using ContextValue = std::optional<std::string>;\n")),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good = (len(problems) > 0) if want else (not problems)
        ok &= good
        results.append((label, want, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-98/P17-99：插件日志缓冲的线程安全 ＋ 注册表线程约定 ＋ C ABI sink 零静默 ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-26s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                "有错" if want else "无错", got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("PLG-THREADSAFE fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (PY_HOST, REGISTRY_H, COMPARE_H, ABI_HOST, DESIGN_DOC):
        if not path.is_file():
            print("PLG-THREADSAFE fail 仓库自带文件缺失：%s" % path)
            return 1
    problems = audit_log_buffer(PY_HOST.read_text(encoding="utf-8", errors="replace"))
    problems += audit_registry_contract(REGISTRY_H.read_text(encoding="utf-8", errors="replace"))
    problems += audit_sink_contract(COMPARE_H.read_text(encoding="utf-8", errors="replace"),
                                   ABI_HOST.read_text(encoding="utf-8", errors="replace"),
                                   DESIGN_DOC.read_text(encoding="utf-8", errors="replace"))
    print("-- 实测 --")
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("PLG-THREADSAFE fail %d 项不成立" % len(problems))
        return 1
    print("   ✓ 日志缓冲：写走 `append_plugin_log_line`（锁 + 上限）、读+清空走 `take_python_log_lines`（同锁 swap）")
    print("   ✓ 裸访问器 `plugin_log_buffer()` 已消失；`api.log` 不再直接动那个 vector")
    print("   ✓ 注册表线程约定已成字据（写只在主线程／读可在工作线程／要并发写先加锁）")
    print("   ✓ C ABI sink：`ids_a`/`ids_b` 从插件报的 ID 数出来；不认识的键记进 `unhandled_keys`（零静默）")
    print("   ✓ 口径已落在 `docs/2.0.0-设计口径.md` §4.5（`row_kind` 不落位／不许静默丢／ID 数按插件报的算）")
    print()
    print("PLG-THREADSAFE ok 日志缓冲有锁、注册表有字据、ABI sink 无静默丢（审计 05 PLG-08 / 05 PLG-04）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
