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
import re
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
    # ── 按 id 合并的小工具（"结构照参考、内容照译文"）─────────────────────
    STRING_BLOCK = re.compile(
        r'(<string\b[^>]*?\bid\s*=\s*"([^"]+)"[^>]*>)(.*?)(</string>)', re.S)
    TEXT_INNER = re.compile(r"(<text\b[^>]*>)(.*?)(</text>)", re.S)
    XML_DECL = re.compile(r'<\?xml[^>]*encoding\s*=\s*"([^"]+)"', re.I)

    def _decode(data: bytes) -> tuple[str, str]:
        m = XML_DECL.search(data[:200].decode("ascii", "ignore"))
        enc = (m.group(1) if m else "utf-8").lower()
        for cand in (enc, "utf-8", "cp1251"):
            try:
                return data.decode(cand), cand
            except Exception:
                continue
        return data.decode("utf-8", "replace"), "utf-8"

    def _encode_like(merged: str, enc: str) -> tuple[bytes, str]:
        """按参考文件编码写回；**译文里有目标编码表示不了的字符 ⇒ 改 utf-8 并同步声明**。

        为什么不能直接 `encode(enc, "replace")`：cp1251 的参考文件 + 中文译文时，
        「你好」会变成 `??`（实测踩过）——那是最坏的一种"静默损坏"。
        返回 (字节, 实际编码)。
        """
        bom = b"\xef\xbb\xbf" if merged.startswith("\ufeff") else b""
        body = merged[1:] if bom else merged
        try:
            return bom + body.encode(enc), enc
        except UnicodeEncodeError:
            pass
        # 换 utf-8：声明里的 encoding 也要改，否则解析器按旧编码读
        if XML_DECL.search(body[:200]):
            body = XML_DECL.sub('<?xml version="1.0" encoding="utf-8"', body, count=1) \
                if False else re.sub(r'(<\?xml[^>]*encoding\s*=\s*")[^"]+(")',
                                     r'\1utf-8\2', body, count=1, flags=re.I)
        else:
            body = '<?xml version="1.0" encoding="utf-8"?>\n' + body
        return bom + body.encode("utf-8"), "utf-8"

    def _id_map(xml: str) -> dict:
        """id → 文本内层（只认 <string id=…>…<text>…</text>）。"""
        out = {}
        for block, sid, body, _ in STRING_BLOCK.findall(xml):
            tm = TEXT_INNER.search(body)
            if tm:
                out.setdefault(sid, tm.group(2))
        return out

    def _merge_ids(ref_xml: str, tr_map: dict) -> tuple[str, int, int]:
        """按 id 把译文内容写回参考骨架；返回 (新文本, 用到几条, 参考里几条没译文)。"""
        used = [0]
        total = [0]

        def repl(m):
            open_tag, sid, body, close_tag = m.group(1), m.group(2), m.group(3), m.group(4)
            total[0] += 1
            if sid not in tr_map:
                return m.group(0)
            tm = TEXT_INNER.search(body)
            if not tm:
                return m.group(0)
            used[0] += 1
            # 只换 <text> 内层，其余（属性/缩进/注释）逐字节保留
            new_body = body[:tm.start(2)] + tr_map[sid] + body[tm.end(2):]
            return open_tag + new_body + close_tag

        return STRING_BLOCK.sub(repl, ref_xml), used[0], total[0] - used[0]

    long_texts: list[str] = []
    LONG_LIMIT = 1024   # 用户 2026-10-28：>512 不算超限

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
    # ★★★ 2026-10-28（用户：拿 MODS\123 与参照包对比「缺了什么看出来没？」）：
    #   **默认＝照镜子**：参照包里有什么，产物就有什么（fonts.ltx / localization.ltx /
    #   textures/map / ixr_addons 一样不少），只把**字体**换成合并后的、**文本**换成翻译件。
    #   缺一个 `fonts.ltx`，游戏就可能仍按原字体节渲染 ⇒ 中文全成方块，前面几轮全白做。
    #   只要字体的话用 `--fonts-only`。
    src_gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    if args.fonts_only:
        out.mkdir(parents=True, exist_ok=True)
        src_fonts = src_gd / "fonts"
        if src_fonts.is_dir():
            shutil.copytree(src_fonts, out / "gamedata" / "fonts")
    else:
        shutil.copytree(pack, out)
        copied_all = sum(1 for _ in out.rglob("*") if _.is_file())
        total_copied = copied_all
        log("IXRGEN 基底：整包照抄 %d 个文件（fonts / configs / textures / ixr_addons 全保留）"
            % copied_all)
        # ★★★ 2026-10-28（用户：「我汉化包呢？这是能直接打的吗？」）：
        #   只换字体不叫汉化包 —— **文本也要进产物**（否则装上去还是俄文）。
        #   文本来源＝`--text-root`（页面里就是「文本 XML」那一栏），落到包的 `<lang>` 文本目录，
        #   逐字节复制（翻译件已经是成品，不许我们再加工）。
        text_dirs = [Path(x) for x in (args.text_root or []) if Path(x).is_dir()]
        translation_given = bool(text_dirs)
        if not text_dirs:
            # ★★ 用户口径（2026-10-29）：「提取看原文，生成看译文。」
            #   生成阶段的**字符集与文本都该来自译文**；没给译文就只能用参照包自带的原文
            #   ⇒ 那出来的仍是"原文包"。**不许静默**：明确警告 + 报告里标 translation=false。
            log("IXRGEN warn 没有给译文（--text-root / 「文本 XML」）⇒ 字符集与文本只能取参照包原文，"
                "产物不是汉化包")
            own = src_gd / "configs" / "text" / lang
            if not own.is_dir():
                own = src_gd / "text" / lang
            if own.is_dir():
                text_dirs = [own]
        # ★★★ 2026-10-29（用户口径）：「**所有结构按照参考来，内容按照译文来**」——
        #   参考包＝人工提取的完整语言包（结构真源）；译文目录＝纯 XML + 改造（内容真源）。
        #   实现：**按 id 合并**（不是整文件覆盖）——参考文件当骨架，逐 id 换 <text> 内层，
        #   属性/缩进/XML 声明/编码一律照参考；译文缺的 id 保留原文并计数。
        #   配对三级：① 相对路径同名 → ② 文件名同名 → ③ id 集合重叠 ≥60%。
        text_merged_ids = 0
        text_missing_ids = 0
        text_missing_files: list[str] = []
        text_reencoded = 0                # 目标编码装不下译文 ⇒ 改 utf-8 的文件数
        text_unused: list[str] = []
        rewritten_copied = 0
        if text_dirs and not args.no_texts:
            tr_files: list[Path] = []
            for root in text_dirs:
                base = root / lang if (root / lang).is_dir() else root
                tr_files.extend(sorted(base.rglob("*.xml")))
            tr_by_name: dict[str, Path] = {}
            tr_maps: dict[Path, dict] = {}
            for f in tr_files:
                tr_by_name.setdefault(f.name, f)
                try:
                    tr_maps[f] = _id_map(_decode(f.read_bytes())[0])
                except Exception:
                    tr_maps[f] = {}

            ref_targets = sorted({d for d in out.rglob(lang)
                                  if d.is_dir() and d.parent.name == "text"
                                  and any(d.glob("*.xml"))})
            used_files: set[Path] = set()
            for dst_texts in ref_targets:
                for ref_xml_path in sorted(dst_texts.glob("*.xml")):
                    raw = ref_xml_path.read_bytes()
                    ref_str, enc = _decode(raw)
                    ref_ids = _id_map(ref_str)
                    cand = tr_by_name.get(ref_xml_path.name)
                    if cand is None and ref_ids:
                        best, score = None, 0.0
                        for f, m in tr_maps.items():
                            if not m:
                                continue
                            hit = len(set(m) & set(ref_ids))
                            if hit >= max(1, int(0.6 * len(ref_ids))) and hit > score:
                                best, score = f, hit
                        cand = best
                    if cand is None:
                        text_missing_files.append(str(ref_xml_path.relative_to(out)) +
                                                  "（译文里配对不到，保留原文）")
                        continue
                    used_files.add(cand)
                    merged, used_n, miss_n = _merge_ids(ref_str, tr_maps.get(cand, {}))
                    if used_n == 0:
                        text_missing_files.append(str(ref_xml_path.relative_to(out)) +
                                                  "（配到了译文但 id 一个都对不上，保留原文）")
                        continue
                    try:
                        data_out, enc_used = _encode_like(merged, enc)
                        ref_xml_path.write_bytes(data_out)
                        if enc_used != enc:
                            text_reencoded += 1
                    except Exception as exc:
                        log("IXRGEN warn 写不回去 %s：%r" % (ref_xml_path.name, exc))
                        continue
                    text_merged_ids += used_n
                    text_missing_ids += miss_n
                    scan_long(cand)
            for f in tr_files:
                if f not in used_files:
                    text_unused.append(f.name + "（参考包里没有可配对的文件，未使用）")
            log("IXRGEN 文本：按 id 合并 %d 条（缺译文 %d 条，共处理 %d 个参考文件）"
                % (text_merged_ids, text_missing_ids, len(ref_targets)))
            if text_reencoded:
                log("IXRGEN warn %d 个文件的原编码装不下译文 ⇒ 已改 utf-8 并同步 XML 声明"
                    % text_reencoded)
            for line in text_missing_files[:10]:
                log("IXRGEN warn 保留原文：" + line)
            for line in text_unused[:10]:
                log("IXRGEN warn 未使用：" + line)
            # 改造产物（译文目录里的 scripts / gameplay）：参考包没有这层结构 ⇒ 按译文布局搬进去
            for sub in ("scripts", "gameplay"):
                for root in text_dirs:
                    src_sub = root / sub
                    if src_sub.is_dir():
                        shutil.copytree(src_sub, out / sub, dirs_exist_ok=True)
                        rewritten_copied += sum(1 for x in src_sub.rglob("*") if x.is_file())
                        break
            if rewritten_copied:
                log("IXRGEN 改造产物：%d 个文件（scripts/gameplay）" % rewritten_copied)
            report_unmatched = len(text_missing_files)
            report_unused = len(text_unused)
            if long_texts:
                log("IXRGEN warn 超长无换行文本 %d 段（上限 %d 字）" % (len(long_texts), LONG_LIMIT))
                for line in long_texts[:5]:
                    log("IXRGEN warn " + line)
                log_path = out.parent / (out.name + ".long_texts.log")
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
              "texts": locals().get("text_merged_ids", 0),
              "rewritten": locals().get("rewritten_copied", 0),
              "text_missing_ids": locals().get("text_missing_ids", 0),
              "text_reencoded": locals().get("text_reencoded", 0),
              "copied": locals().get("total_copied", 0),
              "translation": locals().get("translation_given", False),
              "text_unmatched": locals().get("report_unmatched", 0),
              "text_unused": locals().get("report_unused", 0),
              "long_texts": len(long_texts), "status": "ok"}

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

    # ★★★ 2026-10-29（同上）：**产物里所有 ttf 都要补字形** —— addon 自带的字体
    #   （`ixr_addons/<任意名字>/...\*.ttf`）以前完全没被合并，那些界面里的中文会变方块。
    #   已经按 fonts.ltx 处理过的不重复；CFF 轮廓的跳过并记账（与主流程同口径）。
    # 被 fonts.ltx **任何**段引用过的字体都算"受管"（哪怕分区把它跳过了）——
    # 否则分区用例里 letterica（size=16 被跳过）会被额外字体那一遍偷偷合并。
    all_names = {str(sec["kv"].get("name", "")) for sec in sections.values()}
    all_names |= set(by_name)
    processed = {str(out_fonts / (n if n.lower().endswith(".ttf") else n + ".ttf"))
                 for n in all_names if n}
    extra_fonts = 0
    if not args.no_extra_fonts and not args.fonts_only:
        def _skip_extra(path_: Path) -> bool:
            """别把汉字灌进**别的语言**的字体树 / 编辑器字体（体积翻倍且没意义）。
            addon 里的 ttf 一律照收（那才是用户问的"额外字体"）。"""
            parts = [x.lower() for x in path_.relative_to(out).parts]
            if "ixr_addons" in parts:
                return False
            if "fonts" in parts:
                idx = parts.index("fonts")
                if idx + 1 < len(parts):
                    sub = parts[idx + 1]
                    if sub in ("editors",):
                        return True
                    if sub != lang.lower():
                        return True
            return False

        for path in sorted(out.rglob("*.ttf")):
            if str(path) in processed:
                continue
            if _skip_extra(path):
                continue
            try:
                added, missing = merge_into(path, cjk_source, cjk_cmap, wanted)
            except RuntimeError:
                report["sections"].append({"name": str(path.relative_to(out)),
                                           "status": "not-glyf"})
                continue
            extra_fonts += 1
            report["added"] += added
            report["missing"] += len(missing)
            report["fonts"].append({"name": str(path.relative_to(out)), "added": added,
                                    "missing": len(missing),
                                    "missing_list": ["U+%04X" % cp for cp in missing[:20]]})
        if extra_fonts:
            log("IXRGEN 额外字体：%d 个（addon 里的 ttf）也补了字形" % extra_fonts)
    report["extra_fonts"] = extra_fonts
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="IX-Ray 语言包生成（fonttools 外部助手）")
    ap.add_argument("--pack", required=True)
    ap.add_argument("--no-extra-fonts", action="store_true",
                    help="不处理 addon 里自带的 ttf（默认会一并补字形）")
    ap.add_argument("--no-texts", action="store_true",
                    help="只产字体，不把「文本 XML」写进产物（默认会写，汉化包必须带文本）")
    ap.add_argument("--fonts-only", action="store_true",
                    help="只产字体（不照抄参照包）；默认是整包照镜子＋换字体/文本")
    ap.add_argument("--full-pack", action="store_true",
                    help="（兼容旧参数）等同默认：整包为基底")
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
