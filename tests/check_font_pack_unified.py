# -*- coding: utf-8 -*-
"""最小统一判据（--font-pack）：段表来源、writer 选择、缺字策略、状态隔离。

覆盖：
  ① `--ref builtin` ⇒ `SEGMENTS source=builtin n=15` + `writer=bitmap` + 出包；
  ② `--ref <参照包>` ⇒ `SEGMENTS source=ref n=36`（段表来自扫描，不是内置模板）；
  ③ 状态隔离：跑完 ref 之后，builtin 产物与之前逐字节相同（共享核心不得串状态）；
  ④ `--on-missing fail`（默认）语料缺字 ⇒ rc≠0；`warn` ⇒ rc=0 且打 `MISSING used=N`。
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
CJK = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
LATIN = Path(r"C:\Windows\Fonts\arial.ttf")
def _find_packs():
    """任意"含 gamedata/configs/fonts.ltx 的目录"都算参照包（**不点名某个模组**）。"""
    roots = [Path("E:/Software/Games/STALKER/Games/Chronicles of Pripyat/Chronicles of Pripyat")]
    found = []
    for root in roots:
        if not root.is_dir():
            continue
        for ltx in root.glob("*/MODS/*/gamedata/configs/fonts.ltx"):
            found.append(ltx.parent.parent.parent)
        for ltx in root.glob("*/MODS/*/*/gamedata/configs/fonts.ltx"):
            found.append(ltx.parent.parent.parent)
        for ltx in root.glob("MODS/*/gamedata/configs/fonts.ltx"):
            found.append(ltx.parent.parent.parent)
    return found[:1]


PACKS = _find_packs()
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    # 开发树里矢量路径需要显式解释器/启动器（发行包自带，才不需要）
    env = os.environ.copy()
    launcher = ROOT / "plugins_ixr" / "_lib" / "ixr_run.py"
    if launcher.is_file():
        env.setdefault("XRKIT_IXR_PYTHON", sys.executable)
        env.setdefault("XRKIT_IXR_RUN", str(launcher))
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def tree_hash(root: Path) -> str:
    h = hashlib.sha256()
    for f in sorted(root.rglob("*")):
        if f.is_file():
            h.update(f.relative_to(root).as_posix().encode())
            h.update(f.read_bytes())
    return h.hexdigest()


def main() -> int:
    print("== 最小统一：--font-pack 段表/ writer / 缺字策略 ==")
    if not CLI.is_file():
        print("FONT-PACK-UNIFIED skip 缺 CLI")
        return 2
    if not CJK.is_file():
        print("FONT-PACK-UNIFIED skip 缺系统字体")
        return 2
    with tempfile.TemporaryDirectory(prefix="fpu_") as td:
        td = Path(td)
        xml = td / "xml"
        xml.mkdir()
        # 语料：中文 + 西里尔（位图 writer 需要 CJK 字体；NotoSansSC 有 CJK 无西里尔 ⇒ 基线警告）
        (xml / "a.xml").write_text(
            '<string_table><string id="a"><text>汉化 Привет</text></string></string_table>',
            encoding="utf-8")

        rc1, out1 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", CJK, "--out", td / "b1")
        (ok if "SEGMENTS source=builtin n=15" in out1 else fail)("① builtin 段数 15")
        (ok if rc1 == 0 and "writer=bitmap" in out1 else fail)("① builtin ⇒ 位图 writer 出包")

        rc2, out2 = run("--font-pack", "--ref", PACKS[0], "--text-root", xml, "--font", CJK,
                        "--out", td / "v1", "--langs", "rus")
        gone = not PACKS[0].is_dir()
        if gone:
            print("  · 参照包不在（跳 ②③）")
        else:
            (ok if "SEGMENTS source=ref n=36" in out2 else fail)("② 参照包 ⇒ 段数 36（扫描真的生效）")
            # ②a 矢量路径不读 game：喂一个非法档位也必须照常（段表来自参照包）
            rc_g, out_g = run("--font-pack", "--ref", PACKS[0], "--game", "Bogus",
                              "--text-root", xml, "--font", CJK, "--out", td / "v_bogus",
                              "--langs", "rus")
            (ok if "SEGMENTS source=ref n=36" in out_g else fail)(
                "②a 矢量路径忽略 --game（喂 Bogus 仍 source=ref n=36）")
            # ②b 同一文本根 ⇒ CHARSET count 相同（字符集来自同一份核心）
            import re as _re
            m1 = _re.search(r"CHARSET count=(\d+)", out1)
            m2 = _re.search(r"CHARSET count=(\d+)", out2)
            same_cs = bool(m1 and m2 and m1.group(1) == m2.group(1))
            (ok if same_cs else fail)("②b 两路 CHARSET count 相同（%s vs %s）" %
                                      (m1.group(1) if m1 else "?", m2.group(1) if m2 else "?"))
            # ③ 状态隔离：再跑一次 builtin，产物与第一次逐字节相同
            rc3, out3 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                            "--text-root", xml, "--font", CJK, "--out", td / "b2")
            same = (rc3 == 0 and (td / "b1").is_dir() and (td / "b2").is_dir() and
                    tree_hash(td / "b1") == tree_hash(td / "b2"))
            (ok if same else fail)("③ 跑完 ref 后 builtin 产物逐字节相同（状态隔离）")

        # ⑥ JSGME 摆盘：--jsgme ⇒ <out>/MODS/<分类>/<名>/gamedata + JSGME.exe 在 <out>/
        rc_j, out_j = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--langs", "rus",
                          "--text-root", xml, "--font", CJK, "--out", td / "jsgme",
                          "--jsgme", "--jsgme-name", "rus")
        js_out = td / "jsgme"
        mod = js_out / "MODS" / "rus" / "gamedata"
        (ok if rc_j == 0 and mod.is_dir() else fail)(
            "⑥ --jsgme ⇒ MODS/<分类>/<名>/gamedata（rc=%d）" % rc_j)
        (ok if (js_out / "JSGME.exe").is_file() else fail)("⑥ JSGME.exe 复制到输出根")
        (ok if not (js_out / "gamedata").exists() else fail)("⑥ 顶层 gamedata 已移入 MODS")
        (ok if any(p.suffix == ".dds" for p in mod.rglob("*")) else fail)("⑥ 字体产物在 MODS 内")

        # ⑦ 缺字必须**强烈提示**且给出处置办法（用户 2026-11-08：
        #    "缺字不要紧，要紧的是缺了字怎么办"）—— 用一个明显缺西里尔的字体（arial）。
        if not gone:
            # 用一个**字体确实没有**的字符（U+20BD ₽）：NotoSansSC 没有它 ⇒ 必然缺字
            warn_xml = td / "warn_xml"
            warn_xml.mkdir(parents=True, exist_ok=True)
            (warn_xml / "w.xml").write_text(
                '<string_table><string id="a"><text>\u4f60\u597d \u20bd</text></string>'
                '</string_table>', encoding="utf-8")
            rc_w, out_w = run("--font-pack", "--ref", PACKS[0], "--text-root", warn_xml,
                              "--font", CJK, "--out", td / "warn", "--langs", "rus",
                              "--on-missing", "warn")
            (ok if rc_w == 0 and "!!! 缺字" in out_w and "怎么办" in out_w and
                   "--merge-glyphs" in out_w
             else fail)("⑦ 缺字（warn 档）⇒ rc=0 且 CLI 强提示 + 三条处置（rc=%d / 有提示=%d）"
                        % (rc_w, 1 if "!!! 缺字" in out_w else 0))

        # ⑤ 入口：--merge-glyphs 只对矢量有效；位图传了必须**明确报错**
        rc_m, out_m = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--langs", "rus",
                          "--text-root", xml, "--font", CJK, "--out", td / "mrg_bad",
                          "--merge-glyphs", CJK)
        (ok if rc_m == 3 and "只对矢量路径有效" in out_m else fail)(
            "⑤ 位图路径传 --merge-glyphs ⇒ rc=3 + 明确理由（实得 rc=%d）" % rc_m)
        rc_h, out_h = run("--help")
        (ok if "--merge-glyphs" in out_h else fail)("⑤ --help 列出 --merge-glyphs（仅矢量有效）")
        rc_a, out_a = run("--actions")
        (ok if "--merge-glyphs" in out_a else fail)("⑤ --actions 的 spec 含 --merge-glyphs")

        # ④ 缺字策略：arial（无 CJK）对中文语料
        (xml / "b.xml").write_text(
            '<string_table><string id="b"><text>汉化</text></string></string_table>', encoding="utf-8")
        rc4, out4 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", LATIN, "--out", td / "m1")
        (ok if rc4 != 0 and "FONTPACK error" in out4 else fail)(
            "④ 默认 fail：语料缺字 ⇒ 不出包（rc=%d）" % rc4)
        rc5, out5 = run("--font-pack", "--ref", "builtin", "--game", "CoP", "--lang", "rus",
                        "--text-root", xml, "--font", LATIN, "--out", td / "m2",
                        "--on-missing", "warn")
        (ok if rc5 == 0 and "MISSING used=" in out5 else fail)(
            "④ --on-missing warn：继续出包且打 MISSING used=N（rc=%d）" % rc5)

    if PROBLEMS:
        print("FONT-PACK-UNIFIED: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FONT-PACK-UNIFIED: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
