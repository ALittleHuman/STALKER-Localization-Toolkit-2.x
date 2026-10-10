#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""像素口径门禁（屏幕自适应，2026-09-27 用户：「我电脑是2.5K屏，所以做一个自适应」）。

判据：**设计稿像素一律过 `px()`** —— shell 侧任何"尺寸/间距"调用里出现**非零裸数字**即判红，
因为那类字面量按 96 DPI 定死，在 2.5K/高 DPI 屏上不会跟着缩放（表现就是"字体变大了、间距没动"）。

放行：
  · 数字 0（不需要缩放）；
  · ALLOW（白名单，逐条写理由；★ 没命中的白名单条目本身判红 —— 见下 "白名单不许当摆设"）；
  · ALLOW_FILES（整文件豁免：测试夹具）。

输出（供门禁抓取）：`PX-ONLY ok` / `PX-ONLY fail <详情>`；退出码 0/1。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CALLS = (
    "setFixedHeight|setMinimumHeight|setMaximumHeight|setFixedWidth|setMinimumWidth"
    "|setMaximumWidth|setSpacing|setContentsMargins|resize"
)
# ★★ 2026-10-06（审计处理批·0-4）：原判据是
#     `\b(?:CALLS)\s*\(\s*(?!xrkit::core::px\()([1-9])`
#   —— `\s*\(` 之后**必须紧跟 `[1-9]`** ⇒ 第一个实参是 `0` 的调用**整类不可判红**：
#     `setContentsMargins(0, 12, 12, 0)`、`resize(0, 100)` —— 而"左/上边距写 0"恰恰是
#     `setContentsMargins` 最常见的形态。于是"存在但整类输入不可判红"。
#   现在拆成两步：① 括号配平地抓到**整个调用**；② 在实参区间里找非零裸数字，
#   并把 `px(...)` 整体先屏蔽。这样"第几个实参是 0"不再影响判据。
PX_CALL = re.compile(r"\b(?:%s)\s*\(" % CALLS)
# ★ px() 在 shell 侧有**两种写法**（这是上一批改到一半时漏掉的第二处）：
#     `src/shell/hub.cpp` / `task_shell.cpp` / `scroll_viewport.cpp` 走**非限定** `px(...)`
#     （`theme.cpp:47` 里 `int px(int value) { return xrkit::core::px(value); }`，shell 命名空间内可见），
#     其他文件走 `xrkit::core::px(...)`。只认后者 ⇒ 把合规调用误判成违规（改到一半时实测：
#     `hub.cpp: resize(px(1180), px(760))` 被判红）。这里两种都认。
PX_ANY = re.compile(r"\bpx\s*\(")
BARE_NUM = re.compile(r"\b[1-9]\d*\b")


class Unbalanced(ValueError):
    """判据撞上括号不配平的调用 —— **不许静默截断**，要当场红（见 `_match_paren`）。"""


def _match_paren(text: str, i: int) -> int:
    """从 `text[i]`（必须是 `(`）配对到它的右括号下标。

    ★★ 关键：**不许在 depth 归零前因为"遇到 `)` 而 depth<0"提前返回**（上一批就死在这里）。
       `resize(px(1180), px(760))` 的内层 `px(` 让朴素实现把第一个 `)` 当成调用结束，
       于是实参区间被截成 `px(1180`——`px(...)` 屏蔽不上，`1180` 反被当成裸数字
       （**合规代码判红**），而真正的违规可能截到一半（**违规整段漏判**）。
       括号不配平（文件里真有语法错）⇒ 抛 `Unbalanced`，由 `scan()`/`main()` 判红并指名文件。
    """
    depth = 0
    j = i
    in_str = None
    in_chr = False
    while j < len(text):
        c = text[j]
        if in_str is not None:
            if c == "\\":
                j += 2
                continue
            if c == in_str:
                in_str = None
        elif in_chr:
            if c == "\\":
                j += 2
                continue
            if c == "'":
                in_chr = False
        elif c == '"':
            in_str = c
        elif c == "'":
            in_chr = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
            if depth < 0:
                raise Unbalanced("下标 %d 处的 `)` 无对应 `(`" % j)
        j += 1
    raise Unbalanced("从下标 %d 起的 `(` 找不到配对的 `)`" % i)


