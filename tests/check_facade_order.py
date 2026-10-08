# -*- coding: utf-8 -*-
"""门面**结构体字段顺序 vs 聚合初始化顺序**核对器（能识别 `/* name：说明 */` 这种注释）。

为什么要它：相邻字段类型相同时（比如 `ask_open_file` / `ask_save_file` / `ask_directory`
都是 `std::string(const std::string&)`），对位错了编译器**不报错**，参数会喂给错的能力
——静默串味。本仓已经因为"顺序"被编译器拦下三次（都是类型不同才暴露的），说明
**还有类型相同的那种是拦不住的**，必须有个能一眼看全的核对器。
"""

import re
import sys
from pathlib import Path

# ★ Windows 控制台默认 GBK：判据里的 ✗/✓ 打不出来就会**自己崩**（那不是"有错位"，是脚本错）
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent


def check(header: Path, impl: Path, struct_name: str, marker: str) -> bool:
    text = header.read_text(encoding="utf-8")
    body = text[text.find("struct %s {" % struct_name):]
    body = body[:body.find("\n};")]
    fields = []
    for decl in body.split(";"):
        if "std::function<" not in decl:
            continue
        cleaned = re.sub(r"//[^\n]*", "", decl).strip()
        match = re.search(r"(\w+)\s*$", cleaned)
        if match:
            fields.append(match.group(1))

    code = impl.read_text(encoding="utf-8")
    seg = code[code.find(marker):]
    seg = seg[:seg.find("\n};")]
    # ★ 注释形态两种：`/* name */` 与 `/* name：说明 */`
    inits = re.findall(r"/\*\s*(\w+)", seg)

    print("== %s（%s）" % (struct_name, impl.name))
    print("   结构体 %d 项 / 初始化 %d 项" % (len(fields), len(inits)))
    bad = []
    for index, (field, init) in enumerate(zip(fields, inits)):
        if field != init:
            bad.append((index, field, init))
    if bad:
        for index, field, init in bad:
            print("   ✗ #%d 结构体=%-22s 初始化=%s" % (index, field, init))
    if len(fields) != len(inits):
        print("   ✗ 数量不等：结构体多 %s / 初始化多 %s"
              % (fields[len(inits):][:6], inits[len(fields):][:6]))
    if not bad and len(fields) == len(inits):
        print("   ✓ 逐项对位")
    return not bad and len(fields) == len(inits)


ok = check(ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h",
           ROOT / "src" / "shell" / "widget_facade_qt.cpp", "UiFacade", "const plug::UiFacade")
ok &= check(ROOT / "include" / "xrkit" / "engine" / "plugin" / "engine_facade.h",
            ROOT / "src" / "engine" / "plugin" / "engine_facade_impl.cpp", "EngineFacade",
            "const EngineFacade")
ok &= check(ROOT / "include" / "xrkit" / "engine" / "plugin" / "host_facade.h",
            ROOT / "src" / "engine" / "plugin" / "host_facade_impl.cpp", "HostFacade",
            "const HostFacade")
print()
print("总判：%s" % ("全部对位" if ok else "**有错位**"))
import sys
sys.exit(0 if ok else 1)
