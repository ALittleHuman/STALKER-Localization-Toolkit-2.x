#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""IX-Ray 语言包生成器（外部助手，纯 fonttools）—— v0.3。

v0.3（2026-10-27）新增：
  · `--langs rus,eng` / `--all-langs`：一次为多个语言各出一份包（需 `--split-output`，
    否则拒绝 —— 多语言写到同一目录会互相覆盖）。

既有口径（`12_规划_语言包识别与分部位字体.md` §五 + 用户 2026-10-26）：
  · 「指定」模式 = 以参照包为基底：只替换字体，其余文件逐字节保留；
  · 只改字体文件本身（名字不变 ⇒ fonts.ltx 不用动）；
  · 字符集 = 包内 configs/text/<lang>/*.xml + --text-root + configs/ui/**/*.xml；
  · 逐槽位字体计划（--plan）；分区（--sizes）；缺字必须入报告（零静默）。

用法：
  python ixrgen.py --pack <整包> --font <CJK> --out <目录> [--langs rus,eng --split-output]
                   [--plan plan.json] [--sizes 14,16] [--text-root DIR]...
退出码：0 成功；1 失败；2 外部前提缺（fonttools）。
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
except Exception:  # pragma: no cover
    print("IXRGEN error reason=缺少 fonttools（pip install fonttools）")
    raise SystemExit(2)


def log(msg: str) -> None:
    print(msg, flush=True)


def parse_ltx(path: Path) -> dict[str, dict]:
    secs: dict[str, dict] = {}
    cur: dict | None = None
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        s = raw.strip()
        # ★★★ 2026-10-27（用户：「生成结果是俄语包而非汉化包」）：
        #   IX-Ray 的 fonts.ltx 大量**行尾注释**：`name = Arial ;Arial is default font by system`
        #   —— 不剥注释，字体名就变成 `Arial ;Arial is default font by system` ⇒ 找不到文件
        #   ⇒ **主字体（font_default = Arial）被整个跳过** ⇒ 出来的还是俄语字库。
        #   口径：`;` 起注释（行首/行尾都算），`#` 同理。
        for mark in (";", "#"):
            if mark in s:
                s = s.split(mark, 1)[0].strip()
        if not s:
            continue
        if s.startswith("[") and "]" in s:
            head = s[1:s.index("]")]
            cur = {"name": head.split(":", 1)[0].strip(),
                   "base": head.split(":", 1)[1].strip() if ":" in head else "",
                   "kv": {}}
            secs[cur["name"]] = cur
            continue
        if cur is not None and "=" in s:
            k, v = s.split("=", 1)
            cur["kv"][k.strip().lower()] = v.strip()
    return secs


def detect_langs(pack: Path) -> list[str]:
    gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    fonts = gd / "fonts"
    if not fonts.is_dir():
        return []
    return sorted(d.name for d in fonts.iterdir() if d.is_dir())


def collect_charset(pack: Path, lang: str, text_roots: list[Path], scope: str) -> set[str]:
    chars: set[str] = set()

    def eat(xml: Path) -> None:
        try:
            root = ET.fromstring(xml.read_bytes())
        except Exception:
            return
        for node in root.iter():
            if node.tag in ("text", "caption"):
                if node.text:
                    chars.update(node.text)
                for value in (node.attrib or {}).values():
                    chars.update(value)

    gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    dirs: list[Path] = []
    lang_dir = gd / "configs" / "text" / lang
    if lang_dir.is_dir():
        dirs.append(lang_dir)
    for root in text_roots:
        cand = root / lang
        dirs.append(cand if cand.is_dir() else root)
    ui = gd / "configs" / "ui"
    if ui.is_dir():
        dirs.append(ui)
    for d in dirs:
        if d.is_dir():
            for xml in d.rglob("*.xml"):
                eat(xml)
    if scope == "common":
        chars.update(chr(c) for c in range(0x4E00, 0x4E00 + 3500))
    return chars


def merge_into(base_path: Path, cjk_source, cjk_cmap: dict, wanted: list[int]) -> tuple[int, list[int]]:
    dst = TTFont(str(base_path), fontNumber=0)
    if "glyf" not in dst:
        raise RuntimeError("not-glyf")
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
        # ★ cmap 必须真写；只加字形不加映射 = 引擎找不到字（修过）
        for sub in dst["cmap"].tables:
            if not sub.isUnicode():
                continue
            for cp, glyph_name in added_map.items():
                sub.cmap[cp] = glyph_name
    dst.save(str(base_path))
    return len(need), missing


