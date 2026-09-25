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


def candidate_2x_text(rel: str, path_a: Path, path_b: Path) -> dict:
    """跑 2.x 的 `--xml-text`，解析成 {consistent, ids_a, ids_b, only_a, only_b, text_diff}。"""
    proc = subprocess.run([str(CLI), "--xml-text", rel, str(path_a), str(path_b)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(text.strip()[:200])
    head = [ln for ln in text.splitlines() if ln.startswith("XMLTEXT ok")]
    if not head:
        raise RuntimeError("没有收尾行：" + text.strip()[:200])
    fields = dict(kv.split("=", 1) for kv in head[-1].split()[2:])
    out = {"consistent": int(fields["consistent"]), "ids_a": int(fields["ids_a"]),
           "ids_b": int(fields["ids_b"]), "only_a": [], "only_b": [], "text_diff": []}
    for line in text.splitlines():
        if line.startswith("XMLTEXT diff id="):
            body = line[len("XMLTEXT diff id="):]
            iid, rest = body.split("\t", 1)
            a_part, b_part = rest.split("\tb=", 1)
            out["text_diff"].append((iid, a_part[2:], b_part))
        elif line.startswith("XMLTEXT only_a="):
            out["only_a"].append(line[len("XMLTEXT only_a="):])
        elif line.startswith("XMLTEXT only_b="):
            out["only_b"].append(line[len("XMLTEXT only_b="):])
    return out


def load_1x_text_mode():
    """把 1.x 的文本模式实现**按 AST 摘出来**执行（不 import 模块 —— 那会拖进 tkinter）。

    参照就是 1.x 的**源码本身**（`_fix_entities` + `parse_file_text_by_id` +
    `compare_text_content` + 三个模块级正则），所以不存在"我转述错了"。
    """
    import ast
    import xml.etree.ElementTree as ET  # noqa: N817

    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from toolkit_textio import read_text_file  # noqa: PLC0415

    src = (TOOLKIT_1X / "apps" / "xml_compare_app.py").read_text(encoding="utf-8")
    tree = ast.parse(src)
    wanted_funcs = {"_fix_entities", "parse_file_text_by_id", "compare_text_content"}
    wanted_consts = {"_AMP_BAD", "_LT_BAD", "_COMMENT_BAD"}
    ns = {"re": re, "ET": ET, "Counter": Counter,
          "decode_file": lambda path, enc="auto": read_text_file(str(path)),
          "log_summary": lambda *a, **k: None}
    for node in tree.body:
        if isinstance(node, ast.Assign):
            names = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if names & wanted_consts:
                exec(compile(ast.Module([node], []), "<1x>", "exec"), ns)  # noqa: S102
        elif isinstance(node, ast.FunctionDef) and node.name in wanted_funcs:
            exec(compile(ast.Module([node], []), "<1x>", "exec"), ns)  # noqa: S102
    missing = [n for n in (wanted_funcs | wanted_consts) if n not in ns]
    if missing:
        raise SystemExit("1.x 源码里缺这些定义：%s" % ", ".join(sorted(missing)))
    return ns


def reference_1x_text(ns, rel: str, path_a: Path, path_b: Path) -> dict:
    r = ns["compare_text_content"](rel, path_a, path_b, enc_a="auto", enc_b="auto")
    return {"consistent": 1 if r.get("consistent") else 0,
            "ids_a": r.get("ids_a", 0), "ids_b": r.get("ids_b", 0),
            "only_a": sorted(r.get("only_a") or []), "only_b": sorted(r.get("only_b") or []),
            "text_diff": sorted((i, a, b) for i, a, b in (r.get("text_diff") or []))}


def csv_reference(rows: list[list[str]], header_rows: list[list[str]]) -> bytes:
    """Python `csv.writer`（excel 方言）+ 先写 `\\ufeff`，写盘 `newline=""` 不翻译换行。"""
    import csv
    import io

    buf = io.StringIO()
    buf.write("\ufeff")
    writer = csv.writer(buf)
    for row in header_rows:
        writer.writerow(row)
    for row in rows:
        writer.writerow(row)
    return buf.getvalue().encode("utf-8")


def text_mode_cases(work: Path) -> list[tuple[str, Path, Path]]:
    """文本模式的样本对：`<text>` 子元素优先、自有文本、实体、重复 id、坏 XML。"""
    cases: list[tuple[str, str, str]] = [
        ("child_text.xml",
         '<?xml version="1.0"?>\n<string_table>\n'
         '  <string id="a"><text>hello</text></string>\n'
         '  <string id="b"><text>world</text></string>\n</string_table>',
         '<?xml version="1.0"?>\n<string_table>\n'
         '  <string id="a"><text>hello</text></string>\n'
         '  <string id="b"><text>world!</text></string>\n</string_table>'),
        ("own_text.xml",
         '<text id="x">plain</text>',
         '<text id="x">plain2</text>'),
        ("entities.xml",
         '<string_table><string id="e"><text>a &amp; b &lt;c&gt; &#1055;</text></string>'
         '</string_table>',
         '<string_table><string id="e"><text>a &amp; b &lt;c&gt; &#1055;</text></string>'
         '</string_table>'),
        ("bare_amp.xml",
         '<string_table><string id="e"><text>tom & jerry</text></string></string_table>',
         '<string_table><string id="e"><text>tom &amp; jerry</text></string></string_table>'),
        # 重复 id：后出现的覆盖先出现的（1.x 直接 dict 赋值）
        ("dup_id.xml",
         '<r><s id="d"><text>first</text></s><s id="d"><text>second</text></s></r>',
         '<r><s id="d"><text>second</text></s></r>'),
        # 只在一侧
        ("one_side.xml",
         '<r><s id="only_a"><text>1</text></s></r>',
         '<r><s id="only_b"><text>2</text></s></r>'),
        # 注释里带 `--`（1.x 的 _COMMENT_BAD 要修）与多根平铺
        ("flat_and_comment.xml",
         '<!-- a -- b --><s id="p"><text>v</text></s><s id="q"><text>w</text></s>',
         '<!-- a -- b --><s id="p"><text>v</text></s><s id="q"><text>W</text></s>'),
        # 坏 XML：两侧都解析不出来 ⇒ 结果不可信（1.x 只告警）
        ("broken.xml",
         '<r><s id="a"><text>x</text></s>',
         '<r><s id="a"><text>x</text></s>'),
    ]
    out: list[tuple[str, Path, Path]] = []
    for name, a, b in cases:
        pa = work / ("a_" + name)
        pb = work / ("b_" + name)
        pa.write_text(a, encoding="utf-8", newline="")
        pb.write_text(b, encoding="utf-8", newline="")
        out.append((name, pa, pb))
    return out


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

        # ── 文本模式（ID 文本对比）：参照 = 1.x 源码按 AST 摘出来直接跑 ──
        text_ns = load_1x_text_mode()
        for name, path_a, path_b in text_mode_cases(work):
            rel = "config/gameplay/" + name
            try:
                want = reference_1x_text(text_ns, rel, path_a, path_b)
                got = candidate_2x_text(rel, path_a, path_b)
            except Exception as exc:  # noqa: BLE001
                failures.append("文本模式 %s：%s" % (name, exc))
                continue
            compared += 1
            if got != want:
                failures.append("文本模式 %s：参照 %s / 候选 %s" % (name, want, got))

        # 真语料上也走几对（同一文件比同一文件：必须判"一致"且 ID 数相同）
        for path in real[:12]:
            try:
                want = reference_1x_text(text_ns, path.name, path, path)
                got = candidate_2x_text(path.name, path, path)
            except Exception as exc:  # noqa: BLE001
                failures.append("文本模式 %s：%s" % (path.name, exc))
                continue
            compared += 1
            if got != want:
                failures.append("文本模式 %s：参照 %s / 候选 %s" % (path.name, want, got))

        # ── 10.5 CSV 渲染：参照 = Python `csv.writer`（excel 方言）+ 前置 BOM ──
        csv_out = work / "probe.csv"
        proc = subprocess.run([str(CLI), "--xml-csv-probe", str(csv_out)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        compared += 1
        if proc.returncode != 0:
            failures.append("CSV 探针失败：%s"
                            % proc.stdout.decode("utf-8", errors="replace")[:200])
        else:
            header_rows = [
                ["XML校对报告"],
                ["生成时间: 2026-09-24 00:00:00"],
                ["比较模式: 行数/ID 统计"],
                ["文件夹 A: A 目录"],
                ["文件夹 B: B 目录"],
                ["编码（auto=自动判定）: A=auto / B=auto"],
                ["实际使用编码: A=windows-1251 / B=utf-8"],
                ["筛选条件: 全部"],
                ["一致: 3 | 不一致: 3 | 仅 A: 1 | 仅 B: 0"],
                [],
                ["文件 (相对路径)", "状态", "差异详情"],
            ]
            body_rows = [
                ["a.xml", "一致", "无差异"],
                ["b,c.xml", "不一致", "仅 A 有 1；文本不同 2"],
                ["quote\".xml", "不一致", "有 \"引号\" 的摘要"],
                ["multi\nline.xml", "不一致", "摘要里有换行"],
                ["中文.xml", "一致", "无差异"],
                ["", "", ""],
            ]
            want_bytes = csv_reference(body_rows, header_rows)
            got_bytes = csv_out.read_bytes() if csv_out.exists() else b""
            if got_bytes != want_bytes:
                failures.append("CSV 与 Python csv.writer 不一致：期望 %d 字节 / 得到 %d 字节"
                                % (len(want_bytes), len(got_bytes)))
            elif not got_bytes.startswith(b"\xef\xbb\xbf"):
                failures.append("CSV 没有以 BOM 开头（Excel 会认成乱码）")
            else:
                records.append({"id": "csv", "bytes": len(got_bytes)})

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
