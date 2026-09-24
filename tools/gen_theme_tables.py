#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""生成 2.x 的主题令牌表（`.inc`）—— 与其它生成器同姿势：生成器 + `--check` 锁。

数据源：1.x `toolkit_theme.py` 里的 `THEMES`（暗/亮各 18 个颜色角色）与 `_FONTS`（6 个字体角色）。
**用 AST 读，不 import** —— 这也正是 1.x 自己 Qt 侧的做法（那份 docstring 写着"Qt 侧按 AST
读这里的表，**不抄一份**；抄一份就是漂移来源"）。原因：`toolkit_theme` 会拖进
`toolkit_platform → toolkit_base → tkinter`，在无 Tk 的环境里 import 不起来。

生成物：
  src/core/theme_tables.inc   kThemeRoles / kThemeDark / kThemeLight / kFontRoles / kBgRoles

用法：
    python tools/gen_theme_tables.py            # 生成/刷新
    python tools/gen_theme_tables.py --check     # 只校验（门禁用）
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "core" / "theme_tables.inc"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
THEME_PY = TOOLKIT_1X / "toolkit_theme.py"


def read_tables():
    """从 1.x 源码里**按 AST**取出 THEMES / _FONTS / _BG_ROLES 三个字面量。"""
    src = THEME_PY.read_text(encoding="utf-8")
    tree = ast.parse(src)
    want = {"THEMES", "_FONTS", "_BG_ROLES"}
    found = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and target.id in want:
                found[target.id] = ast.literal_eval(node.value)
    missing = want - set(found)
    if missing:
        raise SystemExit("!! 1.x 源码里找不到这些字面量：%s" % sorted(missing))
    return found["THEMES"], found["_FONTS"], list(found["_BG_ROLES"])


def cxx_str(text: str) -> str:
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def emit(themes, fonts, bg_roles) -> str:
    sha = hashlib.sha256(THEME_PY.read_bytes()).hexdigest()
    modes = list(themes)
    roles = list(themes[modes[0]])
    for mode in modes:
        if list(themes[mode]) != roles:
            raise SystemExit("!! 暗/亮的角色顺序不一致：%s vs %s" % (list(themes[mode]), roles))
    lines = []
    lines.append("// 本文件由 tools/gen_theme_tables.py 从 1.x toolkit_theme.py **按 AST** 生成，请勿手改。\n")
    lines.append("// 数据源 sha256 %s\n" % sha)
    lines.append("//\n")
    lines.append("// 1.x 的口径：**只有一份真值来源**（就是它的 THEMES/_FONTS），Qt 侧按 AST 读这份表、\n")
    lines.append("// 不抄一份（抄一份就是漂移来源）。这里同样是生成出来的，别手改。\n\n")
    lines.append("// 颜色角色（顺序 = 1.x 表序，18 个）\n")
    lines.append("static const char* const kThemeRoles[%d] = {\n" % len(roles))
    for i in range(0, len(roles), 6):
        lines.append("    " + ", ".join(cxx_str(r) for r in roles[i:i + 6]) + ",\n")
    lines.append("};\n\n")
    for mode in modes:
        lines.append("// %s 模式\n" % mode)
        lines.append("static const char* const kTheme%s[%d] = {\n"
                     % (mode.capitalize(), len(roles)))
        vals = [themes[mode][r] for r in roles]
        for i in range(0, len(vals), 6):
            lines.append("    " + ", ".join(cxx_str(v) for v in vals[i:i + 6]) + ",\n")
        lines.append("};\n\n")
    lines.append("// 会把控件底色一起改掉的角色（1.x `_BG_ROLES`）\n")
    lines.append("static const char* const kBgRoles[%d] = {%s};\n\n"
                 % (len(bg_roles), ", ".join(cxx_str(r) for r in bg_roles)))
    lines.append("// 字体角色（1.x `_FONTS`：(family, size[, \"bold\"])）\n")
    lines.append("// ★ 结构体名字要避开公开头里的 `core::ThemeFontSpec`（同名会\"不明确符号\"）\n")
    lines.append("struct ThemeFontRow {\n    const char* role;\n    const char* family;\n"
                 "    int size;\n    int bold;\n};\n\n")
    lines.append("static const ThemeFontRow kThemeFonts[%d] = {\n" % len(fonts))
    for role, spec in fonts.items():
        family = spec[0]
        size = int(spec[1])
        bold = 1 if len(spec) > 2 and str(spec[2]).lower() == "bold" else 0
        lines.append("    {%s, %s, %d, %d},\n" % (cxx_str(role), cxx_str(family), size, bold))
    lines.append("};\n")
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="生成/校验 2.x 的主题令牌表")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not THEME_PY.is_file():
        print("!! 找不到 1.x 的 %s" % THEME_PY)
        print("GEN-THEME-TABLES: FAIL")
        return 1

    themes, fonts, bg_roles = read_tables()
    content = emit(themes, fonts, bg_roles)

    if args.check:
        if not OUT.exists():
            print("  !! 缺文件：%s" % OUT.relative_to(ROOT))
            print("GEN-THEME-TABLES: FAIL")
            return 1
        if OUT.read_bytes() != content.encode("utf-8"):
            print("  !! 与 1.x 的表不一致：%s" % OUT.relative_to(ROOT))
            print("GEN-THEME-TABLES: FAIL")
            return 1
        print("  ok %s（%d 字节）" % (OUT.relative_to(ROOT), len(content.encode("utf-8"))))
        print("GEN-THEME-TABLES: PASS")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(content.encode("utf-8"))
    print("  模式：%s；颜色角色 %d 个；字体角色 %d 个；背景角色 %d 个"
          % (list(themes), len(themes[list(themes)[0]]), len(fonts), len(bg_roles)))
    print("  已写 %s（%d 字节）" % (OUT.relative_to(ROOT), len(content.encode("utf-8"))))
    print("GEN-THEME-TABLES: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
