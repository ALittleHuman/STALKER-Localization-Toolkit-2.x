# -*- coding: utf-8 -*-
"""P17-102 判据：**目录遍历只许有一份实现**（审计 `02 F-14`）。

事实
----
`02 F-14` 的原话是"让 extract 复用 collect_tree"。审计把 `text/extract.cpp` 里那份
`recursive_directory_iterator` 记成"第 5 份"遍历。实查（P17-102 动手前）：

| 文件 | 修前 |
| --- | --- |
| `src/engine/fs/overlay.cpp` | `collect_tree()` —— **唯一一份实现**（自带 `WalkReport` 台账） |
| `src/engine/text/batch.cpp` | 已转调 `fs::collect_tree(root, &walk)` ✅ |
| `src/engine/text/extract.cpp` | ❌ **又写了**一份 `recursive_directory_iterator`，且**自己一套错误文案** |

为什么必须收口（不只是"少写几行"）：两套遍历的**出错语义**会各自漂 —— `batch.cpp` 按 `WalkReport`
逐条记账，`extract.cpp` 自己拼一句 `ec.message()` ⇒ 同一个"扫不动"在两个页面上**说法不同**；
排序也各自实现（`collect_tree` 是相对路径 UTF-8 字节序，`extract.cpp` 是 Unicode 小写折叠序），
而候选顺序直接决定 `files/strings/chars/skipped` 台账与产物 XML 的条目次序。

判据（三条都要有牙）
--------------------
① **候选收集必须转调**：`src/engine/text/extract.cpp` 与 `src/engine/text/batch.cpp`
   必须用 `collect_tree` ＋ `WalkReport`（这两处是同一个语义"收集候选文件再逐个处理"）；
② `extract.cpp` 的出错路径必须把台账里的理由**带出去**（`walk.errors`）；
③ **全仓库**出现 `recursive_directory_iterator` 的文件必须在**点名清单**里 ——
   任何**新**写一份的地方都要在这里登记并写清"为什么不能复用"（顺手把"忘登记"变成红灯）。
   ★ `src/engine/text/extract.cpp` **不在**清单里：它已经转调了（这一条就是本批的成果）。

历史上出现过遍历的地方（登记，不是"允许永远不收敛"）
----------------------------------------------------
| 文件 | 为什么现在可以留 |
| --- | --- |
| `src/engine/fs/overlay.cpp` | `collect_tree` 本体（唯一实现） |
| `src/engine/font/charset.cpp` | 它要"**按文件名升序**的 `*.xml`"给字库页；`collect_tree` 的口径是相对路径 UTF-8 字节序 ⇒ 少了排序会改语料次序（**过渡**，见记档） |
| `src/engine/xml/dirdiff.cpp` | 目录对比要**两侧各自的错误集合**（出错后继续收集，不是"整批不做"）；`WalkReport` 是**单一** errors 列表 ⇒ 语义不同（**过渡**，见记档） |
| `tools/cli/main.cpp` | 只用来**挑包文件**（不产出候选台账）（**过渡**，见记档） |

输出：`WALK-ONE ok …` / `WALK-ONE fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("src", "tools")
ONLY_OWNER = "src/engine/fs/overlay.cpp"
# ★ 允许**暂时**自己写遍历的文件（每个都要能说出"为什么不能复用"，见文件头那张表）
ALLOWED = (
    "src/engine/fs/overlay.cpp",
    "src/engine/font/charset.cpp",
    "src/engine/xml/dirdiff.cpp",
    "tools/cli/main.cpp",
)
EXTRACT = ROOT / "src" / "engine" / "text" / "extract.cpp"
BATCH = ROOT / "src" / "engine" / "text" / "batch.cpp"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def traversal_owners() -> list:
    """哪些文件**真的**写了 `recursive_directory_iterator`（剥注释后判，说明里引用不算）。"""
    owners = []
    for top in SCAN_DIRS:
        base = ROOT / top
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            if "recursive_directory_iterator" in body:
                owners.append(path.relative_to(ROOT).as_posix())
    return sorted(owners)


def owners_ok(owners: list) -> bool:
    """口径：本体必须在；其余出现遍历的文件都必须**在登记清单里**。"""
    return ONLY_OWNER in owners and all(o in ALLOWED for o in owners)


def owners_problems(owners: list) -> list:
    """`owners` 的**问题清单**（实测与反例自证共用这一个判断，免得两边口径漂）。"""
    problems = []
    if ONLY_OWNER not in owners:
        problems.append("`collect_tree` 本体不见了（%s 里没有遍历）" % ONLY_OWNER)
    rogue = [o for o in owners if o not in ALLOWED]
    if rogue:
        problems.append("有文件自己写遍历却**没登记**（登记清单见 check_walk_single_source.py 的 "
                        "ALLOWED）：" + ", ".join(rogue))
    return problems


def audit_reexport(path: Path, label: str) -> list:
    problems = []
    body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    if "collect_tree(" not in body:
        problems.append("%s：没有转调 `collect_tree(`（还在自己遍历？）" % label)
    if "WalkReport" not in body:
        problems.append("%s：没有用 `WalkReport` 台账（出错语义会与别的路漂）" % label)
    return problems


def audit_extract_error_path(path: Path) -> list:
    return audit_error_path_text(path.read_text(encoding="utf-8", errors="replace"))


def audit_error_path_text(body: str) -> list:
    """对**一段文本**判断"遍历出错有没有把台账理由带出去"（反例自证要能喂假文本）。

    ★ 口径（本判据第一版太松、**出过一次假绿**）：光出现 `walk.errors` 不算 ——
      必须**真的取台账里的那一条理由**（`walk.errors.front()`）拼进给用户看的那句话里。
      只写 `walk.errors.size()` 或只写 `walk.errors.empty()` 都**不算**（那样用户还是看不到原因）。
    """
    body = strip_comments(body)
    if "walk.errors.front()" not in body:
        return ["src/engine/text/extract.cpp：遍历出错没有把台账里的**理由**带出去"
                "（期望 `walk.errors.front()` 出现在出错文案里；只数条数不算 —— 零静默口径要求理由可见）"]
    return []


def selfcheck() -> tuple:
    """反例自证：每个反例都**真喂进**被测判断（不许写个 `if` 表达式自欺）。"""
    overlay_only = "std::filesystem::recursive_directory_iterator it(x);"
    extract_ok = "fs::collect_tree(root, &walk); if (!walk.errors.empty()) { }"
    cases = [
        ("只有 overlay 有遍历", 0,
         owners_problems(owners_of({"src/engine/fs/overlay.cpp": overlay_only}))),
        # ★ 反例：**新写一份且没登记** ⇒ 必须红（用与实测**同一个** owners_problems）
        ("新写一份却没登记", 1,
         owners_problems(owners_of({"src/engine/fs/overlay.cpp": overlay_only,
                                    "src/engine/text/rogue.cpp": overlay_only}))),
        # ★ 正例：登记清单里的例外（charset/dirdiff/main）**不算**问题
        ("登记过的例外不算问题", 0,
         owners_problems(owners_of({"src/engine/fs/overlay.cpp": overlay_only,
                                    "src/engine/font/charset.cpp": overlay_only,
                                    "src/engine/xml/dirdiff.cpp": overlay_only,
                                    "tools/cli/main.cpp": overlay_only}))),
        ("注释里提到不算", 0,
         owners_problems(owners_of({
             # 本体：一条说明注释 ＋ 真代码 ⇒ 仍算"出现遍历"（但它本身在清单里，不成问题）
             "src/engine/fs/overlay.cpp": "// 见 recursive_directory_iterator 的说明\n" + overlay_only,
             # extract：注释里提到遍历、代码里没有 ⇒ **不该**被算成"又写了一份"
             "src/engine/text/extract.cpp": "// 别再用 recursive_directory_iterator\n" + extract_ok}))),
        ("extract 没转调", 1, audit_reexport_text("std::ifstream in;", "extract.cpp")),
        ("batch 没转调", 1, audit_reexport_text("std::ifstream in;", "batch.cpp")),
        # ★ 真喂一段"转调了、但没把台账理由带出去"的文本
        ("extract 出错不带台账", 1,
         audit_error_path_text("if (false) { }")),
        # ★ 反例：**只数条数**、不取那一条理由 ⇒ 仍要红（本判据第一版就是栽在这，假绿过）
        ("extract 只数条数不算", 1,
         audit_error_path_text("return std::to_string(walk.errors.size());")),
        ("extract 出错带台账（正例）", 0,
         audit_error_path_text('return "\u76ee\u5f55\u904d\u5386\u51fa\u9519\uff1a" + walk.errors.front();')),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good_case = (len(problems) > 0) if want else (not problems)
        ok &= good_case
        results.append((label, want, len(problems), good_case, problems))
    return ok, results


def audit_reexport_text(body: str, label: str) -> list:
    """对**一段文本**做与 `audit_reexport` 相同的判断（反例自证要能喂假文本）。"""
    body = strip_comments(body)
    problems = []
    if "collect_tree(" not in body:
        problems.append("%s：没有转调 `collect_tree(`（还在自己遍历？）" % label)
    if "WalkReport" not in body:
        problems.append("%s：没有用 `WalkReport` 台账（出错语义会与别的路漂）" % label)
    return problems


def owners_of(files: dict) -> list:
    return sorted(p for p, body in files.items()
                  if "recursive_directory_iterator" in strip_comments(body))



def main() -> int:
    print("== P17-102：目录遍历只许有一份实现（审计 `02 F-14`）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-24s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                "有错" if want else "无错", got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("WALK-ONE fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (EXTRACT, BATCH):
        if not path.is_file():
            print("WALK-ONE fail 仓库自带文件缺失：%s" % path)
            return 1

    problems = []
    owners = traversal_owners()
    problems += owners_problems(owners)
    problems += audit_reexport(EXTRACT, "src/engine/text/extract.cpp")
    problems += audit_reexport(BATCH, "src/engine/text/batch.cpp")
    problems += audit_extract_error_path(EXTRACT)

    print("-- 实测 --")
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("WALK-ONE fail %d 项不成立" % len(problems))
        return 1
    print("   ✓ 出现遍历的文件全在登记清单里（本体 %s ＋ %d 处有理由的例外）"
          % (ONLY_OWNER, len(owners) - 1))
    print("   ✓ `text/extract.cpp` 转调 `collect_tree` ＋ `WalkReport`，出错把 `walk.errors` 带出去")
    print("   ✓ `text/batch.cpp` 仍在转调（没有「修一个退一个」）")
    print()
    print("WALK-ONE ok 目录遍历只有一份实现（审计 02 F-14）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
