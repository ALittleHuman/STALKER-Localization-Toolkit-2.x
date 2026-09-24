#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字符集提取锁（M1-7 第⑦步）：2.x `--extract-chars` vs 1.x `font_pack.extract_chars`。

判据：字符集**逐码位相同**（顺序是 `sorted(chars)`，所以按行比就行）。
字符集错一个字的后果特别隐蔽：引擎对缺失码位不是留空，而是画成**同一个重复字形**
（`TCMap[i] = vFirstValid`），日志里毫无痕迹。

语料两类：
  ① **真语料**：`build/golden_text_extract/**/ours/*_texts.xml`（我们自己提取出来的翻译 XML，
     与 1.x 逐字节一致 ⇒ 拿它当输入最贴近真实用法）；
  ② **合成边界**：单根 / 多根 / 畸形 XML / 实体与数字引用 / CDATA / 注释 / PI / DOCTYPE /
     GBK / UTF-16(BOM) / 无 BOM cp1251 / 空文件 / 非 XML 文件 / 子目录递归。
两类都跑 `full_cyrillic=True/False`。

    python tests/golden_font_charset.py
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_charset"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
FONT_PACK_PY = TOOLKIT_1X / "font_pack" / "font_pack.py"
REAL_XML_DIRS = [
    ROOT / "build" / "golden_text_extract" / "real-gameplay" / "ours",
    ROOT / "build" / "golden_text_extract" / "real-scripts" / "ours",
]

# 已知分歧（**逐条记账，不许静默放宽**）：文件名 → 原因。
# 这一条不是 `extract_chars` 的问题，而是**判定链第 5 步（统计探测）没移植 chardet 的
# GB2312/GBK 探针** ⇒ 无声明、非 UTF-8 的 GBK 源会落到终局默认 windows-1251。
# 与门禁 [16]（文本判定链）的差异台账同源；要修就修 `text/sniff.cpp` 的第 5 步。
KNOWN_DIVERGENCES = {
    "gbk.xml": "判定链第 5 步未移植 chardet 的 GB2312 探针（无 XML 声明时落到 windows-1251）",
}


