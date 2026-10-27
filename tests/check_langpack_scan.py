# -*- coding: utf-8 -*-
"""语言包识别（`--langpack-scan`）判据：**四档识别 + 只读 + 真包夹具**。

口径（2026-10-26 用户拍板 + `12_规划_语言包识别与分部位字体.md` §三/§3.1）：
  · 只读扫描一个目录；不改任何文件（本脚本用完即删临时目录，并核源目录 mtime/sha 不变）；
  · 判定顺序写死：有 ttf + 段里有 name ⇒ IX-Ray（矢量优先，dds 忽略）；否则有 dds ⇒ 三档位图
    （shader=hud\\font ⇒ CoP；目录 config ⇒ SoC；否则 ClS）；都没有 ⇒ unknown + 退码 2；
  · 识别结构，不识别模组：夹具里不出现任何模组名。

退出码：0=全过；1=有判红；2=真包夹具缺失（外部前提缺件，进"本次未覆盖"）。
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

# 真包夹具（本机才有；缺了走 2，不判红）
REAL_PACKS = [
    Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat"
         r"\Chronicles of Pripyat 4.26 IX-Ray\MODS\~NotoSansSC"),
    Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat"
         r"\Chronicles of Pripyat 4.26 IX-Ray\MODS\~HPSimplifiedHans-Light"),
    Path(r"E:\Software\Games\STALKER\Games\Chronicles of Pripyat\Chronicles of Pripyat"
         r"\Chronicles of Pripyat 4.26 IX-Ray\MODS\~HPSimplifiedHans-Regular"),
]

failures = 0


def fail(msg: str) -> None:
    global failures
    failures += 1
    print("  ✗ " + msg)


def ok(msg: str) -> None:
    print("  ✓ " + msg)


def run_scan(path: Path) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--langpack-scan", str(path)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def field(out: str, key: str) -> str:
    for line in out.splitlines():
        if line.startswith("LANGPACK ") and " " + key + "=" in line:
            tail = line.split(" " + key + "=", 1)[1]
            return tail.split(" ", 1)[0]
    return ""


def write(path: Path, text: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def make_fixture(base: Path, kind: str) -> Path:
    """造一个最小语言包；kind ∈ ixr / cop / cis / soc / unknown。"""
    pack = base / kind
    gd = pack / "gamedata"
    if kind == "unknown":
        (gd / "configs").mkdir(parents=True)
        write(gd / "configs" / "localization.ltx", "[string_table]\nlanguage = rus\n")
        return pack
    if kind.startswith("ixr"):
        write(gd / "configs" / "fonts.ltx",
              "; 夹具：矢量（段里有 name）\n"
              "[font_default]\n"
              "shader = hud\\font\n"
              "size = 14\n"
              "name = arial\n"
              "opentype = false\n"
              "[font_letterica]:font_default\n"
              "name = letterica\n"
              "size = 16\n")
        write(gd / "configs" / "localization.ltx",
              "[string_table]\nlanguage = rus\nlanguages = rus,eng\n")
        # 只要存在即可（扫描不解析字体内容）
        (gd / "fonts" / "rus").mkdir(parents=True)
        (gd / "fonts" / "rus" / "arial.ttf").write_bytes(b"")
        (gd / "fonts" / "rus" / "letterica.ttf").write_bytes(b"")
        write(gd / "configs" / "text" / "rus" / "st_dialogs.xml",
              '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
              '  <string id="a"><text>你好</text></string>\n</string_table>\n')
        return pack
    # 位图三档
    shader = "hud\\font" if kind == "cop" else "font"
    cfg_dir = "config" if kind == "soc" else "configs"
    write(gd / cfg_dir / "ui" / "fonts.ltx",
          "; 夹具：位图（texture + shader，没有 name）\n"
          "[ui_font_arial_14]\n"
          "texture = ui\\ui_font_arial_14\n"
          "shader = %s\n"
          "size = 14\n" % shader)
    write(gd / cfg_dir / "localization.ltx", "[string_table]\nlanguage = rus\n")
    tex = gd / "textures" / "ui"
    tex.mkdir(parents=True)
    (tex / "ui_font_arial_14.dds").write_bytes(b"DDS ")
    return pack


def main() -> int:
    print("== 语言包识别（--langpack-scan）：四档 + 只读 + 真包 ==")
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("LANGPACK-SCAN: SKIP (cli-missing)")
        return 1

    tmp = Path(tempfile.mkdtemp(prefix="langpack_scan_"))
    try:
        # ① 四档识别（合成夹具）
        expect = {"ixr": "ixr", "cop": "cop", "cis": "cis", "soc": "soc"}
        for kind, want in expect.items():
            pack = make_fixture(tmp, kind)
            rc, out = run_scan(pack)
            got = field(out, "profile")
            lang = field(out, "lang")
            if rc == 0 and got == want:
                ok("%s：profile=%s lang=%s" % (kind, got, lang))
            else:
                fail("%s：期望 profile=%s rc=0，实得 profile=%s rc=%d\n%s"
                     % (kind, want, got, rc, out.strip()))
            if kind == "ixr":
                if field(out, "slots") == "2" and field(out, "ttf") == "2":
                    ok("ixr：槽位 2（含继承段）、ttf 2")
                else:
                    fail("ixr：槽位/ttf 数不对（slots=%s ttf=%s）"
                         % (field(out, "slots"), field(out, "ttf")))
                if field(out, "dds") == "0":
                    ok("ixr：没有 dds 也认得出（不依赖位图资源）")
                else:
                    fail("ixr：dds 计数应为 0，实得 %s" % field(out, "dds"))

        # ② unknown：零静默 + 退码 2（"没出错、但不是正常路径"）
        pack = make_fixture(tmp, "unknown")
        rc, out = run_scan(pack)
        if rc == 2 and field(out, "profile") == "unknown" and "LANGPACK unrecognized" in out:
            ok("unknown：profile=unknown、退码 2、显式 unrecognized（零静默）")
        else:
            fail("unknown：期望 rc=2 + unrecognized，实得 rc=%d\n%s" % (rc, out.strip()))

        # ③ 自证：把 ixr 夹具的 ttf 删光 ⇒ 必须从 ixr 掉到 unknown（判据不是恒绿）
        pack = make_fixture(tmp, "ixr_mut")
        shutil.rmtree(pack / "gamedata" / "fonts")
        rc, out = run_scan(pack)
        if field(out, "profile") == "unknown":
            ok("自证：删掉 ttf ⇒ 不再判 IX-Ray（判据会红）")
        else:
            fail("自证失败：删掉 ttf 后仍判 %s" % field(out, "profile"))

        # ④ 只读：扫描前后源树快照必须一致
        pack = make_fixture(tmp, "readonly")
        before = sorted((p.relative_to(pack).as_posix(), p.stat().st_size, p.stat().st_mtime_ns)
                        for p in pack.rglob("*") if p.is_file())
        run_scan(pack)
        after = sorted((p.relative_to(pack).as_posix(), p.stat().st_size, p.stat().st_mtime_ns)
                       for p in pack.rglob("*") if p.is_file())
        if before == after:
            ok("只读：扫描前后文件清单/大小/mtime 全同")
        else:
            fail("只读被破坏：扫描改了源目录")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ⑤ 真包夹具（外部前提；缺了退 2）
    missing = [p for p in REAL_PACKS if not p.is_dir()]
    if missing:
        print("!! 缺真包夹具 %d/%d 个（例 %s）" % (len(missing), len(REAL_PACKS), missing[0]))
        print("LANGPACK-SCAN: SKIP (real-packs-missing)")
        return 2
    for pack in REAL_PACKS:
        rc, out = run_scan(pack)
        prof, lang, slots, ttf = (field(out, "profile"), field(out, "lang"),
                                  field(out, "slots"), field(out, "ttf"))
        if rc == 0 and prof == "ixr" and lang == "rus" and slots == "36" and int(ttf or 0) > 0:
            ok("真包 %s：profile=ixr lang=rus slots=36 ttf=%s" % (pack.name, ttf))
        else:
            fail("真包 %s：期望 ixr/rus/36 且 ttf>0，实得 %s/%s/%s/%s rc=%d"
                 % (pack.name, prof, lang, slots, ttf, rc))

    if failures:
        print("LANGPACK-SCAN: FAIL（%d 处）" % failures)
        return 1
    print("LANGPACK-SCAN: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
