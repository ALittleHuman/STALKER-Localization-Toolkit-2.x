# -*- coding: utf-8 -*-
"""门面**三份事实必须同名同数**核对器（声明 / 登记 / 判定三点闭环）。

为什么要它（P16-0，2026-10-08）
--------------------------------
`copy_text` 这一个能力：头字段在（`widget_facade.h:110`）、Qt 实现在
（`src/shell/widget_facade_qt.cpp`）、pybind 名在（`py_host.cpp:2899`）、
能力表在（`check_ui_capability_table.py` 的 EXPECT 名单）——**四处齐全**，
**唯独漏了 `kMethodNames` 与 `has_impl`**。后果是一条完整的静默链：

    `ui_facade_has("copy_text")` → `has_impl` 里没有这条分支 → 恒 false
    → `PyUi::copy_text` 走 `available()` 拿不到门面（`py_host.cpp:2070-2076`）
    → `api.ui.copy_text(...)` **恒返回 False**
    → 而 `plugins/xml_page.py:296` 不看返回值、紧接着显示"已复制 ID：…"
    ⇒ 用户点了没反应，还被**告知成功**（违反零静默红线）。

两把现成的锁都判不出这一类：
  · `check_ui_capability_table.py` 只看 `dir(xrkit.Ui)`（名字集合，来自 `.def`）；
  · `check_facade_order.py` 只比"结构体 vs 聚合初始化"的**顺序**。
⇒ 缺的正是"**结构体 ↔ kMethodNames ↔ has_impl** 三点闭合成环"这一步。

判据（逐门面三条）
------------------
① 结构体字段集合 == `kMethodNames` 集合（**双向**：字段有而名单没有 ⇒ 该能力恒不可用，
   正是 `copy_text` 的形状；名单有而字段没有 ⇒ 悬空名）；
② 结构体字段集合 == `has_impl` 分支集合（**双向**：少一条分支 ⇒ `has` 恒 false）；
③ 计数**下限**（空集不算通过；Ui≥90 / Engine≥70 / Host≥5）。

输出与退出码
------------
    `FACADE-REG ok field=93/93 kmethods=93/93 hasimpl=93/93 三门面三点闭环`
    `FACADE-REG fail <n> 项不成立（见上）`
退出码：`0`=绿；`1`=有断言不成立（含"仓库自带文件缺失"——那是 FAIL，不是 SKIP）。

自带反例自证（`selfcheck()`）
----------------------------
用合成文本喂同一个 `audit()`，**五种坏形状必须各自报红**（登记少一项 / 登记多一项 /
分支少一条 / 分支多一条 / 低于计数下限），**一份好形状必须报绿**。
没有这一段，本锁本身也可能是"恒绿"的假判据。
"""

import sys
from pathlib import Path

# ★ Windows 控制台默认 GBK：判据里的 ✗/✓ 打不出来就会**自己崩**（那不是"有错"，是脚本错）
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

sys.path.insert(0, str(Path(__file__).resolve().parent))

import _facade_tables as ft  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def audit(header_text, impl_text, struct_name, min_fields):
    """返回 (problems, counts)。problems 为空 = 这份门面三点闭环。"""
    fields = ft.struct_function_fields(header_text, struct_name)
    kmethods = ft.k_method_names(impl_text)
    hasimpl = ft.has_impl_names(impl_text)
    counts = {"field": len(fields), "kmethods": len(kmethods), "hasimpl": len(hasimpl)}
    problems = []

    if not fields:
        problems.append("结构体 `%s` 一个 `std::function` 字段都没解析到（解析器与源码脱节？）"
                        % struct_name)
    if len(fields) < min_fields:
        problems.append("结构体字段只有 %d 项，低于下限 %d（空集不算通过）"
                        % (len(fields), min_fields))

    field_set, k_set, h_set = set(fields), set(kmethods), set(hasimpl)

    only_field_k = [n for n in fields if n not in k_set]
    if only_field_k:
        problems.append("字段有、kMethodNames **没有**（⇒ 该能力恒不可用）：%s" % only_field_k)
    only_k = [n for n in kmethods if n not in field_set]
    if only_k:
        problems.append("kMethodNames 多出、结构体里**没有**（⇒ 悬空名）：%s" % only_k)

    only_field_h = [n for n in fields if n not in h_set]
    if only_field_h:
        problems.append("字段有、has_impl **没有分支**（⇒ `*_facade_has` 恒 false）：%s"
                        % only_field_h)
    only_h = [n for n in hasimpl if n not in field_set]
    if only_h:
        problems.append("has_impl 多出、结构体里**没有**（⇒ 分支指向不存在的字段）：%s" % only_h)
    return problems, counts


# ── 反例自证 ────────────────────────────────────────────────────────────
_SYN_HEADER = """
struct SynFacade {
    std::function<void()> alpha;
    std::function<void()> beta;
};
"""

