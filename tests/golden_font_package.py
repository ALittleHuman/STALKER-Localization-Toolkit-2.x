#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""出包端到端锁（M1-7 第⑦步）：2.x `--build-font-pack` vs 1.x `build_package`。

判据：**整棵产物树逐字节一致** —— 10 个 `ui_font_NN<suffix>.dds/.ini`、
`config(s)/text/<lang>/*.xml`（无 BOM UTF-8 + 声明改 utf-8）、`fonts.ltx`、`localization.ltx`。

输入：一个装有翻译 XML 的目录（用真语料：门禁文本提取步产出的 `*_texts.xml`；没有就
退回合成语料）。三套游戏版本各跑一遍（`SoC` 用 `config` + 8888，`ClS` 用 `configs` + 8888，
`CoP` 用 `configs` + A8）—— 这条把 `GAMES` 表、目录名、DDS 格式也一起验了。

    python tests/golden_font_package.py [--game SoC] [--font <路径>]
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_package"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
FONT_PACK_PY = TOOLKIT_1X / "font_pack" / "font_pack.py"
DEFAULT_FONT = Path(r"E:\Software\Games\STALKER\Localization\Tools\otherTools"
                    r"\CN_Pack_Generator\msyh.ttf")
REAL_XML = ROOT / "build" / "golden_text_extract" / "real-gameplay" / "ours"
GAMES = ("SoC", "ClS", "CoP")


