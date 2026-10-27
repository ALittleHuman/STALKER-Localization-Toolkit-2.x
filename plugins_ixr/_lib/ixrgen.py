#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""IX-Ray 语言包生成器（**外部助手**，纯 fonttools）—— 2.x 侧只当它是命令行工具。

口径（`12_规划_语言包识别与分部位字体.md` §五 + 用户 2026-10-26）：
  · 输入 = 一个**能当模组打的整包**（如 `MODS\\~NotoSansSC`）；
  · 「指定」模式 = 以该包为基底：**替换字体、其余文件逐字节保留**（不按模板重写整棵 gamedata）；
  · 只改 `fonts.ltx` 里该段的 `name`/`opentype`（注释/继承/未知键原样保留）；
  · 字符集 = 包里 `configs/text/<lang>/*.xml` + 额外 `--text-root` + `configs/ui/**/*.xml` 的
    `font="..."` 引用（大字号段按段裁剪暂不做，保持"全包字符集"最小口径）；
  · 缺字必须**进报告**（`missing` 计数 + 码位），不许静默。

用法：
  python ixrgen.py --pack <整包目录> --font <CJK ttf> --out <输出目录>
                   [--text-root DIR]... [--charset-scope used|common] [--limit-sections a,b]
输出：JSON（stdout 最后一行 `IXRGEN {...}`）+ 人读行。

退出码：0 成功；1 失败（读不到/不是字体包/写不出）；2 外部前提缺（fonttools/字体/样本）。
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    from fontTools.ttLib import TTFont
except Exception:  # pragma: no cover - 外部前提
    print("IXRGEN error reason=缺少 fonttools（pip install fonttools）")
    raise SystemExit(2)


def log(msg: str) -> None:
    print(msg, flush=True)


def parse_ltx(path: Path) -> list[dict]:
    """保留原始行（生成时按行改写，注释/继承/未知键原样）。"""
    rows: list[dict] = []
    cur: dict | None = None
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = raw.rstrip()
        rows.append({"raw": line, "section": None, "kv": None})
        s = line.strip()
        if not s or s.startswith(";") or s.startswith("#"):
            continue
        if s.startswith("[") and "]" in s:
            head = s[1:s.index("]")]
            name = head.split(":", 1)[0].strip()
            cur = {"name": name, "base": head.split(":", 1)[1].strip() if ":" in head else "",
                   "kv": {}}
            rows[-1]["section"] = cur
            continue
        if cur is not None and "=" in s:
            k, v = s.split("=", 1)
            key = k.strip().lower()
            cur["kv"][key] = v.strip()
            rows[-1]["kv"] = (cur, key)
    return rows


