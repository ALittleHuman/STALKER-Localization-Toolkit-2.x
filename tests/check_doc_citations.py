# -*- coding: utf-8 -*-
"""判据引用的文档**必须真的存在**（P16-0b）。

为什么要有它：另一个会话 2026-10-09 的只读核对发现 `tests/check_page_host_lock.py:3`
把 `docs/2.0.0-插件宿主规格.md` 引为**判据来源**，而这份文档**全仓不存在**（全库搜索只剩那一行引用）。
判据继承一份不存在的规格 ⇒ 后来的人无从核对"这条锁到底在守哪句话"。

判据
----
扫 `tests/*.py`、`tools/*.py`、`plugins/*.py` 里出现的 `docs/<名字>.md` 引用，
逐个核对 `ROOT/docs/<名字>` 是否存在；缺一件即红（逐处报 `文件:行号` + 文档名）。

★ 判据**不**要求"每份文档都被引用"（那是另一件事）；只守"引用了就得有"这一个方向。
★ 引用写在注释/docstring 里也算（本锁**不去注释**）：判据来源就该能被读到。

输出：`DOC-CITE ok n 处引用全部存在` / `DOC-CITE fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证：合成一处存在引用 + 一处不存在引用，必须只报后者。
"""

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("tests", "tools", "plugins")
DOC_REF_RE = re.compile(r"docs[/\\]([0-9A-Za-z\u4e00-\u9fff_\-\.\(\)（）]+?\.md)")


def collect_refs(source: str):
    """返回 [(行号, 文档名)]。"""
    out = []
    for index, line in enumerate(source.splitlines(), start=1):
        for name in DOC_REF_RE.findall(line):
            out.append((index, name))
    return out


def audit(source: str, docs_dir: Path, exists_fn=None):
    """返回 (问题清单, 引用总数)。"""
    exists_fn = exists_fn or (lambda n: (docs_dir / n).exists())
    problems = []
    refs = collect_refs(source)
    for line_no, name in refs:
        if not exists_fn(name):
            problems.append("第 %d 行引用了 `docs/%s`，但这份文档**不存在**" % (line_no, name))
    return problems, len(refs)


def selfcheck():
    def fake_exists(name):
        return name == "2.0.0-存在.md"

    cases = [
        ("引用存在", "见 `docs/2.0.0-存在.md` §一。", 0),
        ("引用不存在", "见 `docs/2.0.0-不存在.md` §一。", 1),
        ("两条引用各一处", "docs/2.0.0-存在.md 与 docs/2.0.0-不存在.md", 1),
        ("没有引用", "这条锁不看文档。", 0),
    ]
    results, ok = [], True
    for label, source, want in cases:
        problems, _n = audit(source, Path("."), fake_exists)
        good = len(problems) == want
        ok &= good
        results.append((label, want, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== 判据引用的文档必须存在 ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-16s 期望=%d 实得=%d" % ("✓" if good else "✗", label, want, got))
        if not good:
            for p in problems[:2]:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("DOC-CITE fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    docs_dir = ROOT / "docs"
    if not docs_dir.is_dir():
        print("DOC-CITE fail 仓库自带目录缺失：%s" % docs_dir)
        return 1

    files = []
    for sub in SCAN_DIRS:
        files += sorted((ROOT / sub).glob("*.py"))
    # ★ 精确排除**本文件**：它的 docstring 与反例自证里按设计含"合成引用"
    #   （`docs/2.0.0-存在.md` 之类），扫自己必然假红。白名单只此一条、且是单个文件
    #   （不许写成"整个 tests/ 目录豁免"——那会让真引用消失得无声无息）。
    files = [p for p in files if p.resolve() != Path(__file__).resolve()]
    if not files:
        print("DOC-CITE fail 一个待扫文件都没有（集合为空不算通过）")
        return 1

    all_problems, total = [], 0
    for path in files:
        problems, n = audit(path.read_text(encoding="utf-8"), docs_dir)
        total += n
        for p in problems:
            all_problems.append("%s:%s" % (path.relative_to(ROOT), p))
    print("-- 实测：扫 %d 个文件，共 %d 处 `docs/*.md` 引用 --" % (len(files), total))
    print()
    if all_problems:
        for p in all_problems:
            print("   ✗ %s" % p)
        print()
        print("DOC-CITE fail %d 处引用了不存在的文档" % len(all_problems))
        return 1
    print("DOC-CITE ok %d 处引用全部存在（%d 个文件）" % (total, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