def build_one(args, lang: str, cjk_source, cjk_cmap) -> dict:
    pack = Path(args.pack)
    out = (Path(args.out) / lang) if args.split_output else Path(args.out)
    gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    fonts_ltx = gd / "configs" / "fonts.ltx"
    if not fonts_ltx.is_file():
        raise SystemExit("IXRGEN error reason=不是字体包（缺 configs/fonts.ltx）：%s" % fonts_ltx)
    fonts_dir = gd / "fonts" / lang
    if not fonts_dir.is_dir():
        return {"lang": lang, "status": "skipped", "reason": "缺 fonts/%s" % lang,
                "added": 0, "missing": 0, "fonts": [], "sections": []}

    sections = parse_ltx(fonts_ltx)
    wanted = sorted({ord(c) for c in collect_charset(
        pack, lang, [Path(p) for p in (args.text_root or [])], args.charset_scope)
        if ord(c) > 0x20})

    plan_sections: dict[str, str] = {}
    plan_sizes: set[int] = set()
    if args.plan:
        plan = json.loads(Path(args.plan).read_text(encoding="utf-8"))
        plan_sections = {str(k): str(v) for k, v in (plan.get("sections") or {}).items()}
        plan_sizes = {int(x) for x in (plan.get("sizes") or [])}
    if args.sizes:
        plan_sizes |= {int(x) for x in args.sizes.split(",") if x.strip()}

    # ★★ 2026-10-28（用户）：复制文本时顺带查**超长且没有换行**的正文 ——
    #   逐条记进 `long_texts`，最后落一份 `_xrkit_long_texts.log`（文件|id|码位数|前 180 字）。
    long_texts: list[str] = []
    LONG_LIMIT = 512

    def scan_long(xml: Path) -> None:
        try:
            root = ET.fromstring(xml.read_bytes())
        except Exception:
            return
        for node in root.iter():
            if node.tag not in ("text", "caption"):
                continue
            body = (node.text or "")
            if not body:
                continue
            cp = len(body)
            has_nl = "\n" in body
            if cp > (LONG_LIMIT * 2 if has_nl else LONG_LIMIT):
                sid = ""
                parent = xml.stem
                long_texts.append("%s|%s|%d|%s" % (xml.name, sid or parent, cp,
                                                   body[:180].replace("\n", " ")))

    if out.exists():
        shutil.rmtree(out)
    # ★★★ 2026-10-27（用户：「MODS\123 生成出来什么东西」）：
    #   「指定」模式**不再整包复制** —— 旧行为把参照包连同 `ixr_addons/`、`textures/`、
    #   `configs/` 一起抄成 128 MB 的"另一份模组"，用户打开一看全是游戏文件。
    #   现在默认只产**字体**：只复制 `gamedata/fonts/**`（含 eng/editors 等同一套字体树），
    #   文本与配置**字节不动**（本来也不改它们）。
    #   要旧行为（整包为基底）显式加 `--full-pack`。
    if args.full_pack:
        shutil.copytree(pack, out)
    else:
        out.mkdir(parents=True, exist_ok=True)
        src_gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
        src_fonts = src_gd / "fonts"
        if src_fonts.is_dir():
            shutil.copytree(src_fonts, out / "gamedata" / "fonts")
        # ★★★ 2026-10-28（用户：「我汉化包呢？这是能直接打的吗？」）：
        #   只换字体不叫汉化包 —— **文本也要进产物**（否则装上去还是俄文）。
        #   文本来源＝`--text-root`（页面里就是「文本 XML」那一栏），落到包的 `<lang>` 文本目录，
        #   逐字节复制（翻译件已经是成品，不许我们再加工）。
        text_dirs = [Path(x) for x in (args.text_root or []) if Path(x).is_dir()]
        if not text_dirs:
            # 没指「文本 XML」⇒ 用参照包自带的文本（产物仍是完整包；用户要的是"能直接打"）
            own = src_gd / "configs" / "text" / lang
            if not own.is_dir():
                own = src_gd / "text" / lang
            if own.is_dir():
                text_dirs = [own]
        text_copied = 0
        if text_dirs and not args.no_texts:
            dst_texts = out / "gamedata" / "configs" / "text" / lang
            dst_texts.mkdir(parents=True, exist_ok=True)
            for root in text_dirs:
                base = root / lang if (root / lang).is_dir() else root
                for xml in sorted(base.rglob("*.xml")):
                    target = dst_texts / xml.name
                    shutil.copy2(xml, target)
                    text_copied += 1
                    scan_long(xml)
            log("IXRGEN 文本：%d 个 XML ⇒ %s" % (text_copied, dst_texts))
            if long_texts:
                log("IXRGEN warn 超长无换行文本 %d 段（上限 %d 字）" % (len(long_texts), LONG_LIMIT))
                for line in long_texts[:5]:
                    log("IXRGEN warn " + line)
                log_path = out / "_xrkit_long_texts.log"
                log_path.write_text(
                    "long_texts=%d limit=%d\n" % (len(long_texts), LONG_LIMIT) +
                    "\n".join(long_texts) + "\n", encoding="utf-8")
                log("IXRGEN warn log=%s" % log_path)
    out_gd = out / "gamedata" if (out / "gamedata").is_dir() else out
    out_fonts = out_gd / "fonts" / lang

    by_name: dict[str, list[str]] = {}
    font_for_name: dict[str, Path] = {}
    skipped_sections: list[dict] = []
    for sec_name, sec in sections.items():
        name = sec["kv"].get("name", "")
        if not name:
            continue
        if plan_sizes:
            size = int(sec["kv"].get("size") or 0)
            if size and size not in plan_sizes:
                skipped_sections.append({"name": sec_name,
                                         "reason": "size %d not in partition" % size})
                continue
        by_name.setdefault(name, []).append(sec_name)
        chosen = plan_sections.get(sec_name, "")
        if chosen:
            font_for_name[name] = Path(chosen)

    report = {"lang": lang, "pack": str(pack), "out": str(out), "sections": [],
              "fonts": [], "missing": 0, "charset": len(wanted),
              "added": 0, "skipped_sections": skipped_sections,
              "plan_sections": plan_sections, "sizes": sorted(plan_sizes),
              "texts": text_copied, "long_texts": len(long_texts), "status": "ok"}

    for name in sorted(by_name):
        path = out_fonts / (name if name.lower().endswith(".ttf") else name + ".ttf")
        if not path.is_file():
            report["sections"].append({"name": name, "status": "missing-file"})
            continue
        src = font_for_name.get(name)
        if src is not None and not src.is_file():
            log("IXRGEN warn 计划里的字体不存在：%s（段 %s 回退为 --font）" % (src, by_name[name][0]))
            src = None
        use_src = Path(src) if src is not None else Path(args.font)
        if use_src != Path(args.font):
            one = TTFont(str(use_src), fontNumber=0)
            cmap = one.getBestCmap() or {}
        else:
            one, cmap = cjk_source, cjk_cmap
        try:
            added, missing = merge_into(path, one, cmap, wanted)
        except RuntimeError:
            log("IXRGEN warn %s 不是 glyf 轮廓（CFF 暂不支持），跳过" % path.name)
            report["sections"].append({"name": name, "status": "not-glyf"})
            continue
        report["added"] += added
        report["missing"] += len(missing)
        report["fonts"].append({"name": path.name, "added": added, "missing": len(missing),
                                "missing_list": ["U+%04X" % cp for cp in missing[:20]]})
        for sec_name in by_name[name]:
            report["sections"].append({"name": sec_name, "font": name, "status": "ok"})
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="IX-Ray 语言包生成（fonttools 外部助手）")
    ap.add_argument("--pack", required=True)
    ap.add_argument("--no-texts", action="store_true",
                    help="只产字体，不把「文本 XML」写进产物（默认会写，汉化包必须带文本）")
    ap.add_argument("--full-pack", action="store_true",
                    help="整包为基底（旧行为；默认只产字体，不抄 texts/textures/addons）")
    ap.add_argument("--font", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lang", default="", help="只处理这一个语言（默认：包里的 primary lang）")
    ap.add_argument("--langs", default="", help="多个语言（逗号分隔；需 --split-output）")
    ap.add_argument("--all-langs", action="store_true", help="包里有几个语言就出几份")
    ap.add_argument("--split-output", action="store_true",
                    help="多语言时输出到 <out>/<lang>/")
    ap.add_argument("--text-root", action="append", default=[])
    ap.add_argument("--charset-scope", default="used", choices=["used", "common"])
    ap.add_argument("--plan", default="")
    ap.add_argument("--sizes", default="")
    args = ap.parse_args()

    pack = Path(args.pack)
    font = Path(args.font)
    if not pack.is_dir():
        log("IXRGEN error reason=参照包不存在：%s" % pack)
        return 1
    if not font.is_file():
        log("IXRGEN error reason=中文字体不存在：%s" % font)
        return 1
    gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    if not (gd / "configs" / "fonts.ltx").is_file():
        log("IXRGEN error reason=不是字体包（缺 configs/fonts.ltx）：%s"
            % (gd / "configs" / "fonts.ltx"))
        return 1

    langs: list[str] = []
    if args.all_langs:
        langs = detect_langs(pack)
    elif args.langs:
        langs = [s.strip() for s in args.langs.split(",") if s.strip()]
    elif args.lang:
        langs = [args.lang]
    else:
        lang = "rus"
        loc = gd / "configs" / "localization.ltx"
        if loc.is_file():
            sec = parse_ltx(loc).get("string_table", {})
            if sec and sec["kv"].get("language"):
                lang = sec["kv"]["language"]
        langs = [lang]
    if not langs:
        log("IXRGEN error reason=包内没有可识别的语言目录（fonts/<lang>）")
        return 1
    if len(langs) > 1 and not args.split_output:
        log("IXRGEN error reason=多语言必须加 --split-output（否则会互相覆盖）")
        return 1

    cjk_source = TTFont(str(font), fontNumber=0)
    cjk_cmap = cjk_source.getBestCmap() or {}
    reports = []
    for lang in langs:
        rep = build_one(args, lang, cjk_source, cjk_cmap)
        reports.append(rep)
        log("IXRGEN 语言 %s：字形 +%d，缺字 %d，状态 %s"
            % (lang, rep.get("added", 0), rep.get("missing", 0), rep.get("status")))
    total = {"langs": langs, "reports": reports,
             "added": sum(r.get("added", 0) for r in reports),
             "missing": sum(r.get("missing", 0) for r in reports)}
    log("IXRGEN " + json.dumps(total, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
