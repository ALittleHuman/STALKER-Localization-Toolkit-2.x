#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""生成 xrkit::engine::font 的**字符集分组**与**ltx 模板**（`.inc`）—— 生成器 + `--check` 锁。

为什么也走生成器：这两块都是 1.x `font_pack.py` 里的**字面量**
（`ASCII_CHARS`/`ANSI_BRIDGE`/`FONTGEN_EXTRA`/`LOST_CYRILLIC` 四个字符集、
`FONTS_LTX`/`LOCALIZATION_LTX` 每个游戏各一份、`GAMES` 表）。手抄的话：
  · 字符集里有 40 个生僻汉字（`FONTGEN_EXTRA`）与 40 个西里尔字母（`LOST_CYRILLIC`），
    抄错一个就是"游戏里某个字画成同一个重复字形"，而且**没有任何报错**；
  · ltx 模板里 `ui\\ui_font_11` 这种双反斜杠、制表符、空行，抄错一处产物就不是逐字节一致。
⇒ 从 1.x 的模块里**程序化取出**，并把来源 sha256 写进文件头，`--check` 逐字节锁住。

生成物：
  src/engine/font/charset_groups.inc   kAsciiChars / kCyrillicBlock* / kAnsiBridge /
                                       kFontgenExtra / kLostCyrillic
  src/engine/font/ltx_templates.inc    kGames / kFontsLtx / kLocalizationLtx

用法：
    python tools/gen_font_templates.py           # 生成/刷新
    python tools/gen_font_templates.py --check    # 只校验（门禁用）
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "engine" / "font"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
FONT_PACK_PY = TOOLKIT_1X / "font_pack" / "font_pack.py"


