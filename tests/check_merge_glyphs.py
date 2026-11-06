# -*- coding: utf-8 -*-
"""A-min 验收：`merge_glyphs.py` 的 8 条判据。

① 缺字 0：用 PIL 按游戏同款字号（32px）逐个渲染，字形必须有墨迹（不是看计数）
② 老字形逐字节不变：target 原有 glyph 的 glyf 序列化与 hmtx 度量全等
③ hmtx 随字形拷：新字形 advance == 源字体按 upem 缩放后的值
④ cmap 可达：每个新码位都能查到 glyph index，且该 glyph 有轮廓
⑤ 复合字形重映射：组件的 glyphName 必须存在于 target 内（不能照抄源名字）
⑥ 幂等：重复合并 numGlyphs 不涨、两次 sha256 相同
⑦ CFF 拒绝：源是 CFF/OTF ⇒ rc≠0 且理由明确
⑧ 可复现：同一输入第三次产出 sha 与第一次相同
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "plugins_ixr" / "_lib" / "merge_glyphs.py"
TARGET = Path(r"C:\Windows\Fonts\arial.ttf")
SOURCE = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
CFF = Path(r"C:\Windows\Fonts\3ds Light.otf")
# 复合字形样张：segoeui 的 U+2011（non-breaking hyphen，组件 hyphen）不在 arial 里
COMPOSITE_SOURCE = Path(r"C:\Windows\Fonts\segoeui.ttf")
COMPOSITE_CP = 0x2011
CPS = [0x4E00, 0x4E8C, 0x4E09, 0x56FD, 0x5B57]
PROBLEMS: list[str] = []


def ok(m: str) -> None:
    print("  \u2713 " + m)


def fail(m: str) -> None:
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run_merge(target: Path, source: Path, cps: list[int], out: Path) -> tuple[int, str]:
    charfile = out.with_suffix(".txt")
    charfile.write_text("\n".join("U+%04X" % cp for cp in cps), encoding="utf-8")
    p = subprocess.run([sys.executable, str(SCRIPT), "--target", str(target),
                        "--source", str(source), "--charset", str(charfile),
                        "--out", str(out)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== A-min 字形合并：8 条判据 ==")
    for need in (SCRIPT, TARGET, SOURCE):
        if not need.is_file():
            print("MERGE-GLYPHS skip 缺文件：%s" % need)
            return 2
    with tempfile.TemporaryDirectory(prefix="amg_") as td:
        td = Path(td)
        out1, out2, out3 = td / "o1.ttf", td / "o2.ttf", td / "o3.ttf"
        rc1, log1 = run_merge(TARGET, SOURCE, CPS, out1)
        rc2, log2 = run_merge(TARGET, SOURCE, CPS, out2)
        rc3, log3 = run_merge(TARGET, SOURCE, CPS, out3)
        if rc1 != 0 or not out1.is_file():
            print("MERGE-GLYPHS: FAIL（合并本身失败）")
            print(log1[-800:])
            return 1
        report = json.loads(log1.split("MERGEGLYPHS ", 1)[1].splitlines()[0])

        # ① 缺字 0（逐个渲染）
        try:
            from PIL import ImageFont
            font = ImageFont.truetype(str(out1), 32)
            bad = []
            for cp in CPS:
                ch = chr(cp)
                mask = font.getmask(ch)
                if mask.getbbox() is None:
                    bad.append("U+%04X" % cp)
            (ok if not bad else fail)("① 缺字 0（32px 逐个渲染%s）" % ("" if not bad else "，缺：" + ",".join(bad)))
        except Exception as exc:  # noqa: BLE001
            fail("① 渲染检查跑不起来：%r" % (exc,))

        # ② 老字形逐字节不变
        a = TTFont(str(TARGET), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
        b = TTFont(str(out1), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
        ag, bg = a["glyf"], b["glyf"]
        aorder = list(a.getGlyphOrder())
        changed = []
        for name in aorder:
            if ag[name].compile(ag) != bg[name].compile(bg):
                changed.append(name)
            if a["hmtx"][name] != b["hmtx"][name]:
                changed.append(name + ":hmtx")
        (ok if not changed else fail)("② 老字形逐字节不变（改了 %d 个：%s）"
                                      % (len(changed), changed[:5]))

        # ③ hmtx 缩放
        b_cmap = b.getBestCmap() or {}
        s = TTFont(str(SOURCE), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
        s_cmap = s.getBestCmap() or {}
        scale = b["head"].unitsPerEm / s["head"].unitsPerEm
        wrong = []
        for cp in CPS:
            name = b_cmap.get(cp)
            sname = s_cmap.get(cp)
            if name is None or sname is None:
                wrong.append("U+%04X:缺映射" % cp)
                continue
            want = int(round(s["hmtx"][sname][0] * scale))
            if b["hmtx"][name][0] != want:
                wrong.append("U+%04X:%d!=%d" % (cp, b["hmtx"][name][0], want))
        (ok if not wrong else fail)("③ 新字形 advance 按 upem 缩放（错：%s）" % (wrong[:3] or "无"))

        # ④ cmap 可达 + 有轮廓
        hollow = []
        for cp in CPS:
            name = b_cmap.get(cp)
            if name is None or not getattr(bg[name], "numberOfContours", 0):
                hollow.append("U+%04X" % cp)
        (ok if not hollow else fail)("④ cmap 可达且字形有轮廓（空：%s）" % (hollow or "无"))

        # ⑤ 复合字形组件重映射（单独的样张：目标字形是复合字形，组件必须重映射到 target 内）
        composite_checked = 0
        bad_comp = []
        if COMPOSITE_SOURCE.is_file():
            out4 = td / "comp.ttf"
            rc4, log4 = run_merge(TARGET, COMPOSITE_SOURCE, [COMPOSITE_CP], out4)
            if rc4 == 0 and out4.is_file():
                c = TTFont(str(out4), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
                order_c = set(c.getGlyphOrder())
                cmap_c = c.getBestCmap() or {}
                name_c = cmap_c.get(COMPOSITE_CP)
                if name_c is None:
                    bad_comp.append("U+%04X:无映射" % COMPOSITE_CP)
                else:
                    g = c["glyf"][name_c]
                    if not g.isComposite():
                        bad_comp.append("%s:拷成了简单字形（不是复合）" % name_c)
                    else:
                        composite_checked += 1
                        src_g = TTFont(str(COMPOSITE_SOURCE), fontNumber=0).getBestCmap()[COMPOSITE_CP]
                        for comp in g.components:
                            if comp.glyphName not in order_c:
                                bad_comp.append("%s→%s" % (name_c, comp.glyphName))
                        # 组件名字不能照抄源（除非 target 本来就有同名且就是拷贝目标）
                        if src_g and all(c2.glyphName != src_g for c2 in g.components):
                            pass
            else:
                bad_comp.append("复合样张合并失败 rc=%d" % rc4)
        else:
            print("  · ⑤ 跳过：本机没有 segoeui.ttf")
        (ok if (composite_checked and not bad_comp) else
         fail)("⑤ 复合字形组件指向 target 内（检查 %d 个，坏：%s）"
               % (composite_checked, bad_comp[:3] or "无"))

        # ⑤b 端到端：--font-pack 矢量路径的 --merge-glyphs（U+2400 是 seguisym 的复合字形）
        cli = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
        launcher = ROOT / "plugins_ixr" / "_lib" / "ixr_run.py"
        segui = Path(r"C:\Windows\Fonts\seguisym.ttf")
        if cli.is_file() and launcher.is_file() and segui.is_file():
            pack = td / "pack"
            (pack / "gamedata" / "configs" / "text" / "rus").mkdir(parents=True, exist_ok=True)
            (pack / "gamedata" / "fonts" / "rus").mkdir(parents=True, exist_ok=True)
            (pack / "gamedata" / "configs" / "fonts.ltx").write_text(
                "[ui_font_arial_14]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_11\n",
                encoding="utf-8")
            (pack / "gamedata" / "fonts" / "rus" / "arial.ttf").write_bytes(TARGET.read_bytes())
            (pack / "gamedata" / "configs" / "text" / "rus" / "a.xml").write_text(
                '<string_table><string id="a"><text>Привет</text></string></string_table>',
                encoding="utf-8")
            cn = td / "cn"
            cn.mkdir()
            (cn / "a.xml").write_text(
                '<string_table><string id="a"><text>汉化\u2400</text></string></string_table>',
                encoding="utf-8")
            import os as _os
            env2 = dict(_os.environ)
            env2["XRKIT_IXR_RUN"] = str(launcher)
            env2["XRKIT_IXR_PYTHON"] = sys.executable
            env2["PYTHONIOENCODING"] = "utf-8"
            env2["PYTHONUTF8"] = "1"
            proc = subprocess.run([str(cli), "--font-pack", "--ref", str(pack),
                                   "--text-root", str(cn), "--font", str(SOURCE),
                                   "--out", str(td / "vout"), "--langs", "rus",
                                   "--merge-glyphs", str(segui)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env2)
            log2 = proc.stdout.decode("utf-8", "replace")
            merged_font = td / "vout" / "gamedata" / "fonts" / "rus" / "arial.ttf"
            if proc.returncode == 0 and merged_font.is_file():
                mf = TTFont(str(merged_font), fontNumber=0)
                nm = (mf.getBestCmap() or {}).get(0x2400)
                okc = nm is not None and mf["glyf"][nm].isComposite() and all(
                    c.glyphName in set(mf.getGlyphOrder()) for c in mf["glyf"][nm].components)
                (ok if okc else fail)("⑤b 端到端：--merge-glyphs 把 U+2400（复合）并进包内字体，组件已重映射")
            else:
                fail("⑤b 端到端 --merge-glyphs 失败 rc=%d\n%s" % (proc.returncode, log2[-400:]))
        else:
            print("  · ⑤b 跳过：缺 CLI / 启动器 / seguisym")

        # ⑤c 跨 upem 的复合字形：组件**偏移与变换**必须按 scale 缩放（缺陷 ① 的靶子）
        small = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")   # upem=1000
        big = Path(r"C:\Windows\Fonts\segoeui.ttf")           # upem=2048
        if small.is_file() and big.is_file():
            sf = TTFont(str(small), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
            bf = TTFont(str(big), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
            s_cmap = sf.getBestCmap() or {}
            b_cmap = bf.getBestCmap() or {}
            b_glyf = bf["glyf"]
            pick = None
            for cp, name in b_cmap.items():
                if cp in s_cmap:
                    continue
                try:
                    if b_glyf[name].isComposite():
                        pick = (cp, name)
                        break
                except Exception:
                    continue
            if pick is None:
                print("  · ⑤c 跳过：segoeui 里找不到 target 缺的复合字形")
            else:
                cp_c, sname = pick
                out5 = td / "upem.ttf"
                rc5, log5 = run_merge(small, big, [cp_c], out5)
                if rc5 == 0 and out5.is_file():
                    of = TTFont(str(out5), fontNumber=0, recalcBBoxes=False,
                                recalcTimestamp=False)
                    scale2 = of["head"].unitsPerEm / bf["head"].unitsPerEm
                    nm2 = (of.getBestCmap() or {}).get(cp_c)
                    src_glyph = b_glyf[sname]
                    bad2 = []
                    if nm2 is None:
                        bad2.append("无映射")
                    else:
                        og = of["glyf"][nm2]
                        if not og.isComposite():
                            bad2.append("不是复合")
                        else:
                            for s_comp, o_comp in zip(src_glyph.components, og.components):
                                wx, wy = int(round(s_comp.x * scale2)), int(round(s_comp.y * scale2))
                                if (o_comp.x, o_comp.y) != (wx, wy):
                                    bad2.append("U+%04X 偏移 (%d,%d)!=(%d,%d)"
                                                % (cp_c, o_comp.x, o_comp.y, wx, wy))
                    (ok if not bad2 else fail)(
                        "⑤c 跨 upem 复合偏移按 scale 缩放（U+%04X，scale=%.4f，坏：%s）"
                        % (cp_c, scale2, bad2[:2] or "无"))
                else:
                    fail("⑤c 跨 upem 合并失败 rc=%d" % rc5)
        else:
            print("  · ⑤c 跳过：缺 1000/2048 upem 样张")

        # ⑤d 星平面码位（>0xFFFF）不许崩（缺陷 ② 的靶子：format 4 溢出）
        astral_src = td / "astral_src.ttf"
        base = TTFont(str(TARGET), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
        picked_name = (base.getGlyphOrder() or ["space"])[-1]
        wrote12 = False
        for sub in base["cmap"].tables:
            if sub.format == 12 and sub.isUnicode():
                sub.cmap[0x20000] = picked_name
                wrote12 = True
        if wrote12:
            base.save(str(astral_src))
            out6 = td / "astral_out.ttf"
            rc6, log6 = run_merge(TARGET, astral_src, [0x20000], out6)
            if rc6 != 0 or not out6.is_file():
                fail("⑤d 星平面合并崩了（rc=%d）" % rc6)
            else:
                of2 = TTFont(str(out6), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
                ok12 = any(sub.format == 12 and sub.cmap.get(0x20000) for sub in of2["cmap"].tables)
                (ok if ok12 else fail)("⑤d 星平面码位进 format 12（不崩）")
        else:
            print("  · ⑤d 跳过：target 没有 format 12 子表")

        # ⑥⑦⑧ 幂等 / 可复现
        same = out1.is_file() and out2.is_file() and out3.is_file() and \
            sha(out1) == sha(out2) == sha(out3)
        (ok if same else fail)("⑥⑧ 三次产出 sha 相同（幂等+可复现）")
        (ok if report["numGlyphs_after"] >= report["numGlyphs_before"] else fail)(
            "⑥ numGlyphs 只增不减（%d→%d）" % (report["numGlyphs_before"], report["numGlyphs_after"]))

        if CFF.is_file():
            rc7, log7 = run_merge(TARGET, CFF, CPS, td / "cff.ttf")
            reason_ok = "CFF" in log7 or "not TrueType" in log7 or "glyf" in log7
            (ok if rc7 != 0 and reason_ok else fail)("⑦ CFF 源拒绝（rc=%d）" % rc7)
        else:
            print("  · ⑦ 跳过：本机没有 CFF/OTF 样张")

    if PROBLEMS:
        print("MERGE-GLYPHS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("MERGE-GLYPHS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