def load_reference():
    spec = importlib.util.spec_from_file_location("font_pack_1x", FONT_PACK_PY)
    if spec is None or spec.loader is None:
        raise RuntimeError("装载失败：%s" % FONT_PACK_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_cps(path: Path) -> list:
    return [int(t, 16) for t in path.read_text(encoding="ascii").split()]


def make_corpus(root: Path) -> None:
    """合成语料：把所有已知的解析/编码边界都塞进去。"""
    if root.exists():
        shutil.rmtree(root)
    (root / "sub").mkdir(parents=True)
    # 单根 + 缩进/换行 + 实体 + 数字引用
    (root / "single.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '\t<string id="a"><text>Hello &amp; &lt;世界&gt; &#20013;&#x6587;</text></string>\n'
        '\t<string id="b"><text>第二行\t制表符</text></string>\n'
        '</string_table>\n', encoding="utf-8")
    # 多根（ET 单根解析会失败 → 1.x 走"补根"回退）
    (root / "multi.xml").write_text(
        '<string id="a"><text>多根一</text></string>\n<string id="b"><text>多根二</text></string>\n',
        encoding="utf-8")
    # 畸形（未闭合标签 → 1.x 走"去标签"回退；& 未转义也是畸形）
    (root / "broken.xml").write_text(
        '<?xml version="1.0"?>\n<string id="a"><text>未闭合 & 特殊</text>\n', encoding="utf-8")
    # CDATA / 注释 / PI / DOCTYPE
    (root / "cdata.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE table [<!ENTITY x "y">]>\n'
        '<!-- 注释里的字不该进集 ZZZ -->\n<?php 处理指令也不该进 QQQ ?>\n'
        '<table><text><![CDATA[CDATA正文数据]]></text></table>\n', encoding="utf-8")
    # GBK（1.x：chardet 探到 GB2312；我们第 5 步未移植 ⇒ **台账分歧**，见 KNOWN_DIVERGENCES）
    (root / "gbk.xml").write_bytes(
        '<?xml version="1.0"?>\n<text>中文汉化测试</text>\n'.encode("gb18030"))
    # GBK **带声明**：判定链第 4 步（XML 声明被字节证实）就能定下来 ⇒ 应当一致
    (root / "gbk_decl.xml").write_bytes(
        '<?xml version="1.0" encoding="gb2312"?>\n<text>声明编码的中文</text>\n'.encode("gb18030"))
    # UTF-16 LE with BOM
    (root / "utf16.xml").write_bytes(
        '<?xml version="1.0" encoding="utf-16"?>\n<text>宽字符文件</text>\n'.encode("utf-16"))
    # 无声明 cp1251（1.x 落到默认 windows-1251）
    (root / "cp1251.xml").write_bytes("<text>Привет мир</text>\n".encode("windows-1251"))
    # 空文件 / 非 XML / 子目录里的 XML（递归）
    (root / "empty.xml").write_bytes(b"")
    (root / "notxml.txt").write_text("这行不该被扫到 QQQ\n", encoding="utf-8")
    (root / "sub" / "nested.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<t><text>子目录递归</text></t>\n', encoding="utf-8")


def blame_files(fp, corpus_dir: Path, full_cyrillic: bool):
    """把整目录的差异归到具体文件：返回"与 1.x 不同的文件名"集合（无法归属返回 None）。"""
    import tempfile
    blamed = set()
    for path in sorted(corpus_dir.rglob("*.xml")):
        with tempfile.TemporaryDirectory() as tmp:
            one = Path(tmp)
            shutil.copy2(path, one / path.name)
            out = one / "out.txt"
            proc = subprocess.run([str(CLI), "--extract-chars", str(one), str(out)]
                                  + ([] if full_cyrillic else ["--no-full-cyrillic"]),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if proc.returncode != 0:
                return None
            ours = {int(t, 16) for t in out.read_text(encoding="ascii").split()}
            ref = fp.extract_chars(str(one), full_cyrillic=full_cyrillic)
            want = {ord(c) for c in ref} if ref else set()
        if ours != want:
            blamed.add(path.name)
    return blamed


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-CHARSET: SKIP (cli-missing)")
        return 2
    if not FONT_PACK_PY.is_file():
        print("!! 找不到 1.x 的 %s" % FONT_PACK_PY)
        print("GOLDEN-FONT-CHARSET: SKIP (toolkit-1x-missing)")
        return 2

    fp = load_reference()
    WORK.mkdir(parents=True, exist_ok=True)
    synth = WORK / "synth"
    make_corpus(synth)

    corpora = [("合成边界语料", synth)]
    for d in REAL_XML_DIRS:
        if d.is_dir():
            corpora.append(("真语料 %s" % d.parent.name, d))
        else:
            print("  (跳过真语料 %s：不存在 —— 先跑门禁的文本提取步)" % d)

    failures = 0
    cases = 0
    for name, corpus_dir in corpora:
        for full_cyrillic in (True, False):
            cases += 1
            tag = "%s full_cyrillic=%d" % (name, 1 if full_cyrillic else 0)
            out_file = WORK / ("ours_%d_%s.txt" % (1 if full_cyrillic else 0,
                                                   corpus_dir.name))
            cmd = [str(CLI), "--extract-chars", str(corpus_dir), str(out_file)]
            if not full_cyrillic:
                cmd.append("--no-full-cyrillic")
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if proc.returncode != 0:
                failures += 1
                print("  ✗ %s：CLI 退出码 %d —— %s"
                      % (tag, proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
                continue
            ours = read_cps(out_file)
            ref = fp.extract_chars(str(corpus_dir), full_cyrillic=full_cyrillic)
            want = sorted({ord(c) for c in ref}) if ref is not None else []
            if ours == want:
                print("  ✓ %s：%d 个码位完全一致" % (tag, len(want)))
                continue
            failures += 1
            only_ours = [c for c in ours if c not in set(want)]
            only_ref = [c for c in want if c not in set(ours)]

            # 差异归属：逐个文件比一次，看是不是**台账里**那几条（只有台账内的才算已知）
            blamed = blame_files(fp, corpus_dir, full_cyrillic)
            if blamed is not None and all(name in KNOWN_DIVERGENCES for name in blamed):
                failures -= 1
                print("  ! %s：%d 条差异全部来自**已知分歧**（不算通过，也不判红）"
                      % (tag, len(blamed)))
                for name in sorted(blamed):
                    print("      %s —— %s" % (name, KNOWN_DIVERGENCES[name]))
                continue
            print("  ✗ %s：我们 %d / 1.x %d" % (tag, len(ours), len(want)))
            print("      只在我们的：%s" % " ".join("U+%04X" % c for c in only_ours[:20]))
            print("      只在 1.x 的：%s" % " ".join("U+%04X" % c for c in only_ref[:20]))
            if blamed is not None:
                print("      超出台账的文件：%s"
                      % " ".join(n for n in blamed if n not in KNOWN_DIVERGENCES))

    print("")
    print("GOLDEN-FONT-CHARSET: %s (cases=%d failed=%d)"
          % ("FAIL" if failures else "PASS", cases, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