def load_reference():
    """按文件路径装载 1.x 的 `font_pack`（同名命名空间包会挡住 `import font_pack`）。"""
    spec = importlib.util.spec_from_file_location("font_pack_1x", FONT_PACK_PY)
    if spec is None or spec.loader is None:
        raise SystemExit("!! 装载失败：%s" % FONT_PACK_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cxx_string(text: str, indent: str = "    ") -> str:
    r"""转成 C++ 字符串字面量：数据**逐字符转义**，只在转义边界折行。

    ★ 两个坑都踩过，别再改回去：
      1. 折行只是**源码可读性**，相邻字符串字面量在 C++ 里是直接拼接的 ——
         所以折行本身**不引入任何数据字符**；数据里的换行必须写成 `\n` 转义。
         第一版把换行当成折行 ⇒ `fonts.ltx` 模板里的 77 个换行全丢了（产物从
         1569 B 变 1415 B）。
      2. 反过来，第一版给每个折行都补了 `\n` ⇒ 字符集里凭空多出 U+000A
         （门禁当场判红：我们 411 / 1.x 413）。
    """
    escapes = {"\\": "\\\\", '"': '\\"', "\n": "\\n", "\r": "\\r", "\t": "\\t"}
    tokens = [escapes.get(ch, ch) for ch in text]
    lines = []
    cur = ""
    for tok in tokens:
        if len(cur) + len(tok) > 100:
            lines.append(cur)
            cur = ""
        cur += tok
    lines.append(cur)
    return "\n".join('%s"%s"' % (indent, seg) for seg in lines)


def group_text(value) -> str:
    """1.x 里这些分组有的是 `set`、有的是 `str`。集合语义与顺序无关，
    这里一律按码位升序拼成字符串（确定性），C++ 侧按字符逐个收进自己的集合。"""
    if isinstance(value, str):
        return value
    return "".join(sorted(value))


def emit_groups(fp) -> str:
    sha = hashlib.sha256(FONT_PACK_PY.read_bytes()).hexdigest()
    lines = []
    lines.append("// 本文件由 tools/gen_font_templates.py 生成，请勿手改。\n")
    lines.append("// 数据源：1.x font_pack/font_pack.py（sha256 %s）\n" % sha)
    lines.append("//\n")
    lines.append("// 这些是 1.x 的字符集分组字面量：手抄一个字符就会让某个字在游戏里\n")
    lines.append("// 画成**同一个重复字形**（引擎对缺失码位不是留空），且毫无报错。\n\n")
    lines.append("// ASCII + 俄语基础字符（1.x `ASCII_CHARS`）\n")
    lines.append("static const char* const kAsciiChars =\n%s;\n\n"
                 % cxx_string(group_text(fp.ASCII_CHARS)))
    cps = sorted(ord(c) for c in fp.CYRILLIC_BLOCK)
    lines.append("// 完整西里尔块（1.x `CYRILLIC_BLOCK`，实测 U+%04X..U+%04X 连续）\n"
                 % (cps[0], cps[-1]))
    lines.append("static const uint32_t kCyrillicBlockFrom = 0x%04Xu;\n" % cps[0])
    lines.append("static const uint32_t kCyrillicBlockTo = 0x%04Xu;\n\n" % cps[-1])
    lines.append("// 1.x `ANSI_BRIDGE`：引擎的 ansi 桥接会把这些字节映到这些槽位\n")
    lines.append("static const char* const kAnsiBridge = %s;\n\n"
                 % '"%s"' % group_text(fp.ANSI_BRIDGE).replace("\\", "\\\\").replace('"', '\\"'))
    lines.append("// 1.x `FONTGEN_EXTRA`：原版 Char.txt 编码 bug 丢掉的生僻汉字\n")
    lines.append("static const char* const kFontgenExtra = %s;\n\n"
                 % '"%s"' % group_text(fp.FONTGEN_EXTRA).replace("\\", "\\\\").replace('"', '\\"'))
    lines.append("// 1.x `LOST_CYRILLIC`：原版 CharAdder 编码 bug 丢掉的西里尔\n")
    lines.append("// （只有 full_cyrillic=False 的「复刻旧包」模式才剔除；默认保留）\n")
    lines.append("static const char* const kLostCyrillic = %s;\n"
                 % '"%s"' % group_text(fp.LOST_CYRILLIC).replace("\\", "\\\\").replace('"', '\\"'))
    return "".join(lines)


def emit_templates(fp) -> str:
    sha = hashlib.sha256(FONT_PACK_PY.read_bytes()).hexdigest()
    lines = []
    lines.append("// 本文件由 tools/gen_font_templates.py 生成，请勿手改。\n")
    lines.append("// 数据源：1.x font_pack/font_pack.py（sha256 %s）\n" % sha)
    lines.append("//\n")
    lines.append("// `fonts.ltx` 与 `localization.ltx` 的模板：制表符、双反斜杠、空行都要\n")
    lines.append("// 原样保留，否则产物就不是逐字节一致。`localization.ltx` 里的\n")
    lines.append("// `_#LANG#` / `#LANG#` 是占位符（字体后缀 / 加载语言）。\n\n")
    lines.append("struct GameTemplate {\n")
    lines.append("    const char* game;\n")
    lines.append("    const char* cfg_dir;\n")
    lines.append("    const char* dds_fmt;\n")
    lines.append("    const char* fonts_ltx;\n")
    lines.append("    const char* localization_ltx;\n")
    lines.append("};\n\n")
    lines.append("static const GameTemplate kGameTemplates[%d] = {\n" % len(fp.GAMES))
    for game, cfg in fp.GAMES.items():
        lines.append("    {\n")
        lines.append('        "%s", "%s", "%s",\n' % (game, cfg["cfg_dir"], cfg["dds"]))
        lines.append("%s,\n" % cxx_string(fp.FONTS_LTX[game], "        "))
        lines.append("%s,\n" % cxx_string(fp.LOCALIZATION_LTX[game], "        "))
        lines.append("    },\n")
    lines.append("};\n\n")
    lines.append("// 字号表（1.x `SIZES`）：槽位名 + 是否超采样；渲染字号 = 槽位名 + offset，\n")
    lines.append("// 但**文件名与 ltx 仍用槽位名**。\n")
    lines.append("struct FontSize {\n    int size;\n    int x2;\n};\n\n")
    lines.append("static const FontSize kFontSizes[%d] = {\n" % len(fp.SIZES))
    for size, x2 in fp.SIZES:
        lines.append("    {%d, %d},\n" % (size, x2))
    lines.append("};\n")
    return "".join(lines)


def build():
    fp = load_reference()
    files = {
        OUT_DIR / "charset_groups.inc": emit_groups(fp),
        OUT_DIR / "ltx_templates.inc": emit_templates(fp),
    }
    print("    字符集分组：ASCII %d / 西里尔块 %d / 桥接 %d / 生僻补 %d / 丢失西里尔 %d"
          % (len(fp.ASCII_CHARS), len(fp.CYRILLIC_BLOCK), len(fp.ANSI_BRIDGE),
             len(fp.FONTGEN_EXTRA), len(fp.LOST_CYRILLIC)))
    print("    ltx 模板：%s" % ", ".join(fp.GAMES))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="生成/校验字库的字符集分组与 ltx 模板")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not FONT_PACK_PY.is_file():
        print("!! 找不到 1.x 的 %s" % FONT_PACK_PY)
        print("GEN-FONT-TEMPLATES: FAIL")
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    files = build()

    if args.check:
        bad = 0
        for path, content in files.items():
            if not path.exists():
                print("  !! 缺文件：%s" % path.relative_to(ROOT))
                bad += 1
            elif path.read_bytes() != content.encode("utf-8"):
                print("  !! 与 1.x 字面量不一致：%s" % path.relative_to(ROOT))
                bad += 1
            else:
                print("  ok %s（%d 字节）"
                      % (path.relative_to(ROOT), len(content.encode("utf-8"))))
        print("GEN-FONT-TEMPLATES: %s" % ("FAIL" if bad else "PASS"))
        return 1 if bad else 0

    for path, content in files.items():
        path.write_bytes(content.encode("utf-8"))
        print("  已写 %s（%d 字节）" % (path.relative_to(ROOT), len(content.encode("utf-8"))))
    print("GEN-FONT-TEMPLATES: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
