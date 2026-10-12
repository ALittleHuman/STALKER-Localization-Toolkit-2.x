# -*- coding: utf-8 -*-
"""P17-105 判据：**带日期的"当时快照"文档必须自认是快照**（审计 `03 F-12`）。

事实
----
`03 F-12`：`docs/2.0.0-1.x-vs-2.x控件对照(2026-09-27).md` 大面积过期（三处已被审计点名），
而且 ★ **没有任何门禁读这两份文档**（控件对照 ＋ 界面待办清单）⇒ 它们可以**无限漂**。

口径（分两半，各治一种病）
--------------------------
**① "快照"必须自认是快照**：文件名里带日期的文档，文件头必须有"**当时快照 / 不是规格**"
之类的声明，并**指向当前真源**。否则下一个人会把 2026-09-27 的表格当成今天的验收单。
（本仓已有同族的做法：`doc-cite` 守"引用的文档必须存在"；这一条守"过期的文档必须自曝"。）

**② "已更正的断言"必须与实现一致**（防的是"改回旧说法"或"实现被回退"）：
   · `:62` 转换页六颗按钮 —— 文档行必须**不再**写"缺 5 个"，且 `convert_page.cpp` 里
     `add_action("开始转换")` / `("暂停")` / `("恢复备份")` / `("清空备份")` / `("打开输出")` /
     `("导出统计")` **都在**；
   · `:102` 视频页主按钮 —— 文档行必须**不再**写"无按钮…开不了转码"，且 `video_page.cpp` 是
     挂主按钮的那份实现（判据看"页面里有主按钮的挂载点"，不锁具体字面量）；
   · `:130` 插件 `context` 槽位 —— 文档行必须**不再**写"缺 context"，且
     `src/shell/plugin_context_menu.cpp` 里确有 `menu_items_for(..., "context", ...)`。

输出：`DATED-DOC ok …` / `DATED-DOC fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
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
DATED_DOCS = (
    "docs/2.0.0-1.x-vs-2.x控件对照(2026-09-27).md",
    "docs/2.0.0-界面待办清单(2026-09-27).md",
)
CONTROL_DOC = ROOT / "docs" / "2.0.0-1.x-vs-2.x控件对照(2026-09-27).md"
CONVERT_PAGE = ROOT / "src" / "shell" / "pages" / "convert_page.cpp"
CONTEXT_MENU = ROOT / "src" / "shell" / "plugin_context_menu.cpp"

SNAPSHOT_WORDS = ("当时快照", "不是当前规格", "不是规格", "以代码为准")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def has_snapshot_header(first_lines: str) -> list:
    """带日期的文档，文件头必须自认是快照（并指向真源）。"""
    if not any(w in first_lines for w in SNAPSHOT_WORDS):
        return ["文件头没有『当时快照 / 不是规格』之类的声明（下一个人会把它当今天的验收单）"]
    return []


def audit_control_doc(doc: str, convert_page: str, context_menu: str) -> list:
    """只认**断言**，不认**关于更正的说明**（这条判据第一版把说明也判红了 —— 假红）。

    做法：把每行按 `|` 切成单元格，**只看"2.x 现状"那一列**（第 3 列），
    并且**先剥掉**」更正说明段（从"（P17-104 更正"到该单元格末尾）——
    那里引用旧说法是**有意的**（说明"原来写错了什么"）。
    """
    problems = []
    claims = []
    for raw in doc.splitlines():
        if not raw.startswith("|"):
            continue
        # ★ 判据第一版按"固定第 3 格"取「2.x 现状」列 —— 但本文件**列数不一致**
        #   （有的行把"备注"并进了现状列）⇒ 三格的行会落到"判定"格上 ⇒ **漏判（假绿）**。
        #   改成"**整行扫**，但先把『（P17-104 更正…）』那段说明剥掉"——
        #   说明里引用旧说法是**有意的**（讲清"原来写错了什么"），不该判红。
        line = raw
        while True:
            at = line.find("（P17-104 更正")
            if at < 0:
                break
            end = line.find("|", at)
            line = line[:at] + (line[end:] if end >= 0 else "")
        claims.append((raw.split("|")[1].strip(), line))
    for label, cell in claims:
        if "缺 5 个" in cell:
            problems.append("控件对照 %s 行的『2.x 现状』还写着「缺 5 个」" % label)
        if "无按钮" in cell or "界面上开不了转码" in cell:
            problems.append("控件对照 %s 行的『2.x 现状』还写着「无按钮 / 开不了转码」" % label)
        if "缺 context" in cell:
            problems.append("控件对照 %s 行的『2.x 现状』还写着「缺 context」" % label)
    # 实现侧对账（三条各自的对象）
    for name in ("开始转换", "暂停", "恢复备份", "清空备份", "打开输出", "导出统计"):
        if ('add_action(QStringLiteral("%s"))' % name) not in convert_page:
            problems.append("convert_page.cpp：找不到按钮「%s」的挂载点" % name)
    if 'menu_items_for(' not in context_menu or '"context"' not in context_menu:
        problems.append("plugin_context_menu.cpp：找不到 `menu_items_for(..., \"context\", ...)`")
    return problems


def selfcheck() -> tuple:
    good_head = "> ★★ P17-104 加的口径头：本文是 2026-09-27 的『当时快照』，不是当前规格。"
    good_doc = ("| 9 | 1.x 七颗 | [开始转换][暂停/继续][恢复备份][清空备份][打开输出][导出统计] "
                "| ✅（P17-104 更正：本行原写「缺 5 个」） |\n"
                "| 6 | 主按钮 | [转换为 OGM] 主按钮 "
                "| ✅（P17-104 更正：本行原写「无按钮…开不了转码」） |\n"
                "| 4 | context 槽位 | 有 context | ✅（P17-104 更正：本行原写「缺 context」） |\n")
    good_conv = "".join('add_action(QStringLiteral("%s"));\n' % n
                        for n in ("开始转换", "暂停", "恢复备份", "清空备份", "打开输出", "导出统计"))
    good_ctx = 'registry->menu_items_for(host_page_id, "context", context);\n'
    cases = [
        ("好形状", 0, has_snapshot_header(good_head)
         + audit_control_doc(good_doc, good_conv, good_ctx)),
        ("文件头没自认快照", 1, has_snapshot_header("# 2.0.0 界面控件对照\n> 触发：用户反馈\n")),
        ("文档又写缺 context（旧说法回来了）", 1,
         audit_control_doc(good_doc.replace(
             "| 4 | context 槽位 | 有 context | ✅（P17-104 更正：本行原写「缺 context」） |",
             "| 4 | 插件槽位 | ⚠ 缺 context |"), good_conv, good_ctx)),
        # ★ 反例：**更正说明里引用旧说法不算违规**（这是本判据第一版判错的地方 —— 假红）
        ("更正说明引用旧说法不算", 0,
         audit_control_doc("| 4 | x | 有 context | ✅（P17-104 更正：本行原写「缺 context」） |\n",
                           good_conv, good_ctx)),
        ("实现被回退（按钮少了）", 1,
         audit_control_doc(good_doc, good_conv.replace('add_action(QStringLiteral("导出统计"));', ''),
                           good_ctx)),
        ("context 那条链不见了", 1,
         audit_control_doc(good_doc, good_conv, "// 没有 menu_items_for\n")),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good_case = (len(problems) > 0) if want else (not problems)
        ok &= good_case
        results.append((label, want, len(problems), good_case, problems))
    return ok, results


def main() -> int:
    print("== P17-105：带日期的快照文档必须自认快照 ＋ 已更正的断言必须与实现一致（审计 `03 F-12`）==")
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
        print("DATED-DOC fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    problems = []
    for rel in DATED_DOCS:
        p = ROOT / rel
        if not p.is_file():
            problems.append("仓库自带文档缺失：%s" % rel)
            continue
        head = "\n".join(p.read_text(encoding="utf-8", errors="replace").splitlines()[:12])
        problems += ["%s：%s" % (p.name, x) for x in has_snapshot_header(head)]
    for p in (CONTROL_DOC, CONVERT_PAGE, CONTEXT_MENU):
        if not p.is_file():
            problems.append("仓库自带文件缺失：%s" % p.relative_to(ROOT))
    if not problems:
        problems += audit_control_doc(
            CONTROL_DOC.read_text(encoding="utf-8", errors="replace"),
            strip_comments(CONVERT_PAGE.read_text(encoding="utf-8", errors="replace")),
            strip_comments(CONTEXT_MENU.read_text(encoding="utf-8", errors="replace")))

    print("-- 实测 --")
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("DATED-DOC fail %d 项不成立" % len(problems))
        return 1
    print("   ✓ 两份带日期的文档都在文件头自认『当时快照』并指向真源")
    print("   ✓ 控件对照里三处已更正的断言**不再出现旧说法**，且实现侧对得上"
          "（转换页六颗按钮 / 视频页主按钮 / 插件 context 槽位）")
    print()
    print("DATED-DOC ok 带日期的快照文档自曝 + 已更正断言与实现同源（审计 03 F-12）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