def _px_spans(text: str) -> list:
    """全文里所有 `px(...)` 表达式的 `(起, 止)` 区间。"""
    out = []
    for m in PX_ANY.finditer(text):
        out.append((m.start(), _match_paren(text, m.end() - 1)))
    return out


def bare_in_args(args: str) -> list:
    """实参区间里"没过 px() 包裹的非零裸数字"。

    ★ 必须**按区间屏蔽**而不是把 `px(...)` 文本替换掉：`px(...)` 的参数本身也可能含数字
      （`px(8)`、`px(height) * 16`、`px(1180)`），直接替换会把 `8`/`1180`/`16` 当成违规。
    ★ 屏蔽时**只把数字字符换成空格、其余字符原样保留**：括号/逗号/引号的相对位置因此不变，
      嵌套 px（`xrkit::core::px(core::px(8))`）也不会因为文本替换而配平错位。
    """
    masked = list(args)
    for a, b in _px_spans(args):
        for k in range(a, b + 1):
            if masked[k].isdigit():
                masked[k] = " "
    return BARE_NUM.findall("".join(masked))


def _call_spans(text: str) -> list:
    """返回每个 CALLS 调用的 `(调用起点, 开括号后首字节, 该调用的匹配右括号下标)`。"""
    out = []
    for m in PX_CALL.finditer(text):
        begin = m.end()                  # 开括号之后
        out.append((m.start(), begin, _match_paren(text, begin - 1)))
    return out


# ★ 自证样本（**先证会红**）：判据自身的形态覆盖。
#   BARE = "本该红"：全部来自真代码里的形态（含上一批漏掉的"第一个实参是 0"整类、
#          以及 `px(...)` 出现在**外层调用的实参里**时必须仍然能判红）。
#   WRAPPED = "本该绿"：合规写法，尤其 `px(1180)`/`px(8)` 这种**内层括号**的陷阱形态
#          （上一批正是被它整片误杀：7 处合规调用被判红）。
SELFCHECK_BARE = [
    "setContentsMargins(0, 12, 12, 0)",     # ★ 第一个实参是 0（上一批整类漏判的现场形态）
    "setContentsMargins(12, 0, 0, 0)",
    "resize(0, 100)",                       # ★ 同上
    "setFixedHeight(30)",
    "setSpacing(0, 8)",
    "setContentsMargins(px(8), px(4), 12, px(4))",       # 裸值在第 3 个实参
    "setMinimumWidth(core::px(width) * 16)",             # 裸值在 px() 之后的表达式里
    "resize(px(1180) + 40, px(760))",                    # 裸值在 px() 参与的算式里
]
SELFCHECK_WRAPPED = [
    "setContentsMargins(xrkit::core::px(0), xrkit::core::px(12), "
    "xrkit::core::px(12), xrkit::core::px(0))",
    "setSpacing(0)",
    "resize(xrkit::core::px(1180), xrkit::core::px(760));",
    "resize(px(1180), px(760));",                        # 非限定 px()（hub.cpp 的真实形态）
    "log_panel_layout->setContentsMargins(px(8), px(4), px(8), px(4));",
    "setSpacing(px(6))",
    "setSpacing(px(0, 8))",                              # px() 里带逗号（括号配平的另一处陷阱）
    "setContentsMargins(xrkit::core::px(xrkit::core::px(8)), xrkit::core::px(4), "
    "xrkit::core::px(4), xrkit::core::px(4))",
    "setMinimumHeight(xrkit::core::px(28))",
]
# 不配平 ⇒ 必须抛（不许静默截断出错误的实参区间）
SELFCHECK_UNBALANCED = [
    "resize(px(1180)",
]


def bare_args(snippet: str) -> list:
    """一段活代码里"没过 px() 的非零裸数字"实参（供判据与自证共用）。"""
    hits = []
    for _, begin, end in _call_spans(snippet):
        for n in bare_in_args(snippet[begin:end]):
            hits.append(n)
    return hits


def selfcheck() -> list:
    """判据自身的形态自证：漏一类、误杀一类、或静默容忍不配平，都要在这里先红。"""
    bad = []
    for s in SELFCHECK_BARE:
        if not bare_args(s):
            bad.append("漏判：" + s)
    for s in SELFCHECK_WRAPPED:
        if bare_args(s):
            bad.append("误判（px() 包裹/零值不该红）：%s ⇒ %s" % (s, bare_args(s)))
    for s in SELFCHECK_UNBALANCED:
        try:
            bare_args(s)
        except Unbalanced:
            continue
        bad.append("不配平的调用没被拒绝（判据会截出错区间）：" + s)
    return bad


