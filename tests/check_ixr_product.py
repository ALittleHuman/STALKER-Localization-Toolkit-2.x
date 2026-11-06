# -*- coding: utf-8 -*-
"""IX-Ray 产品入口（`--ixr-build`）判据：**C++ → 外部 fonttools 助手**这条链真通。

为什么单独立：它是"汉化包生成页的 IX-Ray 选项"的后端 —— 页面只调
`engine::font::ixr_build`，而这一段（找 python / 找启动器 / 起进程 / 收退码）必须自己可判。

判据：
  ① 工具链能解析（`--ixr-build` 缺参数 ⇒ 退码 3；给了就真跑）；
  ② 真跑一个夹具包：输出 `IXR ok …`、字体文件被合并（cmap 变大）；
  ③ 失败要**零静默**：不存在的参照包 ⇒ 退 1 且把助手的话带出来；
  ④ 工具链缺件（启动器指到不存在）⇒ 退 1 且说清缺什么。

外部前提：系统 python + NotoSansSC-VF.ttf + arial.ttf；缺 ⇒ 退 2。
启动器/字体库从仓库 `plugins_ixr/_lib` + `third_party/fonttools` 暂存到 `build/ixr_tool`。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
TOOL_DIR = ROOT / "build" / "ixr_tool"
CJK = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
BASE = Path(r"C:\Windows\Fonts\arial.ttf")


def fail(msg: str) -> None:
    print("  ✗ " + msg)


def ok(msg: str) -> None:
    print("  ✓ " + msg)


def stage_tools() -> str:
    """把启动器 + 助手 + vendored fontTools 停到 build/ixr_tool（与发行包同构）。"""
    TOOL_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "plugins_ixr" / "_lib" / "ixr_run.py", TOOL_DIR / "ixr_run.py")
    shutil.copy2(ROOT / "plugins_ixr" / "_lib" / "ixrgen.py", TOOL_DIR / "ixrgen.py")
    fontlib = TOOL_DIR / "fontlib"
    if fontlib.exists():
        shutil.rmtree(fontlib)
    (fontlib).mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / "vendor" / "fonttools" / "fontTools", fontlib / "fontTools")
    return str(TOOL_DIR / "ixr_run.py")


def make_pack(root: Path) -> Path:
    pack = root / "pack"
    gd = pack / "gamedata"
    (gd / "configs" / "text" / "rus").mkdir(parents=True)
    (gd / "fonts" / "rus").mkdir(parents=True)
    shutil.copy2(BASE, gd / "fonts" / "rus" / "arial.ttf")
    (gd / "configs" / "fonts.ltx").write_text(
        "[font_default]\nshader = hud\\font\nsize = 14\nname = arial\nopentype = false\n",
        encoding="utf-8")
    (gd / "configs" / "localization.ltx").write_text(
        "[string_table]\nlanguage = rus\n", encoding="utf-8")
    (gd / "configs" / "text" / "rus" / "t.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?><string_table>'
        '<string id="a"><text>Hello</text></string></string_table>', encoding="utf-8")
    return pack


def run_cli(args: list[str], env_extra: dict) -> tuple[int, str]:
    env = dict(os.environ)
    env.update(env_extra)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    proc = subprocess.run([str(CLI)] + args, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def main() -> int:
    problems = 0
    print("== IX-Ray 产品入口（--ixr-build）：C++ → fonttools 助手 ==")
    if not CLI.exists():
        print("!! 缺 %s" % CLI)
        return 1
    missing = [p for p in (CJK, BASE) if not p.is_file()]
    if missing:
        print("!! 缺系统字体：%s" % ", ".join(str(p) for p in missing))
        print("IXR-PRODUCT: SKIP (fonts-missing)")
        return 2
    if not (ROOT / "vendor" / "fonttools" / "fontTools").is_dir():
        print("!! 缺 vendored fontTools（vendor/fonttools）")
        print("IXR-PRODUCT: SKIP (fonttools-missing)")
        return 2

    # ⓪ 页面接线（静态）：IX-Ray 分支必须把「文本 XML」当字符集源传下去
    src = (ROOT / "src" / "shell" / "pages" / "fontpack_page.cpp").read_text(
        encoding="utf-8", errors="replace")
    wiring = [
        ("ixr_text_at_start_.toStdString()", "页面把「文本 XML」传给引擎（字符集来源）"),
        ("ixr_text_at_start_ = xml_edit_", "快照在主线程做（worker 不许读控件）"),
        ("ixr_pack_at_start_.toStdString()", "参照包也是快照"),
    ]
    for needle, why in wiring:
        if needle in src:
            ok("接线：" + why)
        else:
            problems += 1
            fail("页面接线缺失：%s（找不到 `%s`）" % (why, needle))

    launcher = stage_tools()
    env_extra = {"XRKIT_IXR_RUN": launcher, "XRKIT_IXR_PYTHON": sys.executable}
    tmp = Path(tempfile.mkdtemp(prefix="ixr_product_"))
    try:
        # ① 缺参数 ⇒ 退 3（用法错误）
        rc, out = run_cli(["--font-pack"], env_extra)
        if rc == 3 and "FONTPACK error reason=用法" in out:
            ok("缺参数：退码 3 且给用法")
        else:
            problems += 1
            fail("缺参数的行为不对：rc=%d\n%s" % (rc, out[-300:]))

        # ② 真跑：包 + 字体 ⇒ 输出合并后的字体
        pack = make_pack(tmp)
        # 汉字来源目录（页面里就是「文本 XML」那一栏）
        cn_dir = tmp / "cn_text"
        cn_dir.mkdir(parents=True, exist_ok=True)
        (cn_dir / "cn.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="a"><text>你好汉化</text></string></string_table>', encoding="utf-8")
        out_dir = tmp / "out"
        rc, out = run_cli(["--font-pack", "--ref", str(pack), "--font", str(CJK),
                           "--out", str(out_dir), "--text-root", str(cn_dir)], env_extra)
        merged = out_dir / "gamedata" / "fonts" / "rus" / "arial.ttf"
        if rc == 0 and ("FONTPACK ok" in out or "IXR ok" in out) and merged.is_file():
            ok("真跑：rc=0、IXR ok、输出字体存在")
        else:
            problems += 1
            fail("真跑失败：rc=%d\n%s" % (rc, out[-600:]))
        if merged.is_file():
            try:
                from fontTools.ttLib import TTFont
                cmap = set(TTFont(str(merged)).getBestCmap())
                if 0x4F60 in cmap:   # 你
                    ok("合并生效：cmap 里有「你」")
                else:
                    problems += 1
                    fail("cmap 里没有「你」—— 合并没生效")
            except Exception as exc:  # noqa: BLE001
                problems += 1
                fail("读不出输出字体：%r" % (exc,))

        # ②-b 2026-10-27（用户：「生成结果是俄语包而非汉化包」）：
        #   包内文本是拉丁、汉字在 --text-root 指的目录里 ⇒ 产物字体必须含「汉」。
        cn_dir = tmp / "cn_text"
        cn_dir.mkdir(parents=True, exist_ok=True)
        (cn_dir / "cn.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="a"><text>你好汉化</text></string></string_table>', encoding="utf-8")
        out_cn = tmp / "out_cn"
        rc, out = run_cli(["--font-pack", "--ref", str(pack), "--font", str(CJK),
                           "--out", str(out_cn), "--text-root", str(cn_dir)], env_extra)
        merged_cn = out_cn / "gamedata" / "fonts" / "rus" / "arial.ttf"
        if rc == 0 and merged_cn.is_file():
            from fontTools.ttLib import TTFont
            if 0x6C49 in set(TTFont(str(merged_cn)).getBestCmap()):
                ok("--text-root：「文本 XML」里的中文进了字符集（cmap 含「汉」）")
            else:
                problems += 1
                fail("产物字体里没有「汉」—— 字符集没吃 --text-root")
        else:
            problems += 1
            fail("带 --text-root 的生成失败：rc=%d" % rc)

        # ③ 零静默：参照包不存在 ⇒ 退 1 且带出助手的话
        rc, out = run_cli(["--font-pack", "--ref", str(tmp / "nope"), "--font", str(CJK),
                           "--out", str(tmp / "o2")],
                          env_extra)
        if rc == 1 and "FONTPACK error reason=" in out:
            ok("坏参照包：退 1，助手的话被带出来（零静默）")
        else:
            problems += 1
            fail("坏参照包的行为不对：rc=%d\n%s" % (rc, out[-400:]))

        # ④ 工具链缺件 ⇒ 退 1 且说清
        rc, out = run_cli(["--font-pack", "--ref", str(pack), "--font", str(CJK),
                           "--out", str(tmp / "o3")],
                          {"XRKIT_IXR_RUN": str(tmp / "nope_run.py"),
                           "XRKIT_IXR_PYTHON": sys.executable})
        if rc == 1 and "启动器不存在" in out:
            ok("启动器缺失：退 1 且点名缺什么")
        else:
            problems += 1
            fail("启动器缺失的行为不对：rc=%d\n%s" % (rc, out[-300:]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("IXR-PRODUCT: FAIL（%d 处）" % problems)
        return 1
    print("IXR-PRODUCT: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
