# -*- coding: utf-8 -*-
"""判据 H-01：交付 README 的**落点口径**必须与产品行为一致（第三方只读审计 H-01）。

修前事实（审计原话 + 我的核对）
------------------------------
`scripts/package.ps1` 生成的 `README.txt` 与 `docs/2.0.0-打包与发版规格.md` §3 都写
「本工具**不改动游戏目录里的任何文件**」。但解包的**默认落点就是"来源所在目录"**
（引擎 `efs::resolve_output_root`），而来源包通常在游戏/模组目录里 ——
2026-10-05 真发生过「一次默认落点解包把 **3.9 万个文件**盖在模组目录上」的事故。
⇒ 那句话在**用户可见面**上是误导：它不是"我们承诺不写游戏目录"，而是"我们不会**自动安装**"。

正确表述（审计建议 + 用户 2026-10-04 的口径"不改默认落点、不弹确认框，但覆盖必须可见"）：
> 本工具**不会自动安装或修改游戏文件**；解包/封包/字库产物写到**用户选择的目录**，
> 但**默认落点是"来源所在目录"** —— 直接点开始、没改落点的话，产物会落进来源包所在的目录。

断言
----
A **禁句**：交付 README 模板（`package.ps1` 里的 here-string）**不许**出现
   「不改动游戏目录」「不碰游戏目录」这类**无条件**承诺（它们是修前那句的同族）；
B **必须说清落点**：README 模板必须同时出现「默认落点」与「来源」（＝引擎
   `resolve_output_root` 的语义），且必须点名"产物会落进来源包所在目录"这件事；
C **规格同步**：`docs/2.0.0-打包与发版规格.md` 的安装/回退那节**同样**不许有禁句、必须写清落点；
D **同源锁**：正文点到的 `resolve_output_root` 必须真的在 `extract_tree.h` 里声明
   （防止文档引用一个不存在/改了名的规则）；
E **交付面覆盖**：README 模板里"安装/回退/注意"三段（README.txt 的可见面）都在。

★ 为什么判**模板**而不是判产出的 `README.txt`：产出要跑一次打包（本机被 `vcruntime140.dll`
  拦着），而模板是唯一真源（`package.ps1` 里那一段就是原样写出去的）—— 判它才是判"写什么"。
  这条边界写在判据里，别读成"已经验过真实包里的 README.txt"。

退出码：0=绿 / 1=红 / 2=缺输入（脚本或规格缺失）。
`--ps1` / `--spec` 只为**取证**存在（造回退副本跑红证），默认永远走仓库路径。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PS1 = ROOT / "scripts" / "package.ps1"
SPEC = ROOT / "docs" / "2.0.0-打包与发版规格.md"
TREE_H = ROOT / "include" / "xrkit" / "engine" / "fs" / "extract_tree.h"

# 修前那句的同族（**无条件承诺**，都不许再出现）
FORBIDDEN = ["不改动游戏目录", "不改动**游戏目录", "不碰游戏目录", "不会碰游戏目录",
             "不修改游戏目录", "不改动任何游戏文件"]

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


def readme_template(ps1: str) -> str:
    """从 `package.ps1` 抠出 README.txt 的 here-string（`@"` … `"@ | Set-Content … README.txt`）。"""
    m = re.search(r'@"\n(.*?)\n"@\s*\|\s*Set-Content[^\n]*README\.txt', ps1, re.S)
    return m.group(1) if m else ""


def spec_section(spec: str) -> str:
    """规格里"安装与回退"那一节（到下一个 `## ` 标题为止）。"""
    m = re.search(r"##\s*3\..*?(?=\n##\s)", spec, re.S)
    return m.group(0) if m else ""


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--ps1", default=str(PS1))
    ap.add_argument("--spec", default=str(SPEC))
    args = ap.parse_args()
    ps1_path, spec_path = Path(args.ps1), Path(args.spec)

    print("== H-01：交付 README 的落点口径必须与产品行为一致 ==")
    for p in (ps1_path, spec_path, TREE_H):
        if not p.is_file():
            print("README-CLAIMS skip 缺 %s" % p)
            return 2
    ps1 = ps1_path.read_text(encoding="utf-8", errors="replace")
    spec = spec_path.read_text(encoding="utf-8", errors="replace")
    tree_h = TREE_H.read_text(encoding="utf-8", errors="replace")

    readme = readme_template(ps1)
    want(bool(readme), "A0 能从 package.ps1 抠出 README.txt 的模板（抠不到＝判据与脚本脱节）",
         "%d 字符" % len(readme))
    if not readme:
        print("README-CLAIMS fail 抠不到 README 模板")
        return 1

    hits = [w for w in FORBIDDEN if w in readme]
    want(not hits, "A1 交付 README **不许**再写「不改动游戏目录」这类无条件承诺",
         ("仍出现：" + "、".join(hits)) if hits else "")
    want("默认落点" in readme and "来源" in readme,
         "B1 交付 README 必须写出「默认落点 ＝ 来源所在目录」",
         "有默认落点=%s 有来源=%s" % ("默认落点" in readme, "来源" in readme))
    want(re.search(r"不会自动安装|不会自动安装或修改", readme) is not None,
         "B2 交付 README 必须写出「不会自动安装或修改游戏文件」（这是准确的那一半）")

    sec = spec_section(spec)
    want(bool(sec), "C0 规格里能找到「安装与回退」那一节")
    if sec:
        hits2 = [w for w in FORBIDDEN if w in sec]
        want(not hits2, "C1 规格 §3 同样不许再写「不改动游戏目录」这类无条件承诺",
             ("仍出现：" + "、".join(hits2)) if hits2 else "")
        want("默认落点" in sec and "来源" in sec,
             "C2 规格 §3 必须写清「默认落点 ＝ 来源所在目录」")

    for word in ("安装：", "回退：", "注意："):
        want(word in readme, "E1 交付 README 的可见面里有「%s」段" % word.rstrip("："))

    want(re.search(r"resolve_output_root", sec) is not None and "resolve_output_root" in tree_h,
         "D1 规格点到的 `resolve_output_root` 真的在 `extract_tree.h` 里声明（同源锁）")

    print("-" * 66)
    if PROBLEMS:
        print("README-CLAIMS fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("README-CLAIMS ok %d 项（README/规格不再做无条件承诺，且都写清了「默认落点＝来源所在目录」；"
          "★ 验的是模板，真实包里的 README.txt 要等打包放行）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
