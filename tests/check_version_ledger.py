# -*- coding: utf-8 -*-
r"""判据 B-5：`version.json` 的**发布台账必须可核**（第三方审计 B-5 / 中低项）。

事实（审计指出，我当轮核对）：
  · `tree[<版本>][<标记>][<序号>].commit` 在 **ALPHA.13–16、19–23** 里是占位串
    `"2.0.0 ALPHA.0"`（不是提交号）；
  · **ALPHA.17 与 ALPHA.107 缺号**；
  · `artifacts` 里有的路径指向**已不存在的 zip**。

为什么这要判：台账是"发过哪些包、对应哪个提交"的**唯一记录**。占位串与幽灵路径会让
下一个人把"查不到"当成"没发过"。按本仓口径：**要么是真的，要么显式标 `note`/`missing:`**，
不许留一个看起来像真值的占位。

断言：
  A 每个条目的 `commit` 必须匹配 `^[0-9a-f]{7,40}$`，**或者**该条目有非空 `note`（显式说明为什么不是）；
  B 每条 `artifacts` 路径要么**存在于仓库**，要么以 `missing:` 开头（显式认账）；
  C 序号在 `[1, current.index]` 内的**缺号必须被点名**（有 `missing` 清单或 `note`），
    否则就是静默缺号（审计点出的 17 / 107）。
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VJ = ROOT / "version.json"
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
    print("== 发布台账可核（B-5）：commit 不许占位、artifacts 不许幽灵、缺号必须点名 ==")
    if not VJ.is_file():
        print("VERSION-LEDGER skip 缺 version.json")
        return 2
    doc = json.loads(VJ.read_text(encoding="utf-8"))
    tree = doc.get("tree") or {}
    cur = doc.get("current") or {}
    bad_commit, ghost, seen = [], [], []
    sha = re.compile(r"^[0-9a-f]{7,40}$")
    for ver, marks in tree.items():
        for mark, idxs in (marks or {}).items():
            for idx, rec in (idxs or {}).items():
                where = "%s %s.%s" % (ver, mark, idx)
                try:
                    seen.append(int(idx))
                except ValueError:
                    pass
                if not isinstance(rec, dict):
                    continue
                c = str(rec.get("commit", ""))
                note = str(rec.get("note", "")).strip()
                if not sha.match(c) and not note:
                    bad_commit.append("%s: commit=%r（不是提交号，也没有 note）" % (where, c[:40]))
                for a in rec.get("artifacts") or []:
                    a = str(a)
                    if a.startswith("missing:"):
                        continue
                    if not (ROOT / a.replace("\\", "/")).exists():
                        ghost.append("%s: %s" % (where, a[:70]))

    want(not bad_commit, "每个台账条目的 commit 是**真提交号**或有 `note` 认账",
         ("%d 条占位：%s" % (len(bad_commit), bad_commit[:2])) if bad_commit else
         "共核 %d 条" % sum(len(v) for m in tree.values() for v in (m or {}).values()))
    want(not ghost, "artifacts 里没有**幽灵路径**（不存在就以 `missing:` 开头认账）",
         ("%d 条幽灵：%s" % (len(ghost), ghost[:2])) if ghost else "")

    # C：缺号必须被点名
    top = max(seen) if seen else 0
    gap = [i for i in range(1, top + 1) if i not in set(seen)]
    declared = set()
    for m in tree.values():
        for idxs in (m or {}).values():
            for idx, rec in (idxs or {}).items():
                if isinstance(rec, dict) and str(rec.get("note", "")).strip():
                    try:
                        declared.add(int(idx))
                    except ValueError:
                        pass
    unexplained = [i for i in gap if i not in declared]
    want(not unexplained, "序号缺号要么补上、要么在条目上写 `note` 认账",
         ("未说明的缺号：%s（审计点名的 17/107 就在这类）" % unexplained[:12])
         if unexplained else ("缺号 %d 个，均已认账" % len(gap)))

    print("-" * 66)
    if PROBLEMS:
        print("VERSION-LEDGER fail %d 项不成立（**这就是台账现状**）：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("VERSION-LEDGER ok %d 项（commit 可核 / artifacts 无幽灵 / 缺号有账）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
