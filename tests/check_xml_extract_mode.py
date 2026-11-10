# -*- coding: utf-8 -*-
"""XML 校对第三模式「提取文本」的**静态判据**（界面接线层）。

为什么静态：这一条的用户可见行为（"一侧为空 ⇒ 模式变三、筛选消失、按钮换成提取"）在无头
自检里没法真的点；而它的**结构性前提**可以逐条钉住（缺一条就是"界面没接上"）：
  ① 一侧为空才出现（判定走 `fs::collect_tree`，与比较/导出同一份遍历）；
  ② 提取模式下**隐藏「筛选」**、隐藏「开始比较」，显示「提取文本」；
("文本表始终输出；两个复选框独立：①复制语言包 ②改写脚本/配置；都不勾=只出硬编码文本表", "文本表始终输出"),
  ④ 真正调的是引擎 `text::build_language_pack`（不许页面自己写一套摆盘逻辑）；
  ⑤ 结果走状态行（零静默：写出几个 XML、几个改写产物、拒绝覆盖几个）。
自证：把某一锚点在内存里改坏 ⇒ 判据必须报红（不是恒绿）。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src" / "shell" / "pages" / "xml_page.cpp"
HDR = ROOT / "src" / "shell" / "pages" / "xml_page.h"

ANCHORS_CPP = [
    ("void XmlPage::update_extract_mode()", "提取模式的判定/切换入口"),
    ("void XmlPage::run_extract()", "提取动作入口"),
    ("xml_extract_texts", "功能①复选框 objectName"),
    ("xml_extract_rewritten", "功能②复选框 objectName"),
    ("xml_extract_hint", "一侧为空的提示行"),
    ("QStringLiteral(\"提取文本\")", "「提取文本」按钮"),
    ("xrkit::engine::fs::collect_tree", "空目录判定走唯一一份遍历"),
    ("xrkit::engine::text::extract_language_pack", "页面走共享编排（CLI/GUI 同一份）"),
    ("恰好一侧为空", "勾了改写 ⇒ 走完整链（提取 + 摆盘）"),
    ("filter_combo_->setVisible(!show)", "提取模式隐藏「筛选」"),
    ("QStringLiteral(\"extract\")", "模式下拉第三项「提取文本」的 data 键"),
    ("addItem(QStringLiteral(\"提取文本\")", "一侧为空时下拉真的多出第三项"),
    ("dir_a_edit_, &QLineEdit::textChanged", "浏览/粘贴也要触发判定"),
    ("start_button_->setVisible(!show)", "提取模式隐藏「开始比较」"),
    ("复制语言包（文本 XML/字体/localization）", "语言包复制是独立选项（不勾可只抽硬编码）"),
    ("opt.copy_language_pack = want_texts", "页面把语言包复选框传给共享编排"),
    ("opt.write_rewritten = want_rewritten", "页面把改写复选框传给共享编排"),
    ("filter_label_->setVisible(!show)", "提取模式连同「筛选」标签一起隐藏"),
    ("checks_row->addWidget(texts_check_", "复选框单独成行（在输出目录上方）"),
    ("extract_wrap->setStyleSheet", "提取行透明背景（不出现黑底）"),
    ("输出直接建在空的一侧", "输出根固定为空的一侧（不另选）"),
    ("extract_button_->setEnabled(true)", "两框都不勾也可提取（硬编码文本表始终生成）"),
    ("提取失败", "同名文件拒绝覆盖要报出来"),
]
ANCHORS_HDR = [
    ("void update_extract_mode();", "头文件声明切换入口"),
    ("void run_extract();", "头文件声明提取入口"),
    ("extract_mode_", "模式状态位"),
    ("texts_check_", "功能①控件成员"),
    ("rewrite_check_", "功能②控件成员"),
]


def check(text: str, anchors: list[tuple[str, str]]) -> list[str]:
    return [f"缺锚点：{why}（{anchor}）" for anchor, why in anchors if anchor not in text]


def main() -> int:
    print("== XML 校对第三模式「提取文本」静态判据 ==")
    for path in (CPP, HDR):
        if not path.is_file():
            print("!! 缺 %s" % path)
            return 1
    cpp = CPP.read_text(encoding="utf-8", errors="replace")
    hdr = HDR.read_text(encoding="utf-8", errors="replace")

    problems = check(cpp, ANCHORS_CPP) + check(hdr, ANCHORS_HDR)
    for p in problems:
        print("  ✗ " + p)
    if not problems:
        print("  ✓ 界面接线锚点 %d 条全在（cpp %d + hdr %d）"
              % (len(ANCHORS_CPP) + len(ANCHORS_HDR), len(ANCHORS_CPP), len(ANCHORS_HDR)))

    # 自证：故意改坏一条锚点 ⇒ 判据必须报红
    broken = cpp.replace("start_button_->setVisible(!show)", "start_button_->setVisible(show)")
    if check(broken, ANCHORS_CPP):
        print("  ✓ 自证：改坏一条锚点 ⇒ 判据会红")
    else:
        print("  ✗ 自证失败：改坏锚点后判据仍绿（恒绿判据）")
        problems.append("self-red")

    if problems:
        print("XML-EXTRACT-MODE: FAIL（%d 处）" % len(problems))
        return 1
    print("XML-EXTRACT-MODE: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
