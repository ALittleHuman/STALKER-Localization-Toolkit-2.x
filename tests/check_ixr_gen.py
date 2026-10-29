# -*- coding: utf-8 -*-
"""IX-Ray 语言包生成器（`plugins_ixr/_lib/ixrgen.py`）判据：**真合并、真字节、零静默**。

口径（用户 2026-10-26 + `12_规划_语言包识别与分部位字体.md` §五）：
  · 「指定」模式 = 以参照包为基底，**只替换字体**，其余文件逐字节保留（ltx/文本都不许动）；
  · 合并后 **cmap 必须真的含那些码位**（修前只加字形不加 cmap：added 数对、引擎仍找不到字）；
  · 缺字必须进报告（`missing` 计数），不许静默；
  · 不是字体包 ⇒ 退码 1（不是"什么都没做"）。

外部前提：系统字体 `arial.ttf`（拉丁底）与 `NotoSansSC-VF.ttf`（中文字源）；缺 ⇒ 退码 2。
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "plugins_ixr" / "_lib" / "ixrgen.py"
BASE_FONT = Path(r"C:\Windows\Fonts\arial.ttf")
CJK_FONT = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")

TEXT = "你好，汉化测试：Привет"   # 中英俄混排（俄文底字体本来就覆盖，中文要靠合并）


def fail(msg: str) -> None:
    print("  ✗ " + msg)


def ok(msg: str) -> None:
    print("  ✓ " + msg)


def build_pack(root: Path, font: Path, text: str) -> Path:
    pack = root / "pack"
    gd = pack / "gamedata"
    (gd / "configs" / "text" / "rus").mkdir(parents=True)
    (gd / "fonts" / "rus").mkdir(parents=True)
    shutil.copy2(font, gd / "fonts" / "rus" / "arial.ttf")
    (gd / "configs" / "fonts.ltx").write_text(
        "; 判据夹具：一段矢量字体（name 指向 arial）\n"
        "[font_default]\n"
        "shader = hud\\font\n"
        "size = 14\n"
        "name = arial\n"
        "opentype = false\n"
        "[font_letterica]:font_default\n"
        "name = arial\n"
        "size = 16\n", encoding="utf-8")
    (gd / "configs" / "localization.ltx").write_text(
        "[string_table]\nlanguage = rus\n", encoding="utf-8")
    (gd / "configs" / "text" / "rus" / "st_test.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '  <string id="t1"><text>%s</text></string>\n</string_table>\n' % text,
        encoding="utf-8")
    return pack


def run_tool(pack: Path, font: Path, out: Path, extra: list | None = None) -> tuple[int, str]:
    cmd = [sys.executable, str(TOOL), "--pack", str(pack),
           "--font", str(font), "--out", str(out)] + list(extra or [])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def report_of(out: str) -> dict:
    for line in out.splitlines():
        if line.startswith("IXRGEN {"):
            return json.loads(line[len("IXRGEN "):])
    return {}


def main() -> int:
    print("== IX-Ray 生成器（ixrgen）：真合并 + 逐字节保留 + 缺字入账 ==")
    if not TOOL.is_file():
        print("!! 缺 %s" % TOOL)
        return 1
    missing_fonts = [p for p in (BASE_FONT, CJK_FONT) if not p.is_file()]
    if missing_fonts:
        print("!! 缺系统字体：%s" % ", ".join(str(p) for p in missing_fonts))
        print("IXR-GEN: SKIP (fonts-missing)")
        return 2
    try:
        from fontTools.ttLib import TTFont
    except Exception:
        print("!! 缺 fonttools")
        print("IXR-GEN: SKIP (fonttools-missing)")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="ixrgen_check_"))
    problems = 0
    try:
        pack = build_pack(tmp, BASE_FONT, TEXT)
        ltx_before = (pack / "gamedata" / "configs" / "fonts.ltx").read_bytes()
        text_before = (pack / "gamedata" / "configs" / "text" / "rus" / "st_test.xml").read_bytes()
        base_cmap = set(TTFont(str(pack / "gamedata" / "fonts" / "rus" / "arial.ttf")).getBestCmap())
        base_size = (pack / "gamedata" / "fonts" / "rus" / "arial.ttf").stat().st_size

        out = tmp / "out"
        rc, stdout = run_tool(pack, CJK_FONT, out)
        rep = report_of(stdout)
        merged = out / "gamedata" / "fonts" / "rus" / "arial.ttf"
        if rc == 0 and merged.is_file():
            ok("生成成功：rc=0、输出 arial.ttf 存在")
        else:
            problems += 1
            fail("生成失败：rc=%d\n%s" % (rc, stdout[-800:]))

        if merged.is_file():
            cmap = set(TTFont(str(merged)).getBestCmap())
            want = {ord(c) for c in TEXT if ord(c) > 0x20}
            absent = sorted(cp for cp in want if cp not in cmap)
            if not absent:
                ok("cmap 真的含文本里的全部码位（%d 个）—— 不是只加字形不加映射" % len(want))
            else:
                problems += 1
                fail("cmap 缺码位：%s" % ", ".join("U+%04X" % cp for cp in absent))
            if len(cmap) > len(base_cmap):
                ok("cmap 比底字体大（%d → %d）" % (len(base_cmap), len(cmap)))
            else:
                problems += 1
                fail("cmap 没有变大（%d → %d）" % (len(base_cmap), len(cmap)))
            # 体积只做"没被写坏"的下界（几个汉字可能压不进体积差；cmap 变大才是硬证据）
            if merged.stat().st_size > base_size * 0.9:
                ok("字体体积正常（%.0f KB → %.0f KB）"
                   % (base_size / 1024, merged.stat().st_size / 1024))
            else:
                problems += 1
                fail("字体体积异常缩小（%.0f KB → %.0f KB）"
                     % (base_size / 1024, merged.stat().st_size / 1024))

            if rep.get("missing") == 0:
                ok("报告 missing=0")
            else:
                problems += 1
                fail("报告 missing=%r（期望 0）" % rep.get("missing"))
            if rep.get("added", 0) > 0:
                ok("报告 added=%d" % rep["added"])
            else:
                problems += 1
                fail("报告 added=%r（期望 >0）" % rep.get("added"))

        # ① 2026-10-27 新口径：**默认只产字体** —— 输出里**不许有** configs/texts/ixr_addons
        #   （用户：「MODS\123 生成出来什么东西」：旧行为把整包抄成 128 MB 的"另一份模组"）。
        #   文本与配置**不进产物** ⇒ 也就谈不上"被改"。
        if merged.is_file():
            # 2026-10-28 新口径：默认**整包照镜子** —— 参照包里有什么，产物就有什么
            #（fonts.ltx / localization.ltx / textures / ixr_addons 一样不少），
            #  只把字体换成合并后的、文本换成翻译件。
            missing = []
            for src_file in pack.rglob("*"):
                if not src_file.is_file():
                    continue
                rel = src_file.relative_to(pack)
                if not (out / rel).is_file():
                    missing.append(str(rel))
            if not missing:
                ok("产物照镜子：参照包的 %d 个文件一个不少" % sum(1 for _ in pack.rglob("*") if _.is_file()))
            else:
                problems += 1
                fail("产物缺 %d 个参照包文件（例 %s）" % (len(missing), missing[:3]))
            # ★ 结构只由参考包决定 ⇒ **不许多出文件**（译文里多出来的不许塞进产物）
            extra = []
            for out_file in out.rglob("*"):
                if not out_file.is_file():
                    continue
                rel = out_file.relative_to(out)
                if not (pack / rel).is_file():
                    extra.append(str(rel))
            if not extra:
                ok("产物文件集合 == 参照包（结构只由参考包决定，不增不减）")
            else:
                problems += 1
                fail("产物多出 %d 个参照包里没有的文件（例 %s）" % (len(extra), extra[:3]))
            ltx_src = pack / "gamedata" / "configs" / "fonts.ltx"
            ltx_out = out / "gamedata" / "configs" / "fonts.ltx"
            if ltx_src.is_file() and ltx_out.is_file() and ltx_src.read_bytes() == ltx_out.read_bytes():
                ok("产物里的 fonts.ltx 与参照包逐字节一致（决定字体节怎么引用）")
            else:
                problems += 1
                fail("产物里的 fonts.ltx 缺失或被改（游戏会按别的字体节渲染）")
            # ★ 2026-10-28：汉化包必须带文本（用户：「我汉化包呢」）
            text_out = out / "gamedata" / "configs" / "text" / "rus"
            src_text = pack / "gamedata" / "configs" / "text" / "rus" / "st_test.xml"
            if src_text.is_file():
                copied = text_out / "st_test.xml"
                if copied.is_file() and copied.read_bytes() == src_text.read_bytes():
                    ok("产物带文本：configs/text/rus/st_test.xml 逐字节一致")
                else:
                    problems += 1
                    fail("产物里没有（或改动了）configs/text/rus/st_test.xml")
            # 参照包本身**逐字节未改**（这条比"输出里那份没改"更强）
            if (pack / "gamedata" / "configs" / "fonts.ltx").read_bytes() == ltx_before:
                ok("参照包的 fonts.ltx 逐字节未改")
            else:
                problems += 1
                fail("参照包的 fonts.ltx 被改了（只许读）")
            if (pack / "gamedata" / "configs" / "text" / "rus" / "st_test.xml").read_bytes() == text_before:
                ok("参照包的文本逐字节未改")
            else:
                problems += 1
                fail("参照包的文本被改了（只许读）")

        # ①-b 2026-10-27（用户：「生成结果是俄语包而非汉化包」）——两条回归：
        #   (1) `fonts.ltx` 的**行尾注释**必须剥掉：`name = arial ;Arial is default...`
        #       不剥 ⇒ 字体名带注释 ⇒ 找不到文件 ⇒ 主字体被跳过 ⇒ 出来还是原语言字库。
        #   (2) 字符集必须吃 `--text-root`（页面里＝「文本 XML」那一栏）：
        #       包内文本是拉丁，文本根里是中文 ⇒ 产物字体必须含「汉」。
        pack_c = build_pack(tmp / "comment", BASE_FONT, "Hello")
        ltx_c = pack_c / "gamedata" / "configs" / "fonts.ltx"
        ltx_c.write_text(ltx_c.read_text(encoding="utf-8").replace(
            "name = arial\n", "name = arial ;Arial is default font by system\n"), encoding="utf-8")
        cn_root = tmp / "cn_text"
        (cn_root).mkdir(parents=True, exist_ok=True)
        (cn_root / "cn.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="a"><text>\u4f60\u597d\u6c49\u5316</text></string></string_table>',
            encoding="utf-8")
        out_c = tmp / "out_comment"
        rc_c, so_c = run_tool(pack_c, CJK_FONT, out_c, ["--text-root", str(cn_root)])
        rep_c = report_of(so_c)
        merged_c = out_c / "gamedata" / "fonts" / "rus" / "arial.ttf"
        if rc_c == 0 and merged_c.is_file():
            cmap_c = set(TTFont(str(merged_c)).getBestCmap())
            if 0x6C49 in cmap_c:
                ok("行尾注释被剥掉 ＋ --text-root 中文进字符集（cmap 含「汉」）")
            else:
                problems += 1
                fail("产物字体里没有「汉」—— 注释没剥或 text-root 没吃（charset=%r added=%r）"
                     % (rep_c.get("charset"), rep_c.get("added")))
        else:
            problems += 1
            fail("带注释的 fonts.ltx 生成失败：rc=%d\n%s" % (rc_c, so_c[-400:]))
        sec_names = [s.get("name") for s in (rep_c.get("reports") or [{}])[0].get("sections", [])]
        bad_names = [n for n in sec_names if n and (";" in str(n) or "default" in str(n).lower() and " " in str(n))]
        if not bad_names:
            ok("段名里没有残留注释（%s）" % ", ".join(str(n) for n in sec_names)[:80])
        else:
            problems += 1
            fail("段名里还带注释：%s" % bad_names)

        # ①-b2 用户口径（2026-10-29）：「提取看原文，生成看译文。」
        #   生成阶段没给译文 ⇒ 必须**明确警告**且报告标 translation=false（不许静默出原文包）。
        no_tr_out = tmp / "out_no_translation"
        rc_nt, so_nt = run_tool(pack, CJK_FONT, no_tr_out)
        rep_nt = report_of(so_nt)
        if rc_nt == 0 and "IXRGEN warn 没有给译文" in so_nt:
            ok("没给译文：明确警告（不会静默出原文包）")
        else:
            problems += 1
            fail("没给译文却没警告：rc=%d\n%s" % (rc_nt, so_nt[-300:]))
        per_lang = (rep_nt.get("reports") or [{}])[0]
        if per_lang.get("translation") is False:
            ok("报告里 translation=false（每语言报告里）")
        else:
            problems += 1
            fail("报告里的 translation 不是 false（实得 %r）" % per_lang.get("translation"))

        # ①-c 2026-10-29（用户）：「换了别的名字和文件树形式、更多此类文件夹，都能生成吗？
        #   里面的额外字体呢？」—— 造一个**任意名字 + 多层嵌套**的 addon：
        #   它自带 `configs/text/rus/*.xml` 与 `fonts/rus/extra.ttf`；断言
        #   ① 它的文本被翻译件替换；② 它的 ttf 也被补了中文。
        add_pack = build_pack(tmp / "addon", BASE_FONT, "Hello")
        add_dir = add_pack / "ixr_addons" / "_renamed" / "nested"
        (add_dir / "configs" / "text" / "rus").mkdir(parents=True, exist_ok=True)
        (add_dir / "fonts" / "rus").mkdir(parents=True, exist_ok=True)
        (add_dir / "configs" / "text" / "rus" / "addon.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="b"><text>старый текст</text></string></string_table>', encoding="utf-8")
        shutil.copy2(BASE_FONT, add_dir / "fonts" / "rus" / "extra.ttf")
        add_cn = tmp / "addon_cn"
        add_cn.mkdir(parents=True, exist_ok=True)
        (add_cn / "addon.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="b"><text>附加模组译文</text></string></string_table>', encoding="utf-8")
        add_out = tmp / "out_addon"
        rc_a, so_a = run_tool(add_pack, CJK_FONT, add_out, ["--text-root", str(add_cn)])
        replaced = add_out / "ixr_addons" / "_renamed" / "nested" / "configs" / "text" / "rus" / "addon.xml"
        if rc_a == 0 and replaced.is_file() and "附加模组译文" in replaced.read_text(
                encoding="utf-8", errors="replace"):
            ok("任意名字/嵌套的 addon：它的文本被翻译件替换")
        else:
            problems += 1
            fail("addon 里的文本没被替换：rc=%d\n%s" % (rc_a, so_a[-400:]))
        # ②-b 结构/内容口径（用户 2026-10-29）：
        #   译文里多一个参考包没有的 XML ⇒ 必须 warn「未使用」且**不进产物**。
        un_used = tmp / "unused_case"
        (un_used / "pack" / "gamedata" / "configs" / "text" / "rus").mkdir(parents=True)
        (un_used / "pack" / "gamedata" / "configs" / "fonts.ltx").write_text(
            "[font_default]\nname = arial\n", encoding="utf-8")
        shutil.copytree(pack / "gamedata" / "fonts", un_used / "pack" / "gamedata" / "fonts",
                        dirs_exist_ok=True)
        (un_used / "pack" / "gamedata" / "configs" / "text" / "rus" / "in_ref.xml").write_text(
            '<string_table><string id="a"><text>原文</text></string></string_table>',
            encoding="utf-8")
        (un_used / "cn").mkdir(parents=True)
        (un_used / "cn" / "in_ref.xml").write_text(
            '<string_table><string id="a"><text>译文</text></string></string_table>',
            encoding="utf-8")
        (un_used / "cn" / "not_in_ref.xml").write_text(
            '<string_table><string id="b"><text>多余的</text></string></string_table>',
            encoding="utf-8")
        rc_u, so_u = run_tool(un_used / "pack", CJK_FONT, un_used / "out",
                              ["--text-root", str(un_used / "cn")])
        rep_u = (report_of(so_u).get("reports") or [{}])[0]
        # 新模型：译文**整体复制**（参考里没有的同名文件也要带过去）
        if rc_u == 0 and (un_used / "out" / "gamedata" / "configs" / "text" / "rus" /
                          "not_in_ref.xml").is_file():
            ok("译文里有、参考里没有的 XML：照样复制进产物（译文是内容源）")
        else:
            problems += 1
            fail("译文多余文件没复制：rc=%d\n%s" % (rc_u, so_u[-400:]))

        extra_ttf = add_out / "ixr_addons" / "_renamed" / "nested" / "fonts" / "rus" / "extra.ttf"
        if extra_ttf.is_file():
            cm = set(TTFont(str(extra_ttf)).getBestCmap())
            if any(ord(ch) in cm for ch in "附加模组译文"):
                ok("addon 自带的 ttf 也被补了字形（不再漏成方块）")
            else:
                problems += 1
                fail("addon 自带的 ttf 没补字形（cmap 里找不到译文里的字）")
        else:
            problems += 1
            fail("addon 自带的 ttf 没进产物")

        # ①-f 用户纠正后的模型（2026-10-29）：
        #   「把译文文件夹复制，然后查与原文语言包的结构差异，然后生成补全。文件内容不用管。」
        #   ⇒ ① 译文 XML 原样复制（逐字节）；② 原文有、译文缺的**本地化**文件被补全（逐字节）；
        #     ③ 参考里非本地化文件（bin/、*.db0…）**不补**，只在日志里报数（整包当参考也不会抄游戏）。
        cf_pack = build_pack(tmp / "copyfill", BASE_FONT, "Hello")
        # 参考里再放一个"非本地化"文件，验证它不会被补进产物
        (cf_pack / "bin").mkdir(parents=True, exist_ok=True)
        (cf_pack / "bin" / "game.exe").write_bytes(b"MZ fake")
        (cf_pack / "gamedata" / "configs" / "text" / "rus" / "extra_ref.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="z"><text>原文补全</text></string></string_table>', encoding="utf-8")
        cf_cn = tmp / "copyfill_cn"
        cf_cn.mkdir(parents=True, exist_ok=True)
        (cf_cn / "st_test.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="t1"><text>译文</text></string></string_table>', encoding="utf-8")
        cf_out = tmp / "out_copyfill"
        rc_cf, so_cf = run_tool(cf_pack, CJK_FONT, cf_out, ["--text-root", str(cf_cn)])
        tr_xml = cf_out / "gamedata" / "configs" / "text" / "rus" / "st_test.xml"
        if rc_cf == 0 and tr_xml.is_file() and tr_xml.read_bytes() == (cf_cn / "st_test.xml").read_bytes():
            ok("译文 XML 原样复制（逐字节，不再改内容）")
        else:
            problems += 1
            fail("译文 XML 没被原样复制：rc=%d\n%s" % (rc_cf, so_cf[-300:]))
        ref_only = cf_out / "gamedata" / "configs" / "text" / "rus" / "extra_ref.xml"
        if ref_only.is_file() and "原文补全" in ref_only.read_text(encoding="utf-8", errors="replace"):
            ok("原文有、译文缺的本地化文件被补全")
        else:
            problems += 1
            fail("本地化结构补全失败（extra_ref.xml 不在产物里）")
        if not (cf_out / "bin" / "game.exe").exists():
            ok("参考里的非本地化文件（bin/game.exe）没被抄进产物")
        else:
            problems += 1
            fail("参考里的 bin/game.exe 被抄进了产物（整包当参考就会抄游戏）")
        if "参考里另有" in so_cf and "未补" in so_cf:
            ok("日志如实报「参考里另有 N 个非本地化文件未补」")
        else:
            problems += 1
            fail("日志没报非本地化文件跳过数：%s" % so_cf[-200:])

        # ①-g 文本 XML 编码口径（用户 2026-10-29）：
        #   「只需要保证文本 XML（不包括非文本 XML）编码是 UTF-8 无 BOM 就行了。」
        enc_pack = build_pack(tmp / "enc2", BASE_FONT, "Hello")
        ui_dir = enc_pack / "gamedata" / "configs" / "ui"
        ui_dir.mkdir(parents=True, exist_ok=True)
        ui_bytes = ('<?xml version="1.0" encoding="windows-1251"?>\r\n<ui><x a="1"/></ui>\r\n'
                    ).encode("cp1251")
        (ui_dir / "ui_ref.xml").write_bytes(ui_bytes)
        (enc_pack / "gamedata" / "configs" / "text" / "rus" / "cp_ref.xml").write_bytes(
            ('<?xml version="1.0" encoding="windows-1251"?><string_table>'
             '<string id="r"><text>Русский</text></string></string_table>').encode("cp1251"))
        enc_cn = tmp / "enc2_cn"
        enc_cn.mkdir(parents=True, exist_ok=True)
        bom_xml = ('<?xml version="1.0" encoding="utf-8"?><string_table>'
                   '<string id="t1"><text>译文</text></string></string_table>')
        (enc_cn / "st_test.xml").write_bytes(b"\xef\xbb\xbf" + bom_xml.encode("utf-8"))
        enc_out = tmp / "out_enc2"
        rc_e, so_e = run_tool(enc_pack, CJK_FONT, enc_out, ["--text-root", str(enc_cn)])
        got_tr = (enc_out / "gamedata" / "configs" / "text" / "rus" / "st_test.xml").read_bytes()
        if rc_e == 0 and not got_tr.startswith(b"\xef\xbb\xbf") and "译文".encode("utf-8") in got_tr:
            ok("译文文本 XML 带 BOM ⇒ 产物无 BOM（UTF-8）")
        else:
            problems += 1
            fail("BOM 没去掉（前 3 字节 %r）" % got_tr[:3])
        got_cp = (enc_out / "gamedata" / "configs" / "text" / "rus" / "cp_ref.xml").read_bytes()
        if not got_cp.startswith(b"\xef\xbb\xbf") and "Русский".encode("utf-8") in got_cp:
            ok("补全来的 cp1251 文本 XML ⇒ 也是 UTF-8 无 BOM")
        else:
            problems += 1
            fail("补全的文本 XML 没规范成 UTF-8 无 BOM")
        if (enc_out / "gamedata" / "configs" / "ui" / "ui_ref.xml").read_bytes() == ui_bytes:
            ok("非文本 XML 一个字节都没动")
        else:
            problems += 1
            fail("非文本 XML 被改了（只该动文本 XML）")

        # ② 逐槽位字体计划：letterica 段用另一个字体；只处理 size=14 的段时 letterica 必须不动
        pack2 = build_pack(tmp / "plan", BASE_FONT, TEXT)
        # 两份字体文件都在（build_pack 只造了 arial；补一个 letterica 段）
        (pack2 / "gamedata" / "fonts" / "rus" / "letterica.ttf").write_bytes(
            (pack2 / "gamedata" / "fonts" / "rus" / "arial.ttf").read_bytes())
        ltx2 = (pack2 / "gamedata" / "configs" / "fonts.ltx")
        ltx2.write_text(
            "[font_default]\nshader = hud\\font\nsize = 14\nname = arial\nopentype = false\n"
            "[font_letterica]:font_default\nname = letterica\nsize = 16\n", encoding="utf-8")
        plan_file = tmp / "plan.json"
        plan_file.write_text(
            '{"sections": {"font_letterica": %s}, "sizes": []}' % json.dumps(str(CJK_FONT)),
            encoding="utf-8")
        out_plan = tmp / "out_plan"
        proc = subprocess.run([sys.executable, str(TOOL), "--pack", str(pack2),
                               "--font", str(CJK_FONT), "--out", str(out_plan),
                               "--plan", str(plan_file)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        both = [out_plan / "gamedata" / "fonts" / "rus" / n for n in ("arial.ttf", "letterica.ttf")]
        if proc.returncode == 0 and all(f.is_file() for f in both):
            ok("逐槽位计划：两份字体都产出")
        else:
            problems += 1
            fail("逐槽位计划失败：rc=%d" % proc.returncode)

        out_p14 = tmp / "out_p14"
        subprocess.run([sys.executable, str(TOOL), "--pack", str(pack2),
                        "--font", str(CJK_FONT), "--out", str(out_p14), "--sizes", "14"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        f14 = out_p14 / "gamedata" / "fonts" / "rus" / "letterica.ttf"
        if f14.is_file():
            cmap14 = set(TTFont(str(f14)).getBestCmap())
            if 0x6C49 not in cmap14:
                ok("分区 sizes=14：size=16 的 letterica 段没被动（cmap 里没有「汉」）")
            else:
                problems += 1
                fail("分区没生效：letterica 段（size=16）也被合并了")
        else:
            problems += 1
            fail("分区产物缺 letterica.ttf")

        # ②b 多语言：造一个双语言包（fonts/rus + fonts/eng），--all-langs 必须出两份
        pack3 = build_pack(tmp / "multi", BASE_FONT, TEXT)
        for lang in ("rus", "eng"):
            (pack3 / "gamedata" / "fonts" / lang).mkdir(parents=True, exist_ok=True)
            (pack3 / "gamedata" / "fonts" / lang / "arial.ttf").write_bytes(
                (pack3 / "gamedata" / "fonts" / "rus" / "arial.ttf").read_bytes())
            (pack3 / "gamedata" / "configs" / "text" / lang).mkdir(parents=True, exist_ok=True)
            (pack3 / "gamedata" / "configs" / "text" / lang / "t.xml").write_text(
                '<?xml version="1.0" encoding="utf-8"?><string_table>'
                '<string id="a"><text>你好</text></string></string_table>', encoding="utf-8")
        out_multi = tmp / "out_multi"
        proc = subprocess.run([sys.executable, str(TOOL), "--pack", str(pack3),
                               "--font", str(CJK_FONT), "--out", str(out_multi),
                               "--all-langs", "--split-output"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        both = [out_multi / lang / "gamedata" / "fonts" / lang / "arial.ttf"
                for lang in ("rus", "eng")]
        if proc.returncode == 0 and all(f.is_file() for f in both):
            ok("多语言：--all-langs --split-output 出了 rus / eng 两份")
        else:
            problems += 1
            fail("多语言没出两份：rc=%d\n%s"
                 % (proc.returncode, proc.stdout.decode("utf-8", "replace")[-300:]))

        # ②c 多语言没给 --split-output ⇒ 必须拒绝（不许互相覆盖）
        proc = subprocess.run([sys.executable, str(TOOL), "--pack", str(pack3),
                               "--font", str(CJK_FONT), "--out", str(tmp / "out_bad"),
                               "--all-langs"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode == 1 and "split-output" in proc.stdout.decode("utf-8", "replace"):
            ok("多语言不给 --split-output：退 1 且说理由")
        else:
            problems += 1
            fail("多语言没有 split-output 保护：rc=%d" % proc.returncode)

        # ③ 零静默：给一个没有中文的字体当源 ⇒ 必须把缺字记进报告（不是静默成功）
        out2 = tmp / "out2"
        rc2, stdout2 = run_tool(pack, BASE_FONT, out2)   # arial 无中文
        rep2 = report_of(stdout2)
        if rep2.get("missing", 0) > 0:
            ok("零静默：无中文字体作源 ⇒ 报告 missing=%d（不假装成功）" % rep2["missing"])
        else:
            problems += 1
            fail("缺字没有入账：rep2=%r" % rep2)

        # ③ 不是字体包 ⇒ 退码 1
        bad = tmp / "bad"
        (bad / "gamedata" / "configs").mkdir(parents=True)
        (bad / "gamedata" / "configs" / "localization.ltx").write_text(
            "[string_table]\nlanguage = rus\n", encoding="utf-8")
        rc3, stdout3 = run_tool(bad, CJK_FONT, tmp / "out3")
        if rc3 == 1 and "不是字体包" in stdout3:
            ok("不是字体包：退码 1 且说出缺什么（零静默）")
        else:
            problems += 1
            fail("非字体包没被拒：rc=%d\n%s" % (rc3, stdout3[-300:]))
        # ④ **真包夹具**（用户口径：判据以 MODS 下三个真实字体包为夹具）：
        #   拿 ~NotoSansSC 当参照包生成一遍，核"逐字节保留 + 能出字体"。
        # ★★ 2026-10-27：MODS 挪到游戏目录上一级、且可能带 `~` 前缀 ⇒ 用候选根解析
        candidates = [
            Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat\MODS\NotoSansSC"),
            Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat\MODS\~NotoSansSC"),
            Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat"
                 r"\Chronicles of Pripyat 4.26 IX-Ray\MODS\~NotoSansSC"),
        ]
        real = next((c for c in candidates if c.is_dir()), Path("__missing__"))
        if real.is_dir():
            out_real = tmp / "out_real"
            rc9, stdout9 = run_tool(real, CJK_FONT, out_real)
            base_ltx = (real / "gamedata" / "configs" / "fonts.ltx").read_bytes()
            out_ltx = (out_real / "gamedata" / "configs" / "fonts.ltx")
            fonts = list((out_real / "gamedata" / "fonts" / "rus").glob("*.ttf"))
            if rc9 == 0 and fonts:
                ok("真包 ~NotoSansSC：生成成功、产出 %d 个 ttf" % len(fonts))
            else:
                problems += 1
                fail("真包生成失败：rc=%d\n%s" % (rc9, stdout9[-400:]))
            # 2026-10-27 新口径：产物只有 gamedata/fonts ⇒ 输出里不该有 configs；
            #   "没被改"这条改成验**参照包原文件**（只许读）。
            # 2026-10-28 照镜子口径：产物**必须**带 fonts.ltx，且与参照包逐字节一致
            if out_ltx.is_file() and out_ltx.read_bytes() == base_ltx:
                ok("真包：产物带 fonts.ltx 且逐字节一致（字体节引用才成立）")
            else:
                problems += 1
                fail("真包：产物缺 fonts.ltx 或被改（游戏会按别的字体节渲染）")
            if (real / "gamedata" / "configs" / "fonts.ltx").read_bytes() == base_ltx:
                ok("真包：参照包的 fonts.ltx 逐字节未改")
            else:
                problems += 1
                fail("真包：参照包的 fonts.ltx 被改了")
            sample_text = list((real / "gamedata" / "configs" / "text" / "rus").glob("*.xml"))[:1]
            if sample_text:
                rel = sample_text[0].relative_to(real)
                copied = out_real / rel
                if copied.is_file() and copied.read_bytes() == sample_text[0].read_bytes():
                    ok("真包：产物带文本且逐字节一致（%s）" % rel.name)
                else:
                    problems += 1
                    fail("真包：产物缺文本（或改动）%s" % rel)
        else:
            print("!! 真包 ~NotoSansSC 不在（跳过真包夹具）")
            print("IXR-GEN: SKIP (real-packs-missing)")
            return 2
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("IXR-GEN: FAIL（%d 处）" % problems)
        return 1
    print("IXR-GEN: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