_SYN_OK = """
const char* const kMethodNames[] = { "alpha", "beta" };
bool has_impl(const SynFacade& f, const std::string& name) {
    if (name == "alpha") return static_cast<bool>(f.alpha);
    if (name == "beta") return static_cast<bool>(f.beta);
    return false;
}
"""


def _syn_impl(kmethods, branches):
    ks = ", ".join('"%s"' % n for n in kmethods)
    bs = "".join('    if (name == "%s") return static_cast<bool>(f.%s);\n' % (n, n)
                 for n in branches)
    return ("const char* const kMethodNames[] = { %s };\n"
            "bool has_impl(const SynFacade& f, const std::string& name) {\n"
            "%s    return false;\n}\n" % (ks, bs))


def selfcheck():
    """五种坏形状必须报红、一份好形状必须报绿。返回 (ok, 逐条结果列表)。"""
    cases = [
        ("好形状（alpha+beta 齐全）", _SYN_HEADER, _SYN_OK, 2, False),
        ("登记少一项（beta 漏登记）", _SYN_HEADER, _syn_impl(["alpha"], ["alpha", "beta"]), 2, True),
        ("登记多一项（gamma 悬空）", _SYN_HEADER, _syn_impl(["alpha", "beta", "gamma"],
                                                     ["alpha", "beta"]), 2, True),
        ("分支少一条（beta 无分支）", _SYN_HEADER, _syn_impl(["alpha", "beta"], ["alpha"]), 2, True),
        ("分支多一条（gamma 无字段）", _SYN_HEADER, _syn_impl(["alpha", "beta"],
                                                     ["alpha", "beta", "gamma"]), 2, True),
        ("低于计数下限（下限 3，只有 2 项）", _SYN_HEADER, _SYN_OK, 3, True),
    ]
    results = []
    ok = True
    for label, header, impl, min_fields, want_red in cases:
        problems, _counts = audit(header, impl, "SynFacade", min_fields)
        got_red = bool(problems)
        good = (got_red == want_red)
        ok &= good
        results.append((label, want_red, got_red, good, problems))
    return ok, results


def main() -> int:
    print("== 门面三份事实（声明 / kMethodNames / has_impl）三点闭环核对器 ==")
    print()
    print("-- 反例自证（五种坏形状必须报红）--")
    self_ok, results = selfcheck()
    for label, want_red, got_red, good, problems in results:
        print("   %s %-34s 期望=%s 实得=%s"
              % ("✓" if good else "✗", label,
                 "红" if want_red else "绿", "红" if got_red else "绿"))
        if not good:
            for p in problems:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("FACADE-REG fail 反例自证不通过（本锁自己不可信，先修它）")
        return 1
    print("   ✓ 反例自证 6/6（坏形状全红、好形状全绿）")
    print()

    facades = [
        ("Ui", "UiFacade",
         ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h",
         ROOT / "src" / "engine" / "plugin" / "widget_facade.cpp", 90),
        ("Engine", "EngineFacade",
         ROOT / "include" / "xrkit" / "engine" / "plugin" / "engine_facade.h",
         ROOT / "src" / "engine" / "plugin" / "engine_facade.cpp", 70),
        ("Host", "HostFacade",
         ROOT / "include" / "xrkit" / "engine" / "plugin" / "host_facade.h",
         ROOT / "src" / "engine" / "plugin" / "host_facade.cpp", 5),
    ]

    print("-- 三门面三点闭环 --")
    all_problems = 0
    summary = []
    for label, struct_name, header, impl, min_fields in facades:
        if not header.exists() or not impl.exists():
            # 仓库自带文件缺失 = FAIL（照本仓纪律：绝不 SKIP）
            print("   ✗ %-7s 仓库自带文件缺失：%s / %s" % (label, header.name, impl.name))
            all_problems += 1
            summary.append("%s=缺失" % label)
            continue
        problems, counts = audit(header.read_text(encoding="utf-8"),
                                 impl.read_text(encoding="utf-8"), struct_name, min_fields)
        print("   == %-7s %s（%s）  field=%d kmethods=%d hasimpl=%d"
              % (label, struct_name, impl.name, counts["field"], counts["kmethods"],
                 counts["hasimpl"]))
        if problems:
            for p in problems:
                print("      ✗ %s" % p)
            all_problems += len(problems)
        else:
            print("      ✓ 三点闭环")
        summary.append("%s=%d/%d/%d" % (label, counts["field"], counts["kmethods"],
                                       counts["hasimpl"]))
    print()
    if all_problems:
        print("FACADE-REG fail %d 项不成立（%s）" % (all_problems, " ".join(summary)))
        return 1
    print("FACADE-REG ok field=kmethods=hasimpl 三点闭环（%s）" % " ".join(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
