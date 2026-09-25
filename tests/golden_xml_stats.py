#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""XML 统计模式的黄金对拍：2.x（`engine::xml`） vs **Python 自己的口径**。

参照不是"我以为应该长什么样"，而是两件**真实存在**的东西：
  ① 行数 = `len(text.splitlines())`（Python 内置语义；1.x 曾错误地对结尾换行额外 +1）；
  ② ID 计数 = 1.x `apps/xml_compare_app.py` 里的 `ID_PATTERN`
     （`\\bid\\s*=\\s*["']([^"']+)["']`，re.I，且**先剥掉开头的 XML 声明**）。
解码走 1.x 的 `read_text_file`（与门禁 [14] 同一份参照），所以这条对拍只考"行数/ID"。

    python tests/golden_xml_stats.py            # 对拍
    python tests/golden_xml_stats.py --list     # 只打印样本量
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
BASELINE = HERE / "golden" / "xml_stats.json"
PER_ROOT = 20

REAL_ROOTS = [
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\gameplay"),
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\scripts"),
    Path(r"E:\Software\Games\STALKER\Localization\Gamedata\A.R.E.A\Original\configs\gameplay"),
    Path(r"E:\Software\Games\STALKER\SoC_Source\S.T.A.L.K.E.R. Shadow of Chernobyl_RUS"),
]

# 与 1.x 逐字一致的 ID 正则与"剥开头声明"的正则
ID_PATTERN = re.compile(r"""\bid\s*=\s*["']([^"']+)["']""", re.IGNORECASE)
DECL_PATTERN = re.compile(r"^\s*\ufeff?\s*<\?xml[^>]*\?>\s*", re.IGNORECASE | re.DOTALL)


def synth_samples() -> dict[str, bytes]:
    """10.6 的边界：**只有真正多出一行内容才算多一行**（Python 的完整边界集合）。"""
    out: dict[str, bytes] = {}
    out["empty.xml"] = b""
    out["one_line.xml"] = b"<root/>"
    out["trailing_lf.xml"] = b"<root>\n</root>\n"        # 结尾换行 ⇒ 仍是 2 行
    out["no_trailing_lf.xml"] = b"<root>\n</root>"       # 与上一个**行数相同**
    out["crlf.xml"] = b"<root>\r\n</root>\r\n"
    out["blank_line.xml"] = b"<root>\n\n</root>"         # 中间空行算一行 ⇒ 3 行
    out["vt.xml"] = b"<a>\x0b<b/>"                       # \v 也是边界
    out["ff.xml"] = b"<a>\x0c<b/>"                       # \f
    out["nel.xml"] = "<a>\u0085<b/>".encode("utf-8")     # NEL
    out["ls.xml"] = "<a>\u2028<b/>".encode("utf-8")      # LINE SEPARATOR
    out["ps.xml"] = "<a>\u2029<b/>".encode("utf-8")      # PARAGRAPH SEPARATOR
    out["decl_with_ids.xml"] = (
        '<?xml version="1.0" encoding="windows-1251"?>\n'
        '<string_table>\n  <string id="a"><text>x</text></string>\n'
        '  <string id="a"><text>y</text></string>\n'
        '  <string  id = \'b\'><text>z</text></string>\n</string_table>\n'
    ).encode("utf-8")
    out["ids_only_in_decl.xml"] = (
        '<?xml version="1.0" id="decl_id"?>\n<root id="real"/>'
    ).encode("utf-8")
    # ID 前后是词字符时不该算（`\b` 语义）
    out["word_boundary.xml"] = b'<x grid="nope" id="yes" valid="1"/>'
    return out


def real_files() -> list[Path]:
    out: list[Path] = []
    for root in REAL_ROOTS:
        if not root.is_dir():
            continue
        files = sorted(p for p in root.rglob("*.xml") if p.is_file())[:PER_ROOT]
        out.extend(files)
    return out


def reference_1x(path: Path) -> tuple[str, int, int]:
    """1.x `read_text_file` 解码 → (编码名, Python 的行数, 1.x 正则的 ID 数)。"""
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from toolkit_textio import read_text_file  # noqa: PLC0415

    text, enc = read_text_file(str(path))
    body = DECL_PATTERN.sub("", text, count=1)
    return enc, len(text.splitlines()), len(Counter(ID_PATTERN.findall(body)))


def candidate_2x(path: Path, enc: str) -> tuple[int, int]:
    proc = subprocess.run([str(CLI), "--xml-stats", str(path), "--encoding", enc],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(text.strip()[:200])
    line = [ln for ln in text.splitlines() if ln.startswith("XMLSTATS ok")]
    if not line:
        raise RuntimeError("没有收尾行：" + text.strip()[:200])
    fields = dict(kv.split("=", 1) for kv in line[-1].split()[2:])
    return int(fields["lines"]), int(fields["ids"])


def main() -> int:
    parser = argparse.ArgumentParser(description="XML 统计模式对拍")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--write", action="store_true",
                        help="只把参照结果写进基线（记录用）")
    args = parser.parse_args()

    synth = synth_samples()
    real = real_files()
    if args.list:
        print("合成 %d 个 / 真语料 %d 个" % (len(synth), len(real)))
        return 0

    if not CLI.exists():
        print("找不到 CLI：%s" % CLI)
        print("GOLDEN-XML-STATS: SKIP (cli-missing)")
        return 2

    failures: list[str] = []
    compared = 0
    records: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="xrkit_xmlstats_") as tmp:
        work = Path(tmp)
        for name, raw in sorted(synth.items()):
            path = work / name
            path.write_bytes(raw)
            try:
                enc, lines, ids = reference_1x(path)
                got_lines, got_ids = candidate_2x(path, enc)
            except Exception as exc:  # noqa: BLE001
                failures.append("%s：%s" % (name, exc))
                continue
            compared += 1
            if (got_lines, got_ids) != (lines, ids):
                failures.append("%s：参照 lines=%d ids=%d / 候选 lines=%d ids=%d"
                                % (name, lines, ids, got_lines, got_ids))
            records.append({"id": name, "lines": lines, "ids": ids})

        for path in real:
            try:
                enc, lines, ids = reference_1x(path)
                got_lines, got_ids = candidate_2x(path, enc)
            except Exception as exc:  # noqa: BLE001
                failures.append("%s：%s" % (path.name, exc))
                continue
            compared += 1
            if (got_lines, got_ids) != (lines, ids):
                failures.append("%s：参照 lines=%d ids=%d / 候选 lines=%d ids=%d"
                                % (path.name, lines, ids, got_lines, got_ids))

    if args.write:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(json.dumps(
            {"note": "参照=Python splitlines + 1.x ID_PATTERN", "synth": records},
            indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print("已写基线 → %s" % BASELINE.relative_to(ROOT))

    for item in failures[:10]:
        print("  ✗ %s" % item)
    if len(failures) > 10:
        print("  …（还有 %d 条）" % (len(failures) - 10))
    print("GOLDEN-XML-STATS: %s (compared=%d failed=%d)"
          % ("FAIL" if failures else "PASS", compared, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