# 白名单（每条都必须写明理由；**降级要可见**，不许静默跳过）
ALLOW = {
    # 1 逻辑像素的布局技巧（宽度不参与布局撑开），不是设计稿像素
    ("src/shell/log_box.cpp", "setMinimumWidth(1)"),
}
# 整文件豁免：**测试夹具** —— 里面的画布尺寸是"刻意的固定逻辑像素"（测不变量，不测外观），
# 过 px() 反而会让"测什么"随屏幕变。
ALLOW_FILES = {"src/shell/smoke.cpp"}
#
# ★★ 2026-10-06（审计处理批·0-4 附带）：这里原有 KNOWN_DEBT 表，登记
#   `("src/shell/hub.cpp", "resize(1180, 760)")`。**该条已失效**：`hub.cpp:145` 现在是
#   `resize(px(1180), px(760));`（合规），它按 ≤70 字符截断出的摘要形态再也匹配不上任何一次
#   命中 —— 一张"零次命中的豁免许可"，既降不了真违规，也让"白名单在起作用"无从验证。
#   按纪律不留空转豁免：整表撤掉，替换为下面**带命中计数断言**的 KNOWN_BARE。
#
# ★ KNOWN_BARE：修好"零开头整类漏判"之后，新判据**第一次**抓到的存量违规（旧判据结构性看不见：
#   旧正则要求第 1 个实参就是 `[1-9]`，而这两处的裸数字在 `px(...)` 之后的算式里）。
#   与旧 KNOWN_DEBT 的区别（这是它不再是"白拿通行证"的原因）：
#     · 每条写明 `(文件, 行号, 调用形态)`，且**必须命中恰好 `n` 次**；
#     · 计数对不上（违规少了、多了、或挪了地方）⇒ 当场判红，逼人核对；
#     · 命中时**逐条打印可见提示**，并计入收尾行的 `存量 N 处`，不静默。
#   这批的判据口径是**只修门禁假绿、不改产品逻辑**（见任务纪律），所以存量违规如实登记、
#   不顺手改产品代码；`plugin_ui.cpp` 那两处该由 "全部尺寸/间距调用过 px()" 那一批一起收口。
KNOWN_BARE = {
    # ★ 2026-10-10（P17-51）：行号 +5 —— `ui_label` 上方补了「必须能换行」的说明与 `setWordWrap(true)`
    #   （接口契约批「标签换行」），本文件的两处存量违规整体下移。豁免许可必须精确对上，
    #   所以**行号要跟着走**（这两处仍按原计划留给「全部尺寸/间距调用过 px()」那一批收口）。
    ("src/shell/plugin_ui.cpp", 81, "setMinimumHeight(core::px(height) * 16)"):
        # `ui_text()`：16 行文本高度是**设计稿像素乘数**（16 逻辑像素/行），恒不随界面缩放 ⇒
        # 高 DPI 下这块多行框会明显偏矮。待改为 `core::px(height * 16)`（或 `core::px(16) * height`）。
        (1, "16 逻辑像素/行的行高乘数没过 px() —— 高 DPI 下多行框偏矮；待那一批收口"),
    ("src/shell/plugin_ui.cpp", 164, "setMinimumHeight(core::px(height) * 16)"):
        # `ui_text_view()`：同一形状的第二处（只读多行视图）。
        (1, "同上的只读多行视图（同一形状的第二处）"),
}


def active_code(text: str) -> str:
    """只留活代码：去掉整行注释与行尾 `//` 注释（与 run_ci 的静态锁同一口径）。"""
    out = []
    for line in text.splitlines():
        s = line.split("//", 1)[0]
        out.append(s)
    return "\n".join(out)


