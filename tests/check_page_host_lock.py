#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""插件改装 P1 静态锁：**Hub 不许认识具体页**（`docs/2.0.0-插件宿主规格.md` §一/§四）。

为什么要这条锁：改装的目标是"Hub ＝ 容器 + 注册表 + 机制提供者，**不含业务**"。修前它不成立 ——
`hub.cpp` 里既有内置六页的清单数组（`kBuiltinPages[]`），又有
`dynamic_cast<FsPage*>` / `dynamic_cast<XmlPage*>` 去给具体页注入注册表。
只要这条锁不红，谁都可以把具体页类名再写回 Hub，"Hub 不含业务"就退回成一句口号。

判据（hub.cpp，**去注释后按活代码判**，逐处报 `文件:行号`）：
  ① 不许出现任何具体页类名（FsPage/ConvertPage/TextPage/XmlPage/VideoPage/FontPackPage）
     与内置清单类型 `PageInfo`；
  ② 不许 `#include "pages/xxx_page.h"`（只许 `pages/page_base.h`、`pages/page_factory.h`）；
  ③ 不许再索引内置页清单（`builtin_page(` 这个调用形；`builtin_page_count()` 只回条数，放行）；
  ④ **正向**（反过来锁"机制真的被用上"）：`Hub::Hub` 里必须真的
     `register_builtin_pages(pages_)` + `sync_pages()`，`Hub::render_plugin_slots` 里必须有
     `sync_pages()` —— 否则"把注册表用法整段删掉、页又变回硬编码"照样能骗过 ①②③。

自证（`selfcheck()`）：先喂**反例**证明每一条都会红，再喂**干净样本**证明不误报。
★ 静态分析一律走 `tests/_cpp_source.py::strip_comments`（去注释、**保留行号**）——
不去注释会被"注释里引用了修前那一行"骗成假红（本仓有过这个教训，见 `_cpp_source.py` 的头注释）。

输出（供门禁抓取）：`PAGEHOST ok` / `PAGEHOST fail <详情>`；退出码 0/1。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from _cpp_source import function_body, strip_comments  # noqa: E402

HUB = ROOT / "src" / "shell" / "hub.cpp"

CONCRETE_CLASSES = ("FsPage", "ConvertPage", "TextPage", "XmlPage", "VideoPage", "FontPackPage",
                    "PageInfo")
NAME_RE = re.compile(r"\b(?:%s)\b" % "|".join(CONCRETE_CLASSES))
INCLUDE_RE = re.compile(r'#\s*include\s+"(pages/[^"]+)"')
# 只锁**索引清单**那个调用形：`builtin_page(i)`；`builtin_page_count()` 只回条数，放行。
BUILTIN_TABLE_RE = re.compile(r"\bbuiltin_page\s*\(")
ALLOWED_PAGE_INCLUDES = {"page_base.h", "page_factory.h"}


def scan(hub_source: str) -> list[str]:
    """返回违例清单（`hub.cpp:行号 说明`）。空 = 绿。"""
    text = strip_comments(hub_source)
    offenders: list[str] = []

    for match in NAME_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        offenders.append("hub.cpp:%d 出现具体页类名 %s（Hub 不许认识具体页）"
                         % (line, match.group(0)))

    for match in INCLUDE_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        target = Path(match.group(1)).name
        if target not in ALLOWED_PAGE_INCLUDES:
            offenders.append("hub.cpp:%d 直接 include 具体页头 %s（只许 %s）"
                             % (line, match.group(1), "/".join(sorted(ALLOWED_PAGE_INCLUDES))))

    for match in BUILTIN_TABLE_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        offenders.append("hub.cpp:%d 又在索引内置页清单 builtin_page(...)（清单归页模块）" % line)

    # ④ 正向：机制必须真的被用上（按**函数体**分别计数，不写"文件里出现过就行"）
    ctor = function_body(text, "Hub::Hub(")
    if not ctor:
        offenders.append("hub.cpp 里找不到 Hub::Hub 的函数体（判据无从下手 ⇒ 判红，不许静默）")
    else:
        if "register_builtin_pages(pages_)" not in ctor:
            offenders.append("Hub::Hub 里没有 register_builtin_pages(pages_)（内置页又变回硬编码？）")
        if "sync_pages()" not in ctor:
            offenders.append("Hub::Hub 里没有 sync_pages()（注册表没被消费）")
    render = function_body(text, "Hub::render_plugin_slots()")
    if not render:
        offenders.append("hub.cpp 里找不到 Hub::render_plugin_slots 的函数体")
    elif "sync_pages()" not in render:
        offenders.append("Hub::render_plugin_slots 里没有 sync_pages()（插件页又走回另一条路？）")
    return offenders


# ── 判据自身的形态自证（先用假输入证明每一条会红，再证明干净样本不误报）────────────
_SELFCHECK_RED = {
    "具体页类名": 'void f() {\n    auto* fs = dynamic_cast<FsPage*>(w);\n}\n',
    "内置清单类型": 'void f() {\n    const PageInfo& info = builtin_page(0);\n}\n',
    "具体页头": '#include "pages/xml_page.h"\nvoid f() {}\n',
    "正向锁（Hub 不消费注册表）": (
        'Hub::Hub() {\n    page_tabs_->addTab("文件系统");\n}\n'
        'void Hub::render_plugin_slots() {\n    sync_pages();\n}\n'
    ),
}
_SELFCHECK_GREEN = (
    '#include "pages/page_base.h"\n'
    '#include "pages/page_factory.h"\n'
    '// 注释里提到 FsPage / builtin_page(0) 不算数（去注释后判）\n'
    'Hub::Hub() {\n'
    '    register_builtin_pages(pages_);\n'
    '    sync_pages();\n'
    '}\n'
    'void Hub::render_plugin_slots() {\n'
    '    sync_pages();\n'
    '}\n'
)


def selfcheck() -> list[str]:
    broken: list[str] = []
    for name, source in _SELFCHECK_RED.items():
        if not scan(source):
            broken.append("反例「%s」没被判红" % name)
    if scan(_SELFCHECK_GREEN):
        broken.append("干净样本被误报：%s" % scan(_SELFCHECK_GREEN))
    return broken


def main() -> int:
    broken = selfcheck()
    if broken:
        print("PAGEHOST fail 判据自证不过（%d 处）：" % len(broken))
        for item in broken:
            print("           " + item)
        return 1
    if not HUB.is_file():
        # 目标不在 ⇒ 判红（不许"文件没有就算过"）
        print("PAGEHOST fail 找不到 %s（判据没跑成，不是通过）" % HUB.relative_to(ROOT))
        return 1
    offenders = scan(HUB.read_text(encoding="utf-8"))
    if offenders:
        print("PAGEHOST fail %d 处：Hub 又认识具体页了" % len(offenders))
        for item in offenders[:10]:
            print("           " + item)
        return 1
    print("PAGEHOST ok hub.cpp 不认识任何具体页（判据自证 %d 红 + 1 绿；正向锁："
          "register_builtin_pages + sync_pages 都在）" % len(_SELFCHECK_RED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
