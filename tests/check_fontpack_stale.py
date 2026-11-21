# -*- coding: utf-8 -*-
"""判据：**重复生成要清掉旧字库**（已知问题 §7，2026-11-21 修）。

修前事实：同一输出目录换了 `--suffix`（或字号档位）再生成时，上一次的
`ui_font_*.dds/.ini` 不会被删，会一起被打进包（用户会拿到两套字库）。
另外还发现一条更糟的：合并到既有 `gamedata` 时，本次**新文件名**会让
`is_regular_file` 置 `ERROR_FILE_NOT_FOUND`，旧代码当"检查失败"整批回滚。

本判据走真实产品路径（CLI `--font-pack`），钉四条：
  ① 第一次（`--suffix _chs`）写出 20 个文件；
  ② 第二次（无 suffix）成功、`stale_removed=20`，目录里**只剩**无后缀那一套；
  ③ 第二次不再报"提交前检查失败 / 已回滚"（新文件名不再被误判成错误）；
  ④ 字号补丁（`--patch-offsets`）跑在既有产物上：`stale_removed=0`（只增不删）。

用法：python tests/check_fontpack_stale.py   （0=全过 / 1=红 / 2=缺构建产物）
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
XML = ('<string_table><string id="a"><text>Привет</text></string></string_table>')


def check(cond: bool, label: str, detail: str = "") -> None:
    if cond:
        print("  \u2713 " + label, flush=True)
    else:
        print("  \u2717 " + label + (("  —— " + detail) if detail else ""), flush=True)
        FAILURES.append(label)


def run(args: list[str]) -> str:
    proc = subprocess.run([str(CLI)] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    return proc.stdout.decode("utf-8", "replace")


def field(out: str, name: str) -> str:
    for token in out.split():
        if token.startswith(name + "="):
            return token.split("=", 1)[1]
    return ""


def fonts(ui: Path) -> list[str]:
    return sorted(p.name for p in ui.iterdir() if p.is_file()) if ui.is_dir() else []


def main() -> int:
    if not CLI.is_file():
        print("SKIP cli-missing: %s" % CLI)
        return 2
    work = Path(tempfile.mkdtemp(prefix="fontpack_stale_", dir=str(ROOT / "build")))
    try:
        text_root = work / "textrus"
        text_root.mkdir()
        (text_root / "ui_st_mm.xml").write_text(XML, encoding="utf-8", newline="\n")
        out = work / "out"
        ui = out / "gamedata" / "textures" / "ui"
        base = ["--font-pack", "--ref", "builtin", "--text-root", str(text_root), "--out",
                str(out), "--game", "SoC", "--langs", "rus"]

        print("== ① 第一次：带 --suffix _chs ==")
        first = run(base + ["--suffix", "_chs"])
        names = fonts(ui)
        check(len(names) == 20, "① 写出 20 个字体文件", str(names[:4]))
        check(all("_chs" in n for n in names), "① 文件名都带 _chs", str(names[:4]))

        print("== ② 第二次：无 suffix（旧字库要被清掉） ==")
        second = run(base)
        names2 = fonts(ui)
        check("FONTPACK ok" in second, "② 第二次成功", second.strip()[-300:])
        check(field(second, "stale_removed") == "20", "② stale_removed=20",
              second.strip()[-300:])
        check(len(names2) == 20, "② 目录里仍是 20 个", str(names2[:4]))
        check(all("_chs" not in n for n in names2), "② 旧的 _chs 字库已删干净", str(names2[:4]))

        print("== ③ 不再误报「提交前检查失败 / 回滚」 ==")
        check("提交前检查" not in second and "已回滚" not in second,
              "③ 新文件名不再被当成错误", second.strip()[-300:])

        print("== ④ 字号补丁叠加：只增不删 ==")
        patch = run(base + ["--patch-offsets", "7"])
        check("FONTPACK ok" in patch, "④ 补丁模式成功", patch.strip()[-300:])
        check(field(patch, "stale_removed") == "0", "④ patch 模式 stale_removed=0",
              patch.strip()[-300:])
        check(len(fonts(ui)) == 20, "④ 补丁后文件数不变（同名覆盖）", str(fonts(ui)[:4]))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if FAILURES:
        print("FONTPACK-STALE: FAIL（%d 条）" % len(FAILURES), flush=True)
        return 1
    print("FONTPACK-STALE: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
