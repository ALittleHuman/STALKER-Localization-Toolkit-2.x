# -*- coding: utf-8 -*-
"""P17-90 静态判据：`api.ui.*` 的**参数名**必须与契约表对得上（对不上的要**显式记账**）。

背景（为什么这条判据必须存在）
------------------------------
`src/engine/plugin/contract_tables.inc` 里 `kUiMethods[]` 的注释写得很清楚：
「`api.ui` 的契约：名字 + **参数名**（**第三方就是照这两样写的**）」。

而修前实测：16 条方法里 **10 条**的 Python 绑定参数名与契约表**对不上**（例：契约 `entry(parent,
textvariable, width, show)`，绑定是 `entry(parent, initial, width, password)`）。功能大多等价，
但**按契约写的第三方插件会直接 `TypeError`** —— 这正是"假缺口真存在"：
门禁里已有的 `abi-closure` 只比**方法名与条数**，**参数名一个都没对过账**，所以漂了这么久没人发现。

判据（三组，都要有牙）
----------------------
① **契约里点名的参数，绑定必须收**（名字逐字相同）—— 这是硬红线（"第三方照这两样写"）。
   例：`entry` 必须收 `textvariable` 与 `show`。
② **契约没点名而绑定多给的参数**（2.x 富余项，如 `label(style)` 之外的东西）**允许**，
   但**必须出现在本文件的 `EXTRA` 表里并写明理由** —— 防"悄悄加参数"（门面表是棘轮，不是随手加）。
③ **`DEFERRED`（契约点了名、绑定还没收）必须逐条列在下面并写明"为什么还没收"**，
   且**数量与内容被本判据锁住**：谁把某条补上了 ⇒ 这里会红（提醒他把该条从 DEFERRED 删掉，保持账实相符）；
   谁把某条悄悄删了 ⇒ 也会红（提醒他那是**欠账**，不是可以忘掉的事）。
   ★ 这就是用户 2026-10-09 那条纪律的落地形状：「**字段/契约先运到，判据跟着一起落**」——
   欠账可以被看见、被点数，但**不许消失**。

输出：`UI-CONTRACT ok …` / `UI-CONTRACT fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证（好形状 / 少了参数 / 悄悄多参数 / 欠账没记账 / 欠账已补上却没销账）。
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
CONTRACT = ROOT / "src" / "engine" / "plugin" / "contract_tables.inc"
PY_HOST = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
ABI_HOST = ROOT / "src" / "engine" / "plugin" / "abi_host.cpp"

# ── ② 绑定**多给**的参数（契约没点名）；2.x 富余项，必须写清为什么 ──────────────
#     形状：方法名 → {参数名: 理由}
EXTRA = {
    "button": {"enabled": "★ 契约写 `state`，2.x 这条叫 `enabled`（bool）——**同一件事两个名字**，"
                          "已在 DEFERRED 里记成欠账（要定 state 的取值口径才能改名/改类型）"},
    "pack": {"parent": "★ 契约写 `widget`（欠账已在 DEFERRED 里记账）；2.x 这条叫 `parent`",
             "child": "2.x 的第二个形参（要打包的子控件）；契约把它含在 `widget` 那一个名字里"},
    "set_enabled": {"widget": "★ 这条就是契约 `set_state` 的 2.x 名字（欠账已在 DEFERRED 里记账）",
                    "enabled": "同上（`state` ↔ `enabled`）"},
    "entry_on_change": {"widget": "2.x 新增（P15-0 边打边筛），不在 16 方法的契约面里",
                        "callback": "同上"},
    "progress": {"parent": "2.x 新增（P12c 进度条），契约表不含它"},
    "progress_value": {"widget": "同上"},
    "set_progress": {"widget": "同上", "percent": "同上"},
    "get_text": {"widget": "2.x 新增（读回控件文本；契约面只有 set_text）"},
    "combo_value": {"widget": "同上"},
    "checkbox_state": {"widget": "同上"},
    "set_checked": {"widget": "同上", "checked": "同上"},
    "combo_set_items": {"widget": "同上", "values": "同上"},
    "set_enabled": {"widget": "同上", "enabled": "同上"},
    "log": {"line": "2.x 新增（宿主日志口）"},
}

# ── ③ 契约点名了、绑定**还没收**的参数（**欠账**，逐条写清为什么）──────────────
#     ★ 这张表是**判据锁住的账本**：补上一条 ⇒ 这里必须删一条；删了不补 ⇒ 红。
DEFERRED = {
    "button": {"state": "契约写 state；绑定/门面/C ABI 都是 **bool `enabled`**（类型都不同）"
                        "⇒ 要对齐得先定 state 的取值口径（int? 'normal'/'disabled'?），"
                        "属**接口口径**问题、不是改个名 ⇒ 留给接口批单独一次"},
    "checkbox": {"variable": "1.x 风格的变量绑定（`variable=<var>` 双向绑定）；2.x 走的是"
                             "`on_toggle` 回调。要补得**同时**定「变量对象是什么」（新类型？）"
                             "⇒ 属新增能力、要用户拍板，**不许**先落一个不生效的字段"},
    "combo": {"variable": "同上（`variable` 与 `on_change` 是两种风格）",
              "width": "C ABI 的 `combo` 槽位里**没有 width**（见 `abi.h` 第 7 条）"
                       "⇒ 要补得动 ABI（用户口径：能不动就不动）"},
    # ★★ P17-91 **已销账**：`drop_box.width` 原来记在这里（"可补、门面硬写 0"），
    #   本批把宽度从绑定 → 门面 → C ABI 槽位串通 ⇒ 这条欠账**从账本里删掉**（账实要相符）。
    "pack": {"widget": "契约写 `widget`；绑定/门面/C ABI 都叫 `parent`（同一件事两个名字）"
                       "⇒ 改名会牵动 C ABI 与 Qt 门面，**随着下次门面改动一起做**",
             "anchor": "契约写 `anchor`；C ABI 的 `pack` 只有 7 个形参、**没有 anchor**（`abi.h` 第 9 条）"
                       "⇒ 要补得动 ABI（用户口径：能不动就不动）"},
    "set_state": {"__whole_method__": "契约写 `set_state(widget, state)`；2.x 这条 Python 名是"
                                      "`set_enabled(widget, enabled)`（同一件事、两个名字，"
                                      "C ABI 侧就叫 `set_state`）⇒ 与 `button.state` 同一次改名一起做"},
    "ask_yes_no": {"parent": "契约写 `parent`，绑定没有（其余参数一致）⇒ 补一个用不上的形参＝"
                             "制造「字段先落没人判」的假绿，**故意不补**：留待定「parent 到底用来干什么」"},
    "ask_open_file": {"parent": "同上"},
    "ask_save_file": {"parent": "同上"},
}


def strip_comments(text: str) -> str:
    """查**代码**时先剥注释（本仓踩过：判据说明里引用旧代码 ⇒ 在自己身上假绿）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_contract(text: str) -> list:
    block = text[text.find("kUiMethods[] = {"):]
    block = block[:block.find("};")]
    out = []
    for match in re.finditer(r'\{"([a-z_]+)",\s*"([^"]*)"\}', block):
        out.append((match.group(1), [p.strip() for p in match.group(2).split(",")]))
    return out


