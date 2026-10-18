# -*- coding: utf-8 -*-
"""判据 B-4：**机器消费行必须是 ASCII `KEY k=v`**（第三方只读审计 B-4）。

修前事实（审计原话）
--------------------
`SHELL-SMOKE`、`PLUGIN-PAGES`、`#8 包内插件` 这几行是**机器消费**的（`package.ps1` 用
`Select-String` 抓它们、门禁脚本用正则解析），可它们**拿中文当分隔符**：
`PLUGIN-PAGES 可见页集合=6 页（内置=6 / 插件py=0）[…] 结束`、
`#8 包内插件：N 个文件（D 个 dll / P 个 py）…`。
⇒ 改一个全角括号/一个"个"字，消费侧就**静默失配**（最难查的一类：解析不到就当成"没打这行"）。
审计建议：迁成 `KEY k=v`，并且**先在消费侧同时认新旧两种，迁完删旧**。

本批（迁移第一步）
------------------
1. **生产者双打**：`smoke.cpp` 在旧中文行之外，再打一行 ASCII：
   · `PKG-PLUGINS files=N dll=N py=N errors=N names=[a,b] dir=…`
   · `PLUGIN-PAGES pages=N builtin=N plugin_py=N owner_matches_id=0|1 names=[1=fs,2=convert,…] dir=…`
   固定骨架**全 ASCII**（`names=`/`dir=` 的**值**可以是中文——那是插件名/路径，不是分隔符）。
2. **消费侧认新优先、旧兜底**：`check_release_criterion.py`、`check_package_fixture_pages.py`。
3. ★ `scripts/package.ps1` 那条**本批不动**：它现在**跑不起来**（`vcruntime140.dll` 拦着），
   改了也没法验；等打包放行、新包产出后再切（交接里已登记）。它读的旧行**仍然在**，不受影响。

断言
----
A `smoke.cpp` 里两条新格式都在；
B 两条新格式的**固定骨架**里没有 CJK（值占位 `%N` 不算）；
C 新格式能被**消费侧的正则**解析（拿合成样例行现场匹配）；
D 两个消费侧脚本都**同时**含新、旧两种正则（迁移期"认新不丢旧"）；
E 打印口径（新增两行都从**同一个来源**取值：`page_names`/`names` —— 反例见 D 的自证）。

退出码：0=绿 / 1=红 / 2=缺输入。`--smoke-src`/`--release-checker`/`--pages-checker`
只为取证存在（拿 `git show <修前>:…` 跑红证）。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SMOKE = ROOT / "src" / "shell" / "smoke.cpp"
RELEASE_CHECKER = ROOT / "tests" / "check_release_criterion.py"
PAGES_CHECKER = ROOT / "tests" / "check_package_fixture_pages.py"

# 新格式的**骨架**（`%1`… 是 Qt 的占位符）
PKG_FMT = ("PKG-PLUGINS files=%1 dll=%2 py=%3 errors=%4 names=[%5] dir=%6")
PAGES_FMT = ("PLUGIN-PAGES pages=%1 builtin=%2 plugin_py=%3 owner_matches_id=%4 "
             "names=[%5] dir=%6")

# 消费侧应有的新正则（判据现场拿合成样例验它）
PKG_RE = re.compile(r"PKG-PLUGINS files=(\d+) dll=(\d+) py=(\d+) errors=(\d+)")
PAGES_NEW_RE = re.compile(
    r"PLUGIN-PAGES pages=(\d+) builtin=(\d+) plugin_py=(\d+) owner_matches_id=(\d) names=\[(.*?)\]")
PAGES_OLD_RE = re.compile(r"PLUGIN-PAGES 可见页集合=(\d+)\s*页（内置=(\d+)\s*/\s*插件py=(\d+)）")
PKG_OLD_RE = re.compile(r"#8 包内插件：(\d+)\s*个文件")

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


def has_cjk(text: str) -> bool:
    return any("\u4e00" <= ch <= "\u9fff" or "\uff00" <= ch <= "\uffef" for ch in text)


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--smoke-src", default=str(SMOKE))
    ap.add_argument("--release-checker", default=str(RELEASE_CHECKER))
    ap.add_argument("--pages-checker", default=str(PAGES_CHECKER))
    args = ap.parse_args()
    smoke_p, rel_p, pages_p = Path(args.smoke_src), Path(args.release_checker), Path(args.pages_checker)

    print("== B-4：机器消费行必须是 ASCII `KEY k=v`（迁移第一步：生产者双打、消费侧认新优先）==")
    for p in (smoke_p, rel_p, pages_p):
        if not p.is_file():
            print("MACHINE-LINES skip 缺 %s" % p)
            return 2
    smoke = smoke_p.read_text(encoding="utf-8", errors="replace")
    rel = rel_p.read_text(encoding="utf-8", errors="replace")
    pages = pages_p.read_text(encoding="utf-8", errors="replace")

    want(PKG_FMT in smoke, "A1 `smoke.cpp` 里有新的 `PKG-PLUGINS files=…` 格式")
    want(PAGES_FMT in smoke, "A2 `smoke.cpp` 里有新的 `PLUGIN-PAGES pages=…` 格式")
    want(not has_cjk(PKG_FMT), "B1 `PKG-PLUGINS` 的**固定骨架**全 ASCII（值占位不算）")
    want(not has_cjk(PAGES_FMT), "B2 `PLUGIN-PAGES pages=…` 的**固定骨架**全 ASCII")

    # C：拿**合成样例行**现场验消费侧的正则（真实包放行前也验得了）
    sample_pkg = ("SHELL-SMOKE note PKG-PLUGINS files=0 dll=0 py=0 errors=0 names=[-] "
                  "dir=C:\\x\\plugins")
    sample_pages = ("SHELL-SMOKE note PLUGIN-PAGES pages=6 builtin=6 plugin_py=0 "
                    "owner_matches_id=1 names=[1=fs,2=convert,3=text,4=xml,5=video,6=fontpack] "
                    "dir=C:\\x")
    m = PKG_RE.search(sample_pkg)
    want(m is not None and m.groups() == ("0", "0", "0", "0"),
         "C1 合成样例能被新 `PKG-PLUGINS` 正则解出四个数",
         ("解出 " + ",".join(m.groups())) if m else "不匹配")
    m = PAGES_NEW_RE.search(sample_pages)
    ids = re.findall(r"\d+=([A-Za-z0-9_]+)", m.group(5)) if m else []
    want(m is not None and ids == ["fs", "convert", "text", "xml", "video", "fontpack"],
         "C2 合成样例能被新 `PLUGIN-PAGES` 正则解出计数与**页名顺序**",
         ",".join(ids) if ids else "不匹配")

    # D：消费侧"认新不丢旧"
    want("PLUGIN-PAGES pages=" in rel, "D1 `check_release_criterion.py` 认新 ASCII 行")
    want("可见页集合=" in rel, "D2 同一个脚本仍然认旧中文行（迁移期不丢旧）")
    want("PKG-PLUGINS files=" in pages, "D3 `check_package_fixture_pages.py` 认新 `PKG-PLUGINS`")
    want("#8 包内插件" in pages, "D4 同一个脚本仍然认旧中文行")

    # D5/D6：**真把消费侧的解析函数跑一遍**（不是只 grep 源码）——
    #   `check_package_fixture_pages.parse_pages_any` 是模块级函数、可直接 import；
    #   拿"新/旧两种行"各喂一次，断言解出来是**同一组事实**（这才是"认新不丢旧"）。
    sys.path.insert(0, str(pages_p.parent))
    try:
        import check_package_fixture_pages as cpfp  # noqa: E402
        new_parsed = cpfp.parse_pages_any(sample_pages) if hasattr(cpfp, "parse_pages_any") else None
        legacy_parsed = cpfp.parse_pages_any(sample_pages.replace(
            "PLUGIN-PAGES pages=6 builtin=6 plugin_py=0 owner_matches_id=1 "
            "names=[1=fs,2=convert,3=text,4=xml,5=video,6=fontpack]",
            "PLUGIN-PAGES 可见页集合=6 页（内置=6 / 插件py=0）"
            "[1=fs= 2=convert= 3=text= 4=xml= 5=video= 6=fontpack=] 结束")) \
            if hasattr(cpfp, "parse_pages_any") else None
        want(new_parsed is not None and new_parsed[:3] == (6, 6, 0) and new_parsed[4] == "ascii",
             "D5 真跑消费侧：**新 ASCII 行**解出 (6,6,0)（判为 ascii 形态）",
             str(new_parsed[:3]) if new_parsed else "解不出")
        want(legacy_parsed is not None and legacy_parsed[:3] == (6, 6, 0)
             and legacy_parsed[4] == "legacy"
             and legacy_parsed[3] == ["fs", "convert", "text", "xml", "video", "fontpack"],
             "D6 真跑消费侧：**旧中文行**同样解出 (6,6,0) 与同一份页名（判为 legacy 形态）",
             str(legacy_parsed[:3]) if legacy_parsed else "解不出")
    finally:
        sys.path.pop(0)

    # E：两行同源（都从 `page_names`/`names` 取；防"新行自己另算一份")
    want("page_names.isEmpty()" in smoke, "E1 新 `PLUGIN-PAGES` 行与旧行同源（都取 `page_names`）")
    want("names.isEmpty()" in smoke, "E2 新 `PKG-PLUGINS` 行与旧行同源（都取 `names`）")

    print("-" * 66)
    if PROBLEMS:
        print("MACHINE-LINES fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("MACHINE-LINES ok %d 项（新 ASCII 行在、骨架全 ASCII、消费侧认新优先且不丢旧；"
          "★ package.ps1 那条等打包放行后再切）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