def scan() -> tuple:
    """返回 `(offenders, 命中的白名单条目集合, 存量违规的命中次数表)`。

    白名单与存量表都**必须真的命中**（见 `main()`）：注册了却匹配不上的豁免 = 假绿。
    """
    offenders = []
    allow_hits = set()
    debt_hits = {}
    files = sorted((ROOT / "src" / "shell").rglob("*.cpp")) + [ROOT / "tools" / "build_gui" / "main.cpp"]
    for src in files:
        text = active_code(src.read_text(encoding="utf-8", errors="replace"))
        rel = str(src.relative_to(ROOT)).replace("\\", "/")
        if rel in ALLOW_FILES:
            continue
        try:
            spans = _call_spans(text)
        except Unbalanced as exc:
            # 判据撞上不配平（源码里真有语法错）⇒ 明确红并指名文件，**不静默跳过这个文件**
            offenders.append("%s: 判据无法配对括号（%s）" % (rel, exc))
            continue
        for start, begin, end in spans:
            if not bare_in_args(text[begin:end]):
                continue
            # 摘要 = 整个调用（单行化）——白名单按"调用形态"匹配，
            # 不再依赖"违规数字恰好在第一个实参位置"。
            snippet = " ".join(text[start:end + 1].split())
            hit = next((k for k in ALLOW if k[0] == rel and k[1] in snippet), None)
            if hit is not None:
                allow_hits.add(hit)
                continue
            line_no = text.count("\n", 0, start) + 1        # 摘要所在行（active_code 不改行数）
            debt = (rel, line_no, snippet)
            if debt in KNOWN_BARE:
                debt_hits[debt] = debt_hits.get(debt, 0) + 1
                continue
            offenders.append("%s:%d: %s" % (rel, line_no, snippet))
    return offenders, allow_hits, debt_hits


def main() -> int:
    # ★ 先跑判据自身的形态自证：自证不过 ⇒ 直接红（不许拿一个漏判的判据去判产品代码）。
    broken = selfcheck()
    if broken:
        print("PX-ONLY fail 判据自证不过（%d 处）：" % len(broken))
        for b in broken:
            print("           " + b)
        return 1
    offenders, allow_hits, debt_hits = scan()
    # ★ 白名单不许当摆设：注册了却一次都没命中的条目 = 该条目已失效（代码改了、
    #   或者它本来就匹配不上），此时"白名单在起作用"是假的 ⇒ 判红，逼人核对或删掉。
    # ★ ASCII 记号，别用 `⇔`/`≤` 这类：控制台是 GBK 时 print 会 UnicodeEncodeError，
    #   门禁自己退 1 却打不出判据（外观像"判据红"，其实是编码炸的）。
    dead_allow = sorted(ALLOW - allow_hits)
    if dead_allow:
        print("PX-ONLY fail %d 条白名单条目零命中（失效的豁免许可 = 假绿）：" % len(dead_allow))
        for k in dead_allow:
            print("           %s: %s" % k)
        return 1
    # ★ 存量违规表同理，但更严：**命中次数必须精确对上**。少了 ⇒ 表已失效（旧 KNOWN_DEBT
    #   就是死在这：条目还在、代码早改成合规了，豁免许可白挂着）；多了/挪了 ⇒ 形状变了。
    debt_bad = []
    for key, (want, why) in KNOWN_BARE.items():
        got = debt_hits.get(key, 0)
        if got != want:
            debt_bad.append("%s:%d 「%s」期望命中 %d 次，实得 %d 次 —— %s"
                            % (key[0], key[1], key[2], want, got, why))
    if debt_bad:
        print("PX-ONLY fail %d 条存量违规登记与实际不符（豁免许可必须精确对得上）：" % len(debt_bad))
        for b in debt_bad:
            print("           " + b)
        return 1
    if offenders:
        print("PX-ONLY fail %d 处尺寸/间距没过 px()：" % len(offenders))
        for o in offenders[:8]:
            print("           " + o)
        return 1
    # 存量违规**逐条可见**（不静默放过）
    for key in sorted(KNOWN_BARE):
        print("PX-ONLY debt %s:%d %s —— %s" % (key[0], key[1], key[2], KNOWN_BARE[key][1]))
    total_debt = sum(n for n, _ in KNOWN_BARE.values())
    print("PX-ONLY ok 尺寸/间距一律过 px()（判据自证 %d 红 + %d 绿 + %d 不配平；"
          "白名单 %d 条；存量待改 %d 处）"
          % (len(SELFCHECK_BARE), len(SELFCHECK_WRAPPED), len(SELFCHECK_UNBALANCED),
             len(ALLOW), total_debt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
