# -*- coding: utf-8 -*-
"""判据 T-07（文档那半）：**设计口径里的压缩能力必须读写分开写实**。

背景
----
审计 T-07 原话：「`docs/2.0.0-设计口径.md` §3.1 承诺了 gzip/lz4/zstd，但实现里没有 ——
UI 无入口，文档与实现不一致」。

现状（P17-169 之后）：
* **写侧**：`xz`（默认，与 1.x 逐字节一致）＋ **`gzip`**（P17-169 实现并验收：超块 id=1、
  往返逐字节一致）；`lzo`/`lz4`/`zstd` **未实现**，且各有结构原因（lzo 要换 lzo2、lz4 要改布局
  ＋反语义、zstd 无基线）；
* **读侧**：五种全都支持（`decompress.cpp`）。

要求（本判据）
--------------
A 读侧那一行必须**明确标注是读侧**，并写明"写侧只支持 xz ＋ gzip，读写能力不对称"；
B 必须单独有一行**如实列出写侧没做的那三种**及其原因（lzo / lz4 / zstd 三个词都要出现）；
C "默认压缩"那一行必须写明**写侧另有 gzip**（不再只承诺、不落地）；
D 不许出现"UI 另给 gzip / lz4 / zstd"这类**未落地的承诺**（P17-169 前的那句就在这里）。

红证：把文档退回 P17-169 之前那一版（`git show 53b3007~1:docs/2.0.0-设计口径.md`）⇒ A/B/C 红。
退出码：0=绿 / 1=红 / 2=缺输入。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "2.0.0-设计口径.md"

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--doc", default=str(DOC))
    args = ap.parse_args()
    doc_path = Path(args.doc)
    print("== T-07（文档那半）：压缩能力必须读写分开写实 ==")
    if not doc_path.is_file():
        print("COMPRESSOR-DOC skip 缺 %s" % doc_path)
        return 2
    text = doc_path.read_text(encoding="utf-8", errors="replace")

    want("压缩（**读**侧）" in text, "A1 §3.1 那一行明确标成**读侧**")
    want("写侧**只支持" in text or "写侧只支持" in text or "写侧**只支持 xz" in text,
         "A2 写明写侧只支持 xz（＋gzip）")
    want("不对称" in text, "A3 点明读写能力**不对称**（别让读者按读侧推写侧）")

    want("写侧压缩器**没做**" in text, "B1 单独一行如实列「写侧没做的三种」")
    for word in ("lzo", "lz4", "zstd"):
        want(word in text, "B2 那三种里点名了 `%s`" % word)
    want("lzo1x_999" in text or "minilzo" in text, "B3 lzo 的原因写实（minilzo 没有 lzo1x_999）")
    want("COMPRESSOR_OPTIONS" in text or "8 字节 options" in text,
         "B4 lz4 的原因写实（要多写 options 块）")

    want("写侧另有 gzip" in text, "C1 「默认压缩」那行写明**写侧另有 gzip**（已落地，不只是承诺）")
    want("UI 另给 gzip / lz4 / zstd" not in text,
         "D 不再有「UI 另给 gzip / lz4 / zstd」这类**未落地承诺**")

    print("-" * 66)
    if PROBLEMS:
        print("COMPRESSOR-DOC fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("COMPRESSOR-DOC ok %d 项（读写分开写实；写侧没做的三种有据；gzip 已落地；无未落地承诺）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
