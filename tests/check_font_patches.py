# -*- coding: utf-8 -*-
"""判据：汉化包 **标准字体包 + 字号补丁**（2026-11-10）。

口径：
  · base：完整标准语言包（text + 10 个槽位的字体 + fonts.ltx/localization.ltx）；
  · 每个选中档位一个补丁目录 `<out>/_patches/font±N/gamedata/textures/ui/`；
  · 补丁**只含** 10 个槽位的 `ui_font_<slot><suffix>.dds` + `.ini`（成对），
    不含 text/scripts/fonts.ltx/localization.ltx；
  · 补丁叠加 base 后，字体文件与“同 offset 整包”逐字节一致。

退出码：0=全绿 / 1=断言不成立 / 2=缺 CLI 或字体（外部前提）。
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                         "cli" / "xrkit_cli.exe")))
FONT = Path(os.environ.get("XRKIT_TEST_FONT", "C:/Windows/Fonts/msyh.ttc"))
SLOTS = [11, 13, 15, 16, 17, 18, 20, 21, 23, 25]
PROBLEMS: list[str] = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label)
        return True
    print("   \u2717 " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append(label + (("（%s）" % detail) if detail else ""))
    return False


def run(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    print("== 字号补丁：base 标准包 + 补丁只含字体纹理 ==")
    if not CLI.is_file():
        print("!! 缺 CLI：%s" % CLI)
        print("FONT-PATCHES: SKIP (cli-missing)")
        return 2
    if not FONT.is_file():
        print("!! 缺测试字体：%s" % FONT)
        print("FONT-PATCHES: SKIP (font-missing)")
        return 2
    work = Path(tempfile.mkdtemp(prefix="xrkit_fontpatch_"))
    try:
        xml_dir = work / "xml"
        xml_dir.mkdir()
        (xml_dir / "a.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><string_table>'
            '<string id="a"><text>你好，Сталкер</text></string></string_table>',
            encoding="utf-8")
        out = work / "out"
        rc, text = run(["--font-pack", "--ref", "builtin", "--text-root", str(xml_dir),
                        "--font", str(FONT), "--out", str(out), "--game", "CoP",
                        "--langs", "rus", "--patch-offsets", "3,-1"])
        want(rc == 0, "base+补丁生成退出码 0", "rc=%d\n%s" % (rc, text[-500:]))

        base_gd = out / "gamedata"
        want((base_gd / "configs" / "text" / "rus").is_dir(),
             "base 有文本目录（标准语言包）")
        want((base_gd / "configs" / "fonts.ltx").is_file() and
             (base_gd / "configs" / "localization.ltx").is_file(),
             "base 有 fonts.ltx / localization.ltx")
        base_tex = base_gd / "textures" / "ui"
        base_files = sorted(p.name for p in base_tex.glob("*"))
        want(len(base_files) == 20, "base 有 10 槽位 × DDS+INI", "%d 个" % len(base_files))

        patches = sorted((out / "_patches").glob("font*"))
        want([p.name for p in patches] == ["font+3", "font-1"],
             "两个档位各一个补丁目录", [p.name for p in patches])
        for patch, off in ((out / "_patches" / "font+3", 3),
                           (out / "_patches" / "font-1", -1)):
            files = sorted(p.relative_to(patch).as_posix() for p in patch.rglob("*")
                           if p.is_file())
            want(len(files) == 20,
                 "font%+d 补丁只含 20 个字体文件" % off, "%d 个" % len(files))
            want(all(f.startswith("gamedata/textures/ui/") for f in files),
                 "font%+d 补丁不含 text/scripts/ltx" % off, files[:3])
            names = sorted(Path(f).name for f in files)
            expected = sorted("ui_font_%d.%s" % (s, ext)
                              for s in SLOTS for ext in ("dds", "ini"))
            want(names == expected, "font%+d 文件名 == 10 槽位 DDS+INI" % off,
                 str(set(names) ^ set(expected))[:200])

        # 叠加等价：补丁 +3 与“--offset 3 的整包”字体文件逐字节一致
        full3 = work / "full3"
        rc2, text2 = run(["--font-pack", "--ref", "builtin", "--text-root", str(xml_dir),
                          "--font", str(FONT), "--out", str(full3), "--game", "CoP",
                          "--langs", "rus", "--offset", "3"])
        want(rc2 == 0, "对照整包（offset=3）生成成功", text2[-300:])
        diffs = []
        patch_tex = out / "_patches" / "font+3" / "gamedata" / "textures" / "ui"
        full_tex = full3 / "gamedata" / "textures" / "ui"
        for name in sorted(expected):
            a = patch_tex / name
            b = full_tex / name
            if not (a.is_file() and b.is_file() and sha(a) == sha(b)):
                diffs.append(name)
        want(not diffs, "补丁与同 offset 整包逐字节一致", ",".join(diffs[:5]))
        changed = [name for name in expected if sha(base_tex / name) != sha(patch_tex / name)]
        want(len(changed) == 20, "标准包与 +3 补丁确实不同（档位生效）",
             "%d 个不同" % len(changed))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("FONT-PATCHES fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("FONT-PATCHES ok %d 项（base 标准包 + 补丁只含字体纹理，叠加逐字节等价）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