def sections_of(rows: list[dict]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for row in rows:
        if row["section"]:
            out[row["section"]["name"]] = row["section"]
    return out


def collect_charset(pack: Path, text_roots: list[Path], scope: str) -> set[str]:
    chars: set[str] = set()

    def eat(xml: Path) -> None:
        try:
            root = ET.fromstring(xml.read_bytes())
        except Exception:
            return
        for node in root.iter():
            if node.tag in ("text", "caption") and node.text:
                chars.update(node.text)
            for value in (node.attrib or {}).values():
                if node.tag in ("text", "caption") and value:
                    chars.update(value)

    dirs: list[Path] = []
    for gd in (pack / "gamedata", pack):
        if (gd / "configs").is_dir():
            for lang_dir in (gd / "configs" / "text").glob("*"):
                if lang_dir.is_dir():
                    dirs.append(lang_dir)
            ui = gd / "configs" / "ui"
            if ui.is_dir():
                dirs.append(ui)
            break
    dirs.extend(text_roots)
    for d in dirs:
        if d.is_dir():
            for xml in d.rglob("*.xml"):
                eat(xml)
    if scope == "common":
        chars.update(chr(c) for c in range(0x4E00, 0x4E00 + 3500))
    return chars


def main() -> int:
    ap = argparse.ArgumentParser(description="IX-Ray 语言包生成（fonttools 外部助手）")
    ap.add_argument("--pack", required=True, help="参照包（能当模组打的整包目录）")
    ap.add_argument("--font", required=True, help="中文字体 .ttf/.otf")
    ap.add_argument("--out", required=True, help="输出目录（会先清空）")
    ap.add_argument("--text-root", action="append", default=[], help="额外文本根（可重复）")
    ap.add_argument("--charset-scope", default="used", choices=["used", "common"])
    ap.add_argument("--limit-sections", default="", help="只处理这些段（逗号分隔；默认全部）")
    ap.add_argument("--plan", default="", help="逐槽位字体计划 JSON：{\"sections\": {段: 字体路径}, \"sizes\": [14,16]}")
    ap.add_argument("--sizes", default="", help="分区：只处理这些字号档（逗号分隔；空=全部）")
    args = ap.parse_args()

    pack = Path(args.pack)
    font = Path(args.font)
    out = Path(args.out)
    if not pack.is_dir():
        log("IXRGEN error reason=参照包不存在：%s" % pack)
        return 1
    if not font.is_file():
        log("IXRGEN error reason=中文字体不存在：%s" % font)
        return 1
    gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    fonts_ltx = gd / "configs" / "fonts.ltx"
    if not fonts_ltx.is_file():
        log("IXRGEN error reason=不是字体包（缺 configs/fonts.ltx）：%s" % fonts_ltx)
        return 1

    # 语言：localization.ltx 的 [string_table] language；缺 ⇒ rus
    lang = "rus"
    loc = gd / "configs" / "localization.ltx"
    if loc.is_file():
        for row in parse_ltx(loc):
            sec = row["section"]
            if sec and sec["name"].lower() == "string_table" and sec["kv"].get("language"):
                lang = sec["kv"]["language"]

    rows = parse_ltx(fonts_ltx)
    sections = sections_of(rows)
    fonts_dir = gd / "fonts" / lang
    if not fonts_dir.is_dir():
        log("IXRGEN error reason=缺 fonts/%s（这不是矢量字体包，或语言判定错了）" % lang)
        return 1
    ttf_in_pack = sorted(p.name for p in fonts_dir.glob("*.ttf"))
    if not ttf_in_pack:
        log("IXRGEN error reason=fonts/%s 里没有 .ttf" % lang)
        return 1

    chars = collect_charset(pack, [Path(p) for p in args.text_root], args.charset_scope)
    log("IXRGEN 字符集 %d 个（scope=%s）" % (len(chars), args.charset_scope))

    # 输出：整包副本（逐字节保留），只替换字体
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(pack, out)
    out_fonts = (out / "gamedata" / "fonts" / lang) if (out / "gamedata").is_dir() else (out / "fonts" / lang)

    default_source = TTFont(str(font), fontNumber=0)
    default_cmap = default_source.getBestCmap() or {}
    wanted = sorted({ord(c) for c in chars if ord(c) > 0x20})

    limit = {s.strip() for s in args.limit_sections.split(",") if s.strip()}
    # ★★ 2026-10-26：逐槽位字体计划（「分部位/分区」的落点）——
    #   段 → 字体；没在计划里的段用 `--font`。sizes 是分区：只处理这些字号档的段。
    plan_sections: dict[str, str] = {}
    plan_sizes: set[int] = set()
    if args.plan:
        plan_path = Path(args.plan)
        if not plan_path.is_file():
            log("IXRGEN error reason=计划文件不存在：%s" % plan_path)
            return 1
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        plan_sections = {str(k): str(v) for k, v in (plan.get("sections") or {}).items()}
        plan_sizes = {int(x) for x in (plan.get("sizes") or [])}
    if args.sizes:
        plan_sizes |= {int(x) for x in args.sizes.split(",") if x.strip()}
    report = {"lang": lang, "pack": str(pack), "out": str(out), "sections": [], "fonts": [],
              "missing": 0, "charset": len(wanted)}

    # 每个被引用的字体文件合并一次（同名字体被多个段共用 ⇒ 只做一遍）
    by_name: dict[str, list[str]] = {}
    font_for_name: dict[str, Path] = {}
    for sec_name, sec in sections.items():
        name = sec["kv"].get("name", "")
        if not name:
            continue
        if limit and sec_name not in limit:
            continue
        if plan_sizes:
            size = int(sec["kv"].get("size") or 0)
            if size and size not in plan_sizes:
                report.setdefault("skipped_sections", []).append(
                    {"name": sec_name, "reason": "size %d not in partition" % size})
                continue
        by_name.setdefault(name, []).append(sec_name)
        chosen = plan_sections.get(sec_name, "")
        if chosen:
            font_for_name[name] = Path(chosen)

    total_added = 0
    for name in sorted(by_name):
        base_path = out_fonts / (name if name.lower().endswith(".ttf") else name + ".ttf")
        if not base_path.is_file():
            log("IXRGEN warn 段 %s 引用的 %s 不在包里（跳过）" % (by_name[name][0], base_path.name))
            report["sections"].append({"name": name, "status": "missing-file",
                                       "sections": by_name[name]})
            continue
        cjk_source, cjk_cmap = default_source, default_cmap
        section_font = font_for_name.get(name)
        if section_font is not None and not section_font.is_file():
            log("IXRGEN warn 计划里的字体不存在：%s（段 %s 回退为 --font）"
                % (section_font, by_name[name][0]))
            section_font = None
        if section_font is not None and section_font != font:
            cjk_source = TTFont(str(section_font), fontNumber=0)
            cjk_cmap = cjk_source.getBestCmap() or {}
        dst = TTFont(str(base_path), fontNumber=0)
        if "glyf" not in dst:
            log("IXRGEN warn %s 不是 glyf 轮廓（CFF 暂不支持），跳过" % base_path.name)
            report["sections"].append({"name": name, "status": "not-glyf",
                                       "sections": by_name[name]})
            continue
        dst_cmap = dst.getBestCmap() or {}
        need = [cp for cp in wanted if cp not in dst_cmap and cp in cjk_cmap]
        missing = [cp for cp in wanted if cp not in dst_cmap and cp not in cjk_cmap]
        if need:
            from fontTools.pens.recordingPen import DecomposingRecordingPen
            from fontTools.pens.transformPen import TransformPen
            from fontTools.pens.ttGlyphPen import TTGlyphPen

            scale = dst["head"].unitsPerEm / cjk_source["head"].unitsPerEm
            order = list(dst.getGlyphOrder())
            order_set = set(order)
            glyf = dst["glyf"]
            hmtx = dst["hmtx"]
            glyph_set = cjk_source.getGlyphSet()
            added_map: dict[int, str] = {}
            for cp in need:
                gname = cjk_cmap[cp]
                pen = TTGlyphPen(None)
                rec = DecomposingRecordingPen(glyph_set)
                glyph_set[gname].draw(rec)
                rec.replay(TransformPen(pen, (scale, 0, 0, scale, 0, 0)))
                glyph = pen.glyph()
                newname = "uni%04X" % cp
                while newname in order_set:
                    newname += "_"
                glyf.glyphs[newname] = glyph
                order.append(newname)
                order_set.add(newname)
                adv, lsb = cjk_source["hmtx"][gname]
                hmtx.metrics[newname] = (int(round(adv * scale)), int(round(lsb * scale)))
                added_map[cp] = newname
            dst.setGlyphOrder(order)
            dst["maxp"].numGlyphs = len(order)
            # ★ 关键：字形加进去了还得**进 cmap**，否则引擎按码位找不到（修前漏了这一步：
            #   added 数对、输出 cmap 里却没有——判据一读 cmap 就露）。
            for sub in dst["cmap"].tables:
                if not sub.isUnicode():
                    continue
                for cp, glyph_name in added_map.items():
                    sub.cmap[cp] = glyph_name
        dst.save(str(base_path))
        total_added += len(need)
        report["missing"] += len(missing)
        report["fonts"].append({"name": base_path.name, "added": len(need),
                                "missing": len(missing),
                                "missing_list": ["U+%04X" % cp for cp in missing[:20]]})
        for sec_name in by_name[name]:
            report["sections"].append({"name": sec_name, "font": name, "status": "ok"})

    report["added"] = total_added
    report["plan_sections"] = {k: v for k, v in plan_sections.items()}
    report["sizes"] = sorted(plan_sizes)
    log("IXRGEN done 字体 %d 个，新增字形 %d，缺字 %d"
        % (len(report["fonts"]), total_added, report["missing"]))
    log("IXRGEN " + json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
