# -*- coding: utf-8 -*-
"""判据：**扫描参照包也能出位图包**（2026-11-21 用户口径）。

修前事实：`--ref <目录>` 一律走矢量（`ixr_build`），位图只能用 `--ref builtin` 的内置模板。
对 SoC/ClS/CoP 这三个**位图**游戏，`--ref 目录` 语义就是错的；扫描对位图路径完全没用。

修后：writer 与"段表来源"解耦 ——
  · 自动识别：ref 里 `.ttf` ⇒ 矢量；`ui_font_*.dds` ⇒ 位图；两者都有/都没有 ⇒ 报错要 `--writer`；
  · 位图 writer 用**参照包的 `fonts.ltx` 原文**逐字节套模板（不再用内置文本）。

本判据钉四条：
  ① SoC 形状的 ref（fonts.ltx + ui_font_*.dds）⇒ `writer=bitmap`，产物 fonts.ltx 与 ref **逐字节相同**；
  ② 只有 `.ttf` 的 ref ⇒ 走**矢量**（开发树缺 IXR python，报的是工具链错误，**不是**"未知游戏/位图"）；
  ③ `.ttf` 与 `ui_font_*.dds` 同时存在 ⇒ 明确报错，提示 `--writer bitmap|vector`（不猜）；
  ④ `--writer bitmap` 在混合 ref 上强制成功（逃生舱）。

用法：python tests/check_fontpack_ref.py   （0=全过 / 1=红 / 2=缺构建产物）
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = Path(os.environ.get("XRKIT_CLI", ROOT / "build" / "msvc-release" / "tools" / "cli" /
                          "xrkit_cli.exe"))

FAILURES: list[str] = []
LTX = ("[ui_font_default]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_11\n\n"
       "[ui_font_arial_21]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_15\n")
XML = '<string_table><string id="a"><text>Привет</text></string></string_table>'


def check(cond: bool, label: str, detail: str = "") -> None:
    if cond:
        print("  \u2713 " + label, flush=True)
    else:
        print("  \u2717 " + label + (("  —— " + detail) if detail else ""), flush=True)
        FAILURES.append(label)


def make_ref(work: Path, name: str, ttf: bool, dds: bool) -> Path:
    ref = work / name
    gd = ref / "gamedata"
    (gd / "configs").mkdir(parents=True)
    (gd / "configs" / "fonts.ltx").write_text(LTX, encoding="utf-8", newline="\n")
    if ttf:
        (gd / "fonts" / "rus").mkdir(parents=True)
        (gd / "fonts" / "rus" / "a.ttf").write_bytes(b"\x00\x01")
    if dds:
        (gd / "textures" / "ui").mkdir(parents=True)
        (gd / "textures" / "ui" / "ui_font_11.dds").write_bytes(bytes(range(16)))
    return ref


def run(args: list[str]) -> str:
    proc = subprocess.run([str(CLI)] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    return proc.stdout.decode("utf-8", "replace")


def main() -> int:
    if not CLI.is_file():
        print("SKIP cli-missing: %s" % CLI)
        return 2
    work = Path(tempfile.mkdtemp(prefix="fontpack_ref_", dir=str(ROOT / "build")))
    try:
        text_root = work / "textrus"
        text_root.mkdir()
        (text_root / "ui_st_mm.xml").write_text(XML, encoding="utf-8", newline="\n")
        base = ["--font-pack", "--text-root", str(text_root), "--game", "SoC", "--lang", "rus"]

        print("== ① 位图 ref：自动识别 + 模板原样套用 ==")
        ref = make_ref(work, "bitmap_ref", ttf=False, dds=True)
        out = work / "out_bitmap"
        text = run(base + ["--ref", str(ref), "--out", str(out)])
        check("writer=bitmap" in text, "① 走位图 writer", text.strip()[-300:])
        produced = out / "gamedata" / "configs" / "fonts.ltx"
        check(produced.is_file(), "① 产出 fonts.ltx", str(produced))
        if produced.is_file():
            got = produced.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
            check(got == LTX, "① fonts.ltx 与参照包**逐字节相同**（不是内置模板）",
                  got[:160])

        print("== ② 只有 .ttf：自动走矢量 ==")
        ref_ttf = make_ref(work, "vector_ref", ttf=True, dds=False)
        text = run(base + ["--ref", str(ref_ttf), "--out", str(work / "out_vector")])
        check("writer=bitmap" not in text, "② 没有误判成位图", text.strip()[-300:])
        check("python" in text.lower() or "ixr" in text.lower(),
              "② 走的是矢量链路（缺 IXR python 时如实报工具链）", text.strip()[-300:])

        print("== ③ 证据冲突：不猜，要求 --writer ==")
        ref_mix = make_ref(work, "mixed_ref", ttf=True, dds=True)
        text = run(base + ["--ref", str(ref_mix), "--out", str(work / "out_mixed")])
        check("writer 证据冲突" in text, "③ 明确报冲突", text.strip()[-300:])
        check("--writer" in text, "③ 提示用 --writer 指定", text.strip()[-300:])

        print("== ④ 强制 writer：混合包也能出位图 ==")
        text = run(base + ["--ref", str(ref_mix), "--out", str(work / "out_forced"),
                           "--writer", "bitmap"])
        check("writer=bitmap" in text, "④ --writer bitmap 生效", text.strip()[-300:])
        print("== ④b 参照包里的**新增字号**：必须生成同名 DDS/INI ==")
        ref_custom = work / "custom_ref"
        gd = ref_custom / "gamedata"
        (gd / "configs").mkdir(parents=True)
        (gd / "configs" / "fonts.ltx").write_text(
            "[custom_12]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_12\n\n"
            "[hud_font_small]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_01\n",
            encoding="utf-8", newline="\n")
        (gd / "textures" / "ui").mkdir(parents=True)
        (gd / "textures" / "ui" / "ui_font_12.dds").write_bytes(bytes(range(8)))
        out_custom = work / "out_custom"
        text = run(base + ["--ref", str(ref_custom), "--out", str(out_custom)])
        check("writer=bitmap" in text, "④b 走位图writer", text.strip()[-300:])
        ui = out_custom / "gamedata" / "textures" / "ui"
        names = sorted(x.name for x in ui.iterdir()) if ui.is_dir() else []
        check("ui_font_12.dds" in names, "④b 生成同名 DDS（ui_font_12.dds）", str(names))
        check("ui_font_12.ini" in names, "④b 生成同名 INI（ui_font_12.ini）", str(names))
        check("ui_font_hud_01" in text, "④b 游戏自带纹理采用并**报出**（不静默）",
              text.strip()[-300:])

        print("== ⑤ 非法 writer 值：明确报错（不是静默忽略） ==")
        text = run(base + ["--ref", str(ref), "--out", str(work / "out_bogus"),
                           "--writer", "nope"])
        check("未知 writer" in text, "⑤ 非法值点名报错", text.strip()[-300:])

        print("== ⑥ builtin + vector：明确拒绝 ==")
        text = run(["--font-pack", "--ref", "builtin", "--text-root", str(text_root),
                    "--game", "SoC", "--lang", "rus", "--out", str(work / "out_bv"),
                    "--writer", "vector"])
        check("不能配" in text and "vector" in text, "⑥ builtin 只有位图模板", text.strip()[-300:])
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if FAILURES:
        print("FONTPACK-REF: FAIL（%d 条）" % len(FAILURES), flush=True)
        return 1
    print("FONTPACK-REF: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
