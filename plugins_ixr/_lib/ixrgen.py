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


_OWN_OUTS: set[str] = set()   # 本次运行自己清空/建出来的输出目录（多语言复用同一父目录）


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

    def _normalize_text_xml(path: Path) -> bool:
        """**只对"文本 XML"**（`*/configs/text/**`）：统一成 UTF-8 **无 BOM**。

        用户 2026-10-29：「对于译文文件的处理，只需要保证文本 XML（不包括非文本 XML）
        编码是 UTF-8 无 BOM 就行了。」
          · 有 BOM 的去 BOM；
          · 声明的编码不是 utf-8 ⇒ 按原编码解码后写 utf-8，并把声明改成 utf-8；
          · 没有声明 ⇒ 就写 utf-8 字节（不硬加声明，免得改动不需要改的文件）；
          · **非文本 XML / 其它扩展名一律不碰**（逐字节保留）。
        返回 True 表示真的改写过（用于计数）。
        """
        raw = path.read_bytes()
        had_bom = raw.startswith(b"\xef\xbb\xbf")
        body = raw[3:] if had_bom else raw
        text, enc = _decode(body)
        # 声明里的编码名
        m = XML_DECL.search(text[:200])
        decl_enc = (m.group(1) if m else "").lower()
        need_reencode = had_bom or (enc != "utf-8") or (decl_enc not in ("", "utf-8"))
        if not need_reencode:
            return False
        if decl_enc and decl_enc != "utf-8":
            text = re.sub(r'(<\?xml[^>]*encoding\s*=\s*")[^"]+(")',
                          r'\1utf-8\2', text, count=1, flags=re.I)
        path.write_bytes(text.encode("utf-8"))
        return True

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

    # ★★★ 2026-10-29（交接区审计 P0-1/P0-4/P0-8/P0-11）：`rmtree(out)` 前必须挡住
    #   "out 与输入根重叠 / out 是空串或根目录 / out 是已有非空目录"——否则一次误选
    #   就把参照包、译文、甚至整个盘清空。以下任何一条命中都**拒绝并说清**，不删任何东西。
    def _norm(p_: Path) -> Path:
        try:
            return p_.resolve(strict=False)
        except OSError:
            return p_

    # ★ 注意：要检查的是**本语言实际要写的目录**（split-output 时是 <out>/<lang>），
    #   不是 args.out 原样 —— 修前拿父目录去判，多语言第二遍会被自己的第一遍挡住。
    out_s = str(out)
    if out_s in ("", ".", "..") or out_s.rstrip("\\/") == "":
        return {"status": "error", "error": "拒绝：--out 不能是空、`.` 或根目录（会给错目录做 rmtree）"}
    if len(Path(out_s).parts) == 1 and Path(out_s).drive:
        return {"status": "error", "error": "拒绝：--out 不能是盘符根目录：" + out_s}
    out_abs = _norm(Path(out_s))
    guard_inputs = [("参照包", _norm(pack)), ("中文字体", _norm(Path(args.font)))]
    for i, td in enumerate(args.text_root or []):
        guard_inputs.append(("译文根 #%d" % (i + 1), _norm(Path(td))))
    for label, root_abs in guard_inputs:
        if out_abs == root_abs:
            return {"status": "error",
                    "error": "拒绝：--out 与%s是同一个目录（%s）" % (label, out_abs)}
        if out_abs in root_abs.parents:
            return {"status": "error",
                    "error": "拒绝：--out 是%s的**祖先**（%s 包含 %s）" % (label, out_abs, root_abs)}
        if root_abs in out_abs.parents:
            return {"status": "error",
                    "error": "拒绝：--out 位于%s**内部**（%s 在 %s 下）" % (label, out_abs, root_abs)}
    out_preexisting_files = 0
    if out_abs.exists() and str(out_abs) not in _OWN_OUTS:
        out_preexisting_files = sum(1 for x in out_abs.rglob("*") if x.is_file())
        if out_preexisting_files > 0 and not args.force:
            return {"status": "error",
                    "error": "拒绝：--out 已有 %d 个文件且没给 --force（防止误删用户目录）：%s"
                             % (out_preexisting_files, out_abs)}
    if out_abs.exists():
        shutil.rmtree(out_abs)
    _OWN_OUTS.add(str(out_abs))
    # ★★★ 2026-10-28（用户：拿 MODS\123 与参照包对比「缺了什么看出来没？」）：
    # ★★★ 2026-10-29（用户纠正模型）：「**把译文文件夹复制，然后查与原文语言包的结构差异，
    #   然后生成补全**。文件内容不用管。」
    #   ⇒ 不再做"按 id 合并"（那是把内容改一遍，用户不需要）。现在的算法：
    #     ① 产物 = **译文目录原样复制**（文本 + scripts/gameplay 改造产物）；
    #     ② 拿**原文语言包**做结构基准：它有的、译文缺的文件 ⇒ 从原文**补全**；
    #     ③ 只补本地化相关路径（configs/text、fonts、textures/map、ixr_addons、
    #        fonts.ltx、localization.ltx）；参考包里别的东西（整个游戏的 bin/db/文本素材）
    #        **绝不**搬（否则参考=整个游戏时会把 50 GB 抄一遍）。
    LOCALIZATION_WHITELIST = (
        "gamedata/configs/text/", "gamedata/configs/ui/", "gamedata/fonts/",
        "gamedata/textures/map/", "configs/text/", "fonts/", "textures/map/",
    )
    # ★★ 2026-10-29（交接区审计 P1-2）：`ixr_addons/` 整棵进白名单太宽 —— 参考包是
    #   整个游戏时，addon 里的 bin/、数据库、贴图也会被"补全"进产物。只认 addon 的
    #   **本地化子树**（文本 / UI / 字体 / 两份 ltx），其余一律不搬（只报数）。
    ADDON_LOCALIZATION_PARTS = ("/configs/text/", "/configs/ui/", "/fonts/")
    LOCALIZATION_FILES = ("gamedata/configs/fonts.ltx", "gamedata/configs/localization.ltx",
                          "configs/fonts.ltx", "configs/localization.ltx")

    def _is_localization(rel: str) -> bool:
        r = "/" + rel.replace("\\", "/").lower()
        if r.lstrip("/") in LOCALIZATION_FILES:
            return True
        if any(r.lstrip("/").startswith(w) for w in LOCALIZATION_WHITELIST):
            return True
        if r.startswith("/ixr_addons/"):
            return (any(part in r for part in ADDON_LOCALIZATION_PARTS) or
                    r.endswith("/configs/fonts.ltx") or r.endswith("/configs/localization.ltx"))
        return False

    text_dirs = [Path(x) for x in (args.text_root or []) if Path(x).is_dir()]
    translation_given = bool(text_dirs)
    if not text_dirs:
        log("IXRGEN warn 没有给译文（--text-root /「文本 XML」）⇒ 只能按原文语言包出包，"
            "这不是汉化包")
    out.mkdir(parents=True, exist_ok=True)
    # ① 译文原样复制（文本落 `<out>/gamedata/configs/text/<lang>`；scripts/gameplay 落根）
    text_copied = 0
    rewritten_copied = 0
    text_reencoded = 0            # 规范化成 UTF-8 无 BOM 的文本 XML 数
    tr_rels: set[str] = set()
    if text_dirs and not args.no_texts:
        for root in text_dirs:
            base = root / lang if (root / lang).is_dir() else root
            dst_texts = out / "gamedata" / "configs" / "text" / lang
            dst_texts.mkdir(parents=True, exist_ok=True)
            for xml in sorted(base.rglob("*.xml")):
                target = dst_texts / xml.name
                shutil.copy2(xml, target)
                if _normalize_text_xml(target):
                    text_reencoded += 1
                tr_rels.add(str(target.relative_to(out)).replace("\\", "/").lower())
                text_copied += 1
                scan_long(xml)
            for sub in ("scripts", "gameplay"):
                src_sub = root / sub
                if src_sub.is_dir():
                    shutil.copytree(src_sub, out / sub, dirs_exist_ok=True)
                    rewritten_copied += sum(1 for x in src_sub.rglob("*") if x.is_file())
                    for x in src_sub.rglob("*"):
                        if x.is_file():
                            tr_rels.add(str((out / sub / x.relative_to(src_sub))
                                            .relative_to(out)).replace("\\", "/").lower())
        log("IXRGEN 译文：%d 个文本 XML + %d 个改造产物（原样复制；%d 个规范化成 UTF-8 无 BOM）"
            % (text_copied, rewritten_copied, text_reencoded))

    # ② 结构基准＝原文语言包；只补本地化路径里**译文缺**的文件
    supplemented: list[str] = []
    ref_skipped: list[str] = []
    # 译文按**文件名**索引：参考结构里有个 text 槽位、译文里也有同名 XML ⇒
    # 往那个槽位里放**译文内容**（结构按参考、内容按译文；不是拿原文顶上去）。
    tr_by_name: dict[str, Path] = {}
    if text_dirs and not args.no_texts:
        for root in text_dirs:
            base = root / lang if (root / lang).is_dir() else root
            for xml in sorted(base.rglob("*.xml")):
                tr_by_name.setdefault(xml.name, xml)
    src_gd = pack / "gamedata" if (pack / "gamedata").is_dir() else pack
    ref_files = [x for x in pack.rglob("*") if x.is_file()]
    for src in ref_files:
        rel = str(src.relative_to(pack)).replace("\\", "/")
        if not _is_localization(rel):
            ref_skipped.append(rel)
            continue
        if rel.lower() in tr_rels:
            continue
        dst = out / rel
        if dst.is_file():
            continue                      # 译文里已经有（同路径）⇒ 不覆盖内容
        dst.parent.mkdir(parents=True, exist_ok=True)
        rel_norm = rel.replace("\\", "/").lower()
        tr_same = tr_by_name.get(src.name) if "/configs/text/" in rel_norm else None
        shutil.copy2(tr_same if tr_same is not None else src, dst)
        # 补全来的也是"文本 XML" ⇒ 同样规范成 UTF-8 无 BOM（同一套口径，不搞两制）
        if "/configs/text/" in rel.replace("\\", "/").lower():
            if _normalize_text_xml(dst):
                text_reencoded += 1
        supplemented.append(rel)
    # 参考包里有、但不是本地化路径的文件：整包当参考时**如实报**，绝不静默
    if ref_skipped:
        looks_like_game = any(r.lower().endswith((".db0", ".db", ".db9", ".sqfs")) or
                              r.lower().startswith(("bin/", "appdata/")) for r in ref_skipped)
        log("IXRGEN 参考里另有 %d 个非本地化文件未补%s"
            % (len(ref_skipped),
               "（参考看起来是**整个游戏** ⇒ 只补本地化相关目录）" if looks_like_game else ""))
        for r in ref_skipped[:5]:
            log("IXRGEN note 跳过：" + r)
    log("IXRGEN 补全：%d 个文件（原文有、译文缺，只限本地化路径）" % len(supplemented))
    if long_texts:
        log("IXRGEN warn 超长无换行文本 %d 段（上限 %d 字）" % (len(long_texts), LONG_LIMIT))
        for line in long_texts[:5]:
            log("IXRGEN warn " + line)
        long_log_path = out.parent / (out.name + ".long_texts.log")
        try:
            long_log_path.write_text(
                "long_texts=%d limit=%d\n" % (len(long_texts), LONG_LIMIT) +
                "\n".join(long_texts) + "\n", encoding="utf-8")
            log("IXRGEN warn log=%s" % long_log_path)
        except OSError as exc:
            log("IXRGEN warn 长文本日志写不出去：%r" % (exc,))
    for r in supplemented[:10]:
        log("IXRGEN note 补：" + r)
    report_unmatched = len(supplemented)
    report_unused = len(ref_skipped)
    text_merged_ids = text_copied
    text_missing_ids = len(supplemented)
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
              "texts": locals().get("text_copied", 0),
              "rewritten": locals().get("rewritten_copied", 0),
              "text_reencoded": locals().get("text_reencoded", 0),
              "copied": len([x for x in out.rglob("*") if x.is_file()]),
              "translation": locals().get("text_copied", 0) > 0,
              "supplemented": locals().get("report_unmatched", 0),
              "ref_files_skipped": locals().get("report_unused", 0),
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
    ap.add_argument("--force", action="store_true",
                    help="允许覆盖已有非空输出目录（默认拒绝，防止误删用户目录）")
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
    # ★ 2026-10-29（审计 P1-6）：报告里有 error ⇒ 退出码非 0，门禁才能靠退出码判红。
    #   缺字（missing>0）仍按 0 退出，但状态改成 `ok_with_missing`，页面/日志能看见。
    if any(r.get("status") == "error" for r in reports):
        return 1
    for r in reports:
        if r.get("missing", 0) > 0 and r.get("status") == "ok":
            r["status"] = "ok_with_missing"
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