def parse_bindings(text: str) -> dict:
    """从 `.def("<name>", … py::arg("a") … )` 里读**绑定收下的参数名**（按声明顺序）。"""
    defs = [(m.start(), m.group(1)) for m in re.finditer(r'\.def\("([a-z_]+)"', text)]
    defs.sort()
    out = {}
    for index, (pos, name) in enumerate(defs):
        end = defs[index + 1][0] if index + 1 < len(defs) else len(text)
        seg = text[pos:end]
        out[name] = re.findall(r'py::arg\("([^"]+)"\)', seg)
    return out


def audit(contract: list, bindings: dict, extra: dict, deferred: dict) -> list:
    problems = []
    for name, params in contract:
        bound = bindings.get(name)
        if bound is None:
            # ★ 整条方法在 2.x 里换了名字（例：契约 `set_state` ↔ 2.x `set_enabled`）：
            #   允许，但必须在 DEFERRED 里用 `__whole_method__` 写明"它现在叫什么、为什么"。
            if "__whole_method__" in deferred.get(name, {}):
                continue
            problems.append("%s：契约里有、Python 绑定里没有" % name)
            continue
        lack = [p for p in params if p not in bound]
        lack = [p for p in lack if p not in deferred.get(name, {})]
        if lack:
            problems.append("%s：契约点名而绑定没收 → %s（要么补上，要么写进 DEFERRED 并说明理由）"
                            % (name, "、".join(lack)))
        surplus = [p for p in bound if p not in params]
        surplus = [p for p in surplus if p not in extra.get(name, {})]
        if surplus:
            problems.append("%s：绑定多给了契约没点名的参数 → %s（要么删掉，要么写进 EXTRA 并说明理由）"
                            % (name, "、".join(surplus)))
        # ③ 账实相符：DEFERRED 里记着的，必须**仍然**是"绑定确实没收"的
        for param, _why in deferred.get(name, {}).items():
            if param in bound:
                problems.append("%s.%s：DEFERRED 里还记着这条欠账，但绑定**已经收了** "
                                "⇒ 请把这条从 DEFERRED 里删掉（账实要相符）" % (name, param))
        if name not in [n for n, _ in contract] and extra.get(name):
            problems.append("%s：EXTRA 里记着一个契约表里没有的方法" % name)
    return problems


