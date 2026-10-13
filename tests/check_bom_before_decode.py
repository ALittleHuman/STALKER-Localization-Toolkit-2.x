# -*- coding: utf-8 -*-
"""P17-113 判据：**认编码必须先认 BOM，再决定解码**（复审计 `02 F-07`）。

背景（审计判定）
----------------
`02 F-07` 的原话是"改为**先认 BOM 再决定解码**"。当时的形状是：先把 UTF-8 的三字节 BOM
剥掉、**再去** sniff ⇒ 带 **UTF-16/UTF-32 BOM**（`FF FE` / `FE FF` / `FF FE 00 00`）的文件
会被那段"剥三字节"逻辑**先啃掉头**，然后当成单字节编码去猜 —— 结果是一串乱码，
而且 **UTF-32LE 的 `FF FE 00 00` 还会被误当成 UTF-16LE**（宽 BOM 必须**先长后短**匹配）。

现状（本批实测，`sniff.cpp:decode_bytes`）：宽 BOM **在剥 UTF-8 BOM 之前**先认，
命中的话把**整段原始字节**交给对应 codec（由 codec 自己吃掉 BOM）⇒ 已修。
★ 但账本把它记成"**已修但审计不知**"，**且原来没有任何判据锁住它**（复审计横向主题③：
  "未修/半修集中在没被门禁写住的地方"）。本批把它**锁上**——修好但没锁，等于下次会退化。

判据（**端到端**，不是只测 `bom_encoding` 那个帮助函数）
------------------------------------------------------
把五种字节喂给真正的解码入口 `--sniff <输入> <输出>`，逐条查结论行与**产物**：

| # | 输入 | 必须 |
| --- | --- | --- |
| ① | UTF-16LE ＋ `FF FE` | `enc=utf-16`、`used=utf-16`、产物 = 正确 UTF-8 |
| ② | UTF-16BE ＋ `FE FF` | `enc=utf-16`、产物正确 |
| ③ | UTF-32LE ＋ `FF FE 00 00` | `enc=utf-32`（**不许**被当成 UTF-16LE）、产物正确 |
| ④ | UTF-8 ＋ `EF BB BF` | `enc=utf-8`、产物**不带** BOM、内容正确 |
| ⑤ | 无 BOM 的 cp1251 | 走**原判定链**认成 `windows-1251`（别把"先认 BOM"变成"只认 BOM"） |

退出码：0=绿 / 1=红 / 2=缺前提（没有 CLI）。
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# 语料：西里尔（STALKER 的正主）+ 拉丁 + 标点 —— 用 cp1251 出乱码时一眼能看出
SAMPLE = "Привет, Сталкер! 0123"
EXPECT_UTF8 = SAMPLE.encode("utf-8")

CASES = (
    ("① UTF-16LE+BOM", b"\xff\xfe" + SAMPLE.encode("utf-16-le"), "utf-16"),
    ("② UTF-16BE+BOM", b"\xfe\xff" + SAMPLE.encode("utf-16-be"), "utf-16"),
    ("③ UTF-32LE+BOM", b"\xff\xfe\x00\x00" + SAMPLE.encode("utf-32-le"), "utf-32"),
    ("④ UTF-8+BOM", b"\xef\xbb\xbf" + SAMPLE.encode("utf-8"), "utf-8"),
    ("⑤ 无BOM cp1251", SAMPLE.encode("cp1251"), "windows-1251"),
)

PROBLEMS: list = []
COUNT = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global COUNT
    COUNT += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-113 认编码必须先认 BOM、再决定解码（复审计 `02 F-07`）==")
    if not cli.is_file():
        print("!! 缺构建产物：%s" % cli)
        print("BOM-FIRST skip 缺 CLI")
        return 2

    with tempfile.TemporaryDirectory(prefix="bom_first_") as tmp:
        for label, data, expect_enc in CASES:
            src = Path(tmp) / "in.txt"
            dst = Path(tmp) / "out.txt"
            src.write_bytes(data)
            if dst.exists():
                dst.unlink()
            proc = subprocess.run([str(cli), "--sniff", str(src), str(dst)],
                                  capture_output=True, timeout=180)
            text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
            head = next((l for l in text.splitlines() if l.startswith("SNIFF ")), "(无 SNIFF 行)")

            want(proc.returncode == 0, "%s 退出码 0" % label, head[:100])
            want(("enc=%s " % expect_enc) in head or head.endswith("enc=%s" % expect_enc),
                 "%s 结论编码 = %s" % (label, expect_enc), head[:100])
            want(("used=%s " % expect_enc) in head,
                 "%s **实际使用**的编码也是 %s（结论≠实使用就是要抓的病）" % (label, expect_enc))

            got = dst.read_bytes() if dst.exists() else b""
            want(got == EXPECT_UTF8,
                 "%s 产物 == 正确 UTF-8 文本（不是乱码）" % label,
                 ("产物 %r" % got[:24]) if got != EXPECT_UTF8 else "%d 字节" % len(got))
            if expect_enc == "utf-8":
                want(not got.startswith(b"\xef\xbb\xbf"),
                     "④ 产物**不带** BOM（口径：输出侧一律不带）")

    # ★ 反向锁：③ 那一格是"先长后短"的要害 —— 单独把它说清楚
    print("   · ③ 的要害：`FF FE 00 00` 同时是 UTF-32LE 的 BOM 与 UTF-16LE 的 BOM + NUL ⇒"
          "**先匹配长的**才对；上面逐条比过 `enc=utf-32`，所以这一格不是靠运气过的")

    print("-" * 66)
    if PROBLEMS:
        print("BOM-FIRST fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BOM-FIRST ok %d 项（五种字节端到端：宽 BOM 先认 ＋ UTF-32 不被当 UTF-16 ＋ "
          "UTF-8 BOM 剥掉且产物无 BOM ＋ 无 BOM 仍走原判定链；复审计 `02 F-07`）" % COUNT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