def load_reference():
    spec = importlib.util.spec_from_file_location("font_pack_1x", FONT_PACK_PY)
    if spec is None or spec.loader is None:
        raise RuntimeError("装载失败：%s" % FONT_PACK_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_xml_dir() -> Path:
    """装翻译 XML 的目录：真语料（顶层那几个）+ 若干编码/声明边界的合成文件。

    ★ 1.x 的 `build_package` 只扫**顶层** XML（不递归），所以这里只放顶层文件。
    """
    d = WORK / "xml_in"
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    srcs = sorted(REAL_XML.glob("*.xml")) if REAL_XML.is_dir() else []
    for s in srcs[:2]:
        shutil.copy2(s, d / s.name)
    # 声明的三种写法 + 无声明 + BOM + 无 XML 声明
    (d / "decl_utf8.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table><string id="a">'
        '<text>声明 utf-8 的中文</text></string></string_table>\n', encoding="utf-8")
    (d / "decl_gb2312.xml").write_bytes(
        '<?xml version="1.0" encoding="gb2312"?>\n<string_table><string id="b">'
        '<text>声明 gb2312 的汉字</text></string></string_table>\n'.encode("gb18030"))
    (d / "decl_1251.xml").write_bytes(
        '<?xml version="1.0" encoding="windows-1251"?>\n<string_table><string id="c">'
        '<text>Объявление 1251</text></string></string_table>\n'.encode("windows-1251"))
    (d / "no_decl_1251.xml").write_bytes(
        '<string_table><string id="d"><text>Без объявления</text></string></string_table>\n'
        .encode("windows-1251"))
    (d / "decl_none.xml").write_text(
        '<string_table><string id="e"><text>完全没有 XML 声明</text></string></string_table>\n',
        encoding="utf-8")
    (d / "bom_utf8.xml").write_bytes(
        b"\xef\xbb\xbf" + '<?xml version="1.0" encoding="utf-8"?>\n<string_table>'
        '<string id="f"><text>带 BOM 的文件</text></string></string_table>\n'.encode("utf-8"))
    if not srcs:
        (d / "synthetic.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
            '\t<string id="a"><text>合成语料：汉字与西里尔 Привет，标点！？</text></string>\n'
            '</string_table>\n', encoding="utf-8")
    return d


def tree_digest(root: Path):
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[str(p.relative_to(root)).replace("\\", "/")] = hashlib.sha256(
                p.read_bytes()).hexdigest()
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="出包端到端一致性锁")
    parser.add_argument("--font", default=str(DEFAULT_FONT))
    parser.add_argument("--game", default="", help="只跑某个游戏版本")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-PACKAGE: SKIP (cli-missing)")
        return 2
    if not FONT_PACK_PY.is_file():
        print("!! 找不到 1.x 的 %s" % FONT_PACK_PY)
        print("GOLDEN-FONT-PACKAGE: SKIP (toolkit-1x-missing)")
        return 2
    font_path = Path(args.font)
    if not font_path.is_file():
        print("!! 找不到字体 %s" % font_path)
        print("GOLDEN-FONT-PACKAGE: SKIP (font-missing)")
        return 2

    fp = load_reference()
    WORK.mkdir(parents=True, exist_ok=True)
    xml_dir = make_xml_dir()
    print("  语料：%s（%d 个 XML）"
          % (xml_dir.relative_to(ROOT), len(list(xml_dir.glob("*.xml")))))

    failures = 0
    games = (args.game,) if args.game else GAMES
    for game in games:
        ours_dir = WORK / ("ours_" + game)
        ref_dir = WORK / ("ref_" + game)
        for d in (ours_dir, ref_dir):
            if d.exists():
                shutil.rmtree(d)
            d.mkdir(parents=True)
        proc = subprocess.run([str(CLI), "--build-font-pack", game, "chs", str(xml_dir),
                               str(font_path), str(ours_dir), "--suffix", "_chs"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            failures += 1
            print("  ✗ %s：CLI 退出码 %d —— %s"
                  % (game, proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
            continue
        try:
            fp.build_package(game, "chs", str(xml_dir), str(font_path), str(ref_dir),
                             suffix="_chs", log=None)
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print("  ✗ %s：1.x 基准失败 %s" % (game, exc))
            continue

        ours = tree_digest(ours_dir / "gamedata")
        ref = tree_digest(ref_dir / "gamedata")
        only_ours = sorted(set(ours) - set(ref))
        only_ref = sorted(set(ref) - set(ours))
        bad = [k for k in sorted(set(ours) & set(ref)) if ours[k] != ref[k]]
        if not only_ours and not only_ref and not bad:
            print("  ✓ %s：整棵产物树 %d 个文件逐字节一致" % (game, len(ref)))
            continue
        failures += 1
        print("  ✗ %s：产物不一致（我们 %d / 1.x %d 个文件）" % (game, len(ours), len(ref)))
        for k in only_ours[:10]:
            print("      只在我们：%s" % k)
        for k in only_ref[:10]:
            print("      只在 1.x：%s" % k)
        for k in bad[:10]:
            a = (ours_dir / "gamedata" / k).read_bytes()
            b = (ref_dir / "gamedata" / k).read_bytes()
            off = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]),
                       -1 if len(a) == len(b) else min(len(a), len(b)))
            print("      不同：%s（我们 %d B / 1.x %d B，首个不同 @%s）"
                  % (k, len(a), len(b), off))

    # ── 字体守卫：拿一个**没有中日韩字形**的字体（tahoma），两边都必须**拒绝**，
    #    而且**不留半成品 gamedata**（1.x 的守卫跑在建目录之前）。
    guard_font = Path(r"C:\Windows\Fonts\tahoma.ttf")
    if guard_font.is_file():
        guard_dir = WORK / "guard_ours"
        if guard_dir.exists():
            shutil.rmtree(guard_dir)
        guard_dir.mkdir(parents=True)
        proc = subprocess.run([str(CLI), "--build-font-pack", "SoC", "chs", str(xml_dir),
                               str(guard_font), str(guard_dir), "--suffix", "_chs"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        ours_rejected = proc.returncode != 0 and not (guard_dir / "gamedata").exists()
        ref_dir = WORK / "guard_ref"
        if ref_dir.exists():
            shutil.rmtree(ref_dir)
        ref_dir.mkdir(parents=True)
        ref_rejected = False
        try:
            fp.build_package("SoC", "chs", str(xml_dir), str(guard_font), str(ref_dir),
                             suffix="_chs", log=None)
        except Exception:  # noqa: BLE001
            ref_rejected = not (ref_dir / "gamedata").exists()
        if ours_rejected and ref_rejected:
            print("  ✓ 字体守卫：无中日韩字形的字体两边都拒绝，且都不留半成品 gamedata")
        else:
            failures += 1
            print("  ✗ 字体守卫：我们拒绝=%s / 1.x 拒绝=%s"
                  % (ours_rejected, ref_rejected))
    else:
        print("  (跳过字体守卫检查：没有 %s)" % guard_font)

    print("")
    print("GOLDEN-FONT-PACKAGE: %s (games=%d failed=%d)"
          % ("FAIL" if failures else "PASS", len(games), failures))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
