# -*- coding: utf-8 -*-
r"""判据 B-5：`version.json` 的台账**对着认账账断账**（第三方审计 B-5 / H-03）。

**修前是什么**：第一版直接断言「每条 `commit` 必须是真提交号、每个 `artifacts` 路径必须存在」——
于是它对 2026-09/10 的历史痕迹**永久红**。而永久红就等于没有判据（没人会看一条永远红的步）。
用户 2026-10-16 裁决：**认账、不改史**（历史已推送、批报告按 SHA 引用、历史是回退点）
⇒ 改成「**对照台账**」：

  · `docs/2.0.0-版本台账认账.md` 是**只追加**的认账账，逐条列：
      - 10 条占位 commit（`2.0.0 ALPHA.13–16 / 19–23 / BETA.1`：串 `2.0.0 ALPHA.0`＝占位）；
      - 36 条幽灵 artifacts（34 个不同路径，`dist/` 下的早期日志与 zip，「未产出/已回退」）；
      - 2 个断号（`gap:17` / `gap:107`，「历史断号、不要补号」）。
  · 本判据：`version.json` 里**出现账上没有的**占位/幽灵/断号 ⇒ **红**；
    账上有的 ⇒ 豁免。**要么是真的，要么在账上** —— 不许留一个看起来像真值的占位。

断言：
  A 每个占位 commit（不匹配 `^[0-9a-f]{7,40}$` 且不是 `missing:` 口径）都在账上；
  B 每条幽灵 artifacts 路径都在账上（按**路径**认账，同路径多条引用只需一条）；
  C 序号在 `[1, current 最大号]` 内的缺号都有一条 `gap:<序号>`；
  D 账上每一行都**带理由**（`— ` 之后非空、且不只几个字）：认账不是列一串裸键；
  E 账上没有**重复键**（重复＝同一件事故记了两遍，容易两边不一致）；
  F（信息性，不判红）账上有些键**已经不对应**任何异常 —— 只追加的必然结果，打印出来供人看。

★ 为什么 F 不判红：账是**只追加**的。若将来某条被真修好（例如回填了提交号），
  按用户口径「不改历史」，那一行也**不删** ⇒ 出现"账上有、现状没有"属正常。
  反过来（现状有、账上没有）才必须红。

退出码：0=绿 / 1=判据不成立 / 2=缺输入（`version.json` 或账本）。
`--ledger PATH` / `--version-json PATH` 只为**取证**用（拿空账本或删一条的账本跑红证），
产品口径永远用默认路径。
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VJ = ROOT / "version.json"
LEDGER = ROOT / "docs" / "2.0.0-版本台账认账.md"

SHA = re.compile(r"^[0-9a-f]{7,40}$")
# `- \`<键>\` — <理由>`：键在反引号里，理由在破折号之后
LEDGER_LINE = re.compile(r"^-\s+`([^`]+)`\s+\u2014\s+(.+?)\s*$")

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


def anomalies(doc: dict):
    """从 version.json 里算出现状：占位键 / 幽灵路径 / 断号。"""
    tree = doc.get("tree") or {}
    placeholders, ghosts, seen = [], [], set()
    for ver, marks in tree.items():
        for mark, idxs in (marks or {}).items():
            for idx, rec in (idxs or {}).items():
                if not isinstance(rec, dict):
                    continue
                key = "%s %s.%s" % (ver, mark, idx)
                try:
                    seen.add(int(idx))
                except ValueError:
                    pass
                if not SHA.match(str(rec.get("commit", ""))):
                    placeholders.append(key)
                for a in rec.get("artifacts") or []:
                    a = str(a)
                    if a.startswith("missing:"):
                        continue  # 新口径已显式认账的，不算幽灵
                    if not (ROOT / a.replace("\\", "/")).exists():
                        ghosts.append(a)
    top = max(seen) if seen else 0
    gaps = [i for i in range(1, top + 1) if i not in seen]
    return placeholders, ghosts, gaps, top


def read_ledger(path: Path):
    """→ (keys 有序去重列表, 重复键, 理由过短的行)。"""
    keys, dups, thin = [], [], []
    seen = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        m = LEDGER_LINE.match(line.strip())
        if not m:
            continue
        key, reason = m.group(1).strip(), m.group(2).strip()
        if key in seen:
            dups.append(key)
        else:
            seen.add(key)
            keys.append(key)
        if len(reason) < 6:
            thin.append("%s（理由只有 %r）" % (key, reason))
    return keys, dups, thin


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--version-json", default=str(VJ))
    ap.add_argument("--ledger", default=str(LEDGER))
    args = ap.parse_args()
    vj = Path(args.version_json)
    lg = Path(args.ledger)

    print("== 版本台账可核（B-5）：占位/幽灵/断号必须**在认账账上** ==")
    if not vj.is_file():
        print("VERSION-LEDGER skip 缺 %s" % vj)
        return 2
    if not lg.is_file():
        print("VERSION-LEDGER skip 缺账本 %s" % lg)
        return 2

    doc = json.loads(vj.read_text(encoding="utf-8"))
    placeholders, ghosts, gaps, top = anomalies(doc)
    keys, dups, thin = read_ledger(lg)
    keyset = set(keys)
    print("   现状：占位 %d / 幽灵 %d（不同路径 %d）/ 断号 %d（1..%d）" %
          (len(placeholders), len(ghosts), len(set(ghosts)), len(gaps), top))
    print("   账上：%d 条（%s）" % (len(keys), lg.name))

    unlisted_ph = [k for k in placeholders if k not in keyset]
    unlisted_gh = [a for a in dict.fromkeys(ghosts) if a not in keyset]
    unlisted_gap = [i for i in gaps if ("gap:%d" % i) not in keyset]

    want(not unlisted_ph, "A 每个占位 commit 都在账上（要么是真提交号，要么认账）",
         ("未认账 %d 条：%s" % (len(unlisted_ph), unlisted_ph[:3])) if unlisted_ph else
         "占位 %d 条全在账上" % len(placeholders))
    want(not unlisted_gh, "B 每条幽灵 artifacts 路径都在账上（按路径认账）",
         ("未认账 %d 条：%s" % (len(unlisted_gh), unlisted_gh[:2])) if unlisted_gh else
         "幽灵 %d 条全在账上" % len(set(ghosts)))
    want(not unlisted_gap, "C 每个断号都有 `gap:<序号>` 认账（不补号）",
         ("未认账：%s" % unlisted_gap[:8]) if unlisted_gap else ("断号 %s 均已认账" % gaps))
    want(not thin, "D 账上每一条都带**理由**（不是一串裸键）",
         ("%d 行理由过短：%s" % (len(thin), thin[:2])) if thin else "")
    want(not dups, "E 账上没有重复键（同一件事只记一遍）",
         ("重复：%s" % dups[:5]) if dups else "")

    # F：信息性 —— 账上有、现状已无（只追加的必然结果，不判红）
    live = set(placeholders) | set(dict.fromkeys(ghosts)) | {"gap:%d" % i for i in gaps}
    stale = [k for k in keys if k not in live]
    if stale:
        print("   \u00b7 F 账上 %d 条已不对应任何异常（只追加，**不删**；仅信息）：%s" %
              (len(stale), ", ".join(stale[:6]) + ("…" if len(stale) > 6 else "")))

    print("-" * 66)
    if PROBLEMS:
        print("VERSION-LEDGER fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("VERSION-LEDGER ok %d 项（占位/幽灵/断号逐条对得上账，且每条都有理由）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
