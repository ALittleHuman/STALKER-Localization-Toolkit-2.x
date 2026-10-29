# -*- coding: utf-8 -*-
"""ori 树判据（交接区《文本提取工具最终规划 2026-10-29》第 1 步）。

口径：
  拖入模组根 → 自动识别容器（`gamedata`、`ixr_addons/<addon>`、任意含
  configs/scripts/gameplay/fonts/textures 的目录）→ 整树复制到 `ori/<容器相对路径>/`
  → 容器内提取 `scripts/` 与 `gameplay/`（改写后回写）→ 文本 XML 落 `<容器>/configs/text/<lang>`
  → `gameplay` 统一落 `configs/gameplay`（容器根的 `gameplay/` 不是游戏读取路径，删掉）
  → 语言取自 `localization.ltx`，不写死 rus。

本文件钉住：容器发现 / 复制完整性 / 提取与改写 / 落点 / 文本逐字节保留 / 输入只读 /
`ori` 非空拒绝 / 单文件输入如实拒绝（下一轮实现）。
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS: list[str] = []


def ok(msg: str) -> None:
    print("  \u2713 " + msg)


def fail(msg: str) -> None:
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def run(argv: list[str], timeout: int = 600) -> tuple[int, str]:
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def snapshot(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def make_mod_root(root: Path) -> None:
    gd = root / "gamedata"
    (gd / "configs" / "text" / "rus").mkdir(parents=True)
    (gd / "fonts" / "rus").mkdir(parents=True)
    (gd / "scripts").mkdir(parents=True)
    (gd / "gameplay").mkdir(parents=True)
    shutil.copy2(Path(r"C:\Windows\Fonts\arial.ttf"), gd / "fonts" / "rus" / "arial.ttf")
    (gd / "configs" / "fonts.ltx").write_text("[font_default]\nname = arial\n", encoding="utf-8")
    (gd / "configs" / "localization.ltx").write_text(
        "[string_table]\nlanguage = rus\n", encoding="utf-8")
    (gd / "configs" / "text" / "rus" / "base.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?><string_table>'
        '<string id="a"><text>原文</text></string></string_table>', encoding="utf-8")
    (gd / "gameplay" / "dialog.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?><string_table>'
        '<string id="d1"><text>你好，旅行者</text></string></string_table>', encoding="utf-8")
    (gd / "scripts" / "a.script").write_text('local t = "你好世界"\n', encoding="utf-8")
    ad = root / "ixr_addons" / "~bodycam"
    (ad / "configs" / "text" / "rus").mkdir(parents=True)
    (ad / "gameplay").mkdir(parents=True)
    (ad / "configs" / "text" / "rus" / "st_pf_bodycam.xml").write_text(
        '<string_table><string id="b"><text>bodycam 原文</text></string></string_table>',
        encoding="utf-8")
    (ad / "gameplay" / "x.xml").write_text(
        '<string_table><string id="x"><text>附加模块文本</text></string></string_table>',
        encoding="utf-8")


def main() -> int:
    print("== ori 树：容器发现 / 整树复制 / 提取改写 / 落点 / 只读 ===")
    if not CLI.is_file():
        print("!! 缺 %s" % CLI)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_ori_"))
    try:
        src = tmp / "src"
        make_mod_root(src)
        ori = tmp / "ori"
        before = snapshot(src)

        rc, out = run([str(CLI), "--ori-tree", str(src), str(ori)])
        if rc != 0:
            fail("--ori-tree 跑不通：rc=%d\n%s" % (rc, out[-600:]))
            return 1

        # ① 两个容器（gamedata + ixr_addons/~bodycam）都被识别
        if "rel=gamedata" in out and "rel=ixr_addons/~bodycam" in out:
            ok("容器发现：gamedata 与 ixr_addons/<addon> 各一个")
        else:
            fail("容器发现不对：\n%s" % out[-400:])

        # ② 整树复制（configs/text 逐字节保留、fonts 复制）
        base_src = src / "gamedata" / "configs" / "text" / "rus" / "base.xml"
        base_out = ori / "gamedata" / "configs" / "text" / "rus" / "base.xml"
        if base_out.is_file() and base_out.read_bytes() == base_src.read_bytes():
            ok("configs/text 逐字节保留")
        else:
            fail("configs/text 被改动或丢失")
        if (ori / "gamedata" / "fonts" / "rus" / "arial.ttf").is_file():
            ok("fonts/ 整棵复制")
        else:
            fail("fonts/ 没复制")

        # ③ gameplay 落 configs/gameplay，容器根 gameplay/ 不再重复
        gp = ori / "gamedata" / "configs" / "gameplay" / "dialog.xml"
        if gp.is_file() and "sgtat_" in gp.read_text(encoding="utf-8", errors="replace"):
            ok("gameplay 改写后落 configs/gameplay，文本换成 ID")
        else:
            fail("gameplay 落点/改写不对")
        if not (ori / "gamedata" / "gameplay").exists():
            ok("容器根的 gameplay/ 不再重复出现（游戏只读 configs/gameplay）")
        else:
            fail("容器根 gameplay/ 还留着，ori 里同一份文本出现两次")

        # ④ scripts 改写 + 文本表落盘
        script = ori / "gamedata" / "scripts" / "a.script"
        if script.is_file() and "translate_string" in script.read_text(encoding="utf-8", errors="replace"):
            ok("scripts 改写为 translate_string(<id>)")
        else:
            fail("scripts 没被改写")
        texts = sorted((ori / "gamedata" / "configs" / "text" / "rus").glob("gs_*.xml"))
        body = "".join(p.read_text(encoding="utf-8", errors="replace") for p in texts)
        if texts and "你好世界" in body and "你好，旅行者" in body:
            ok("提取出的中文正文都在文本表里（%d 个表）" % len(texts))
        else:
            fail("文本表缺中文正文（表数 %d）" % len(texts))

        # ⑤ addon 容器也带自己的 text 与 configs/gameplay
        addon_gp = ori / "ixr_addons" / "~bodycam" / "configs" / "gameplay" / "x.xml"
        addon_texts = list((ori / "ixr_addons" / "~bodycam" / "configs" / "text" / "rus").glob("*.xml"))
        if addon_gp.is_file() and addon_texts:
            ok("addon 容器：自己的 configs/gameplay + configs/text 都在")
        else:
            fail("addon 容器结构不全")

        # ⑥ 输入只读
        if snapshot(src) == before:
            ok("输入源一个字节都没变（只读）")
        else:
            fail("源目录被改动了")

        # ⑦ ori 非空 ⇒ 拒绝（不静默覆盖）
        again_rc, again_out = run([str(CLI), "--ori-tree", str(src), str(ori)])
        if again_rc != 0 and "拒绝" in again_out:
            ok("ori 非空：拒绝并说清（不静默覆盖）")
        else:
            fail("ori 非空却继续写：rc=%d" % again_rc)

        # ⑧ 单文件输入：本轮如实拒绝（下一轮实现），不许假装成功
        single_rc, single_out = run([str(CLI), "--ori-tree", str(src / "gamedata" / "scripts" / "a.script"),
                                     str(tmp / "ori_single")])
        if single_rc != 0 and "单文件" in single_out:
            ok("单文件输入：如实拒绝（下一轮实现），没有假装成功")
        else:
            fail("单文件输入的行为不对：rc=%d\n%s" % (single_rc, single_out[-200:]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("ORI-TREE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("ORI-TREE: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