def audit_abi_parent(abi_text: str) -> list:
    """★★ P17-93（审计 `05 PLG-02`）：**C ABI 的 builder 必须收到宿主给的 parent**。

    修前形状：`abi_host.cpp` 里两个适配 lambda 写死 `builder(user, nullptr)` —— 形参 `parent`
    **收下就丢**，C ABI 插件永远拿不到根容器（而宿主侧 `hub.cpp:678/783` 早把它传下来了）。
    这条**属于本判据的主题**（"契约点名的参数/位置，实现必须真的用起来"），所以并进这里。
    判据：两条适配（`on_panel` / `on_tool`）里都**不许**出现 `builder(user, nullptr)`，
    且都出现 `builder(user, reinterpret_cast<stk_widget*>(parent))`（原样透传）。
    """
    problems = []
    code = strip_comments(abi_text)
    if "builder(user, nullptr)" in code:
        problems.append("abi_host.cpp：C ABI 的 builder 又被写死 `builder(user, nullptr)` "
                        "⇒ 插件拿不到宿主 parent（05 PLG-02 复发）")
    got = code.count("builder(user, reinterpret_cast<stk_widget*>(parent))")
    if got != 2:
        problems.append("abi_host.cpp：`builder(user, …(parent))` 只找到 %d 处（`on_panel` 与 "
                        "`on_tool` 两条都要透传）" % got)
    return problems


def selfcheck() -> tuple:
    good_contract = [("entry", ["parent", "textvariable", "width", "show"]),
                     ("label", ["parent", "text", "style"])]
    good_bindings = {"entry": ["parent", "textvariable", "width", "show"],
                     "label": ["parent", "text", "style"]}
    cases = [
        ("好形状", 0, audit(good_contract, good_bindings, {}, {})),
        ("契约点名而绑定没收（修前形状）", 1,
         audit(good_contract, {"entry": ["parent", "initial", "width", "password"],
                               "label": ["parent", "text", "style"]}, {}, {})),
        ("绑定悄悄多给参数", 1,
         audit(good_contract, {"entry": ["parent", "textvariable", "width", "show", "extra1"],
                               "label": ["parent", "text", "style"]}, {}, {})),
        ("欠账没记账", 1,
         audit(good_contract, {"entry": ["parent", "textvariable"], "label": ["parent", "text", "style"]},
               {}, {})),
        ("欠账已补上却没销账", 1,
         audit(good_contract, good_bindings, {}, {"entry": {"textvariable": "早该补了"}})),
        ("记账之后照旧绿", 0,
         audit(good_contract, {"entry": ["parent", "width"], "label": ["parent", "text", "style"]},
               {}, {"entry": {"textvariable": "欠账", "show": "欠账"}})),
        ("C ABI parent 透传（好形状）", 0, audit_abi_parent(
            "PanelBuilder wrapped = [builder, user](void* parent) -> void* {\n"
            "    return builder(user, reinterpret_cast<stk_widget*>(parent));\n};\n"
            "PanelBuilder wrapped2 = [builder, user](void* parent) -> void* {\n"
            "    return builder(user, reinterpret_cast<stk_widget*>(parent));\n};\n")),
        ("C ABI parent 被丢掉（修前形状）", 1, audit_abi_parent(
            "PanelBuilder wrapped = [builder, user](void* parent) -> void* {\n"
            "    return builder(user, nullptr);\n};\n")),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good = (len(problems) > 0) if want else (not problems)
        ok &= good
        results.append((label, want, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-90：`api.ui.*` 参数名 ↔ 契约表（`kUiMethods[]`）对账 ==")
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
        print("UI-CONTRACT fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (CONTRACT, PY_HOST, ABI_HOST):
        if not path.is_file():
            print("UI-CONTRACT fail 仓库自带文件缺失：%s" % path)
            return 1
    contract = parse_contract(CONTRACT.read_text(encoding="utf-8", errors="replace"))
    bindings = parse_bindings(PY_HOST.read_text(encoding="utf-8", errors="replace"))
    problems = audit(contract, bindings, EXTRA, DEFERRED)
    abi_problems = audit_abi_parent(ABI_HOST.read_text(encoding="utf-8", errors="replace"))
    if abi_problems:
        print("-- C ABI 的 parent 透传（P17-93 / 审计 `05 PLG-02`）--")
        for item in abi_problems:
            print("   ✗ %s" % item)
    else:
        print("-- C ABI 的 parent 透传（P17-93 / 审计 `05 PLG-02`）--")
        print("   ✓ `on_panel` / `on_tool` 两条适配都把宿主 parent 原样交给插件 builder")
    problems += abi_problems

    print("-- 实测（契约 %d 条方法）--" % len(contract))
    for name, params in contract:
        bound = bindings.get(name, [])
        lack = [p for p in params if p not in bound and p not in DEFERRED.get(name, {})]
        deferred = [p for p in params if p in DEFERRED.get(name, {})]
        mark = "✓" if not lack else "✗"
        tail = ""
        if deferred:
            tail = "　（欠账已记账：%s）" % "、".join(deferred)
        print("   %s %-14s 契约[%s]%s" % (mark, name, ", ".join(params), tail))
    print()
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("UI-CONTRACT fail %d 项不成立" % len(problems))
        return 1
    print("UI-CONTRACT ok 契约点名的参数**绑定都收**；多给/欠账的**逐条记账**"
          "（EXTRA %d 条 / DEFERRED %d 条方法）" % (sum(len(v) for v in EXTRA.values()), len(DEFERRED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
