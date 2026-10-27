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


def run_tool(pack: Path, font: Path, out: Path) -> tuple[int, str]:
    proc = subprocess.run([sys.executable, str(TOOL), "--pack", str(pack),
                           "--font", str(font), "--out", str(out)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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

        # ① 逐字节保留：ltx 与文本都不许被改
        if merged.is_file():
            if (out / "gamedata" / "configs" / "fonts.ltx").read_bytes() == ltx_before:
                ok("fonts.ltx 逐字节未改（「指定」模式只换字体）")
            else:
                problems += 1
                fail("fonts.ltx 被改了（「指定」模式必须原样保留）")
            if (out / "gamedata" / "configs" / "text" / "rus" / "st_test.xml").read_bytes() == text_before:
                ok("文本逐字节未改")
            else:
                problems += 1
                fail("文本被改了")

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
