# -*- coding: utf-8 -*-
"""P17-85 静态判据：`01 F-09` 的**可观测链**从引擎一路接到三处消费点（别断在任何一环）。

背景（为什么这条链需要一条"接线判据"）
--------------------------------------
审计 `01 F-09` 要的是"大 DB 整文件读口径**可观测**"：既要知道**读了多少**，也要知道**为什么**。
引擎半（`db.h` 的 `header_bytes`/`used_seek`/`whole_file_reason` ＋ `db.cpp` 的退回点写理由）
P17-70 已落，CLI 半 P17-71 已落 —— **断在半路的是**：

```
overlay.cpp :: package_entries()   ← 拿到 DbListing 之后**只搬 entries**，三个诊断当场丢掉
  → engine_facade_impl.cpp          ← 门面这条 API 只有 (path, fallback, error)
    → py_host.cpp                   ← Python dict 只有 entries/error
    → fs_page.cpp                   ← 官方页根本不知道"这次是整包读的"
```

P17-85 把出参（`DbReadInfo*` / `FsPackageInfo*`，默认 `nullptr` ⇒ 既有调用点行为不变）一路接出来。
本判据盯的就是"**这条链接头**"—— 它最容易出现的失败形状不是编译错误，而是：

* 加了出参但**没人填**（`overlay.cpp` 忘了抄诊断 ⇒ 消费点永远收到"没退回"）；
* 填了但**消费点没用**（界面不打、Python 不给键 ⇒ 等于没做）；
* 官方页**三处调用点只改了一处**（"分两处各修一半"）。

判据（五组，都能判红、自带反例自证）
------------------------------------
① `overlay.cpp`：DB 分支必须把 `listed` 的三个诊断抄进 `read_info`（且出参先清零）；
② `engine_facade_impl.cpp`：实现必须**收下**出参并**填**它；
③ `py_host.cpp`：dict 里必须有 `used_seek` / `header_bytes` / `whole_file_reason` 三个键；
④ `fs_page.cpp`：**三处** `package_entries(` 调用**都**传了出参 ＋ 都调了 `note_package_read`
   （判据按"调用点计数"对账，不是"出现过就算"）；
⑤ Python 夹具（`tests/python_plugins_engine/engine_demo.py`）打了两条新标记
   （`pkg_readinfo_keys` / `pkg_readinfo_quiet`），且 `run_ci.py` 的预期表里**要求它们是 1**。

输出：`F09-READINFO ok …` / `F09-READINFO fail …`；退出码 0=绿 / 1=有断言不成立。
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
OVERLAY = ROOT / "src" / "engine" / "fs" / "overlay.cpp"
FACADE_IMPL = ROOT / "src" / "engine" / "plugin" / "engine_facade_impl.cpp"
PY_HOST = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
FS_PAGE = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"
ENGINE_FIXTURE = ROOT / "tests" / "python_plugins_engine" / "engine_demo.py"
RUN_CI = ROOT / "tests" / "run_ci.py"


def strip_comments(text: str) -> str:
    """查**代码**时先剥注释（本仓踩过：判据说明里引用旧代码 ⇒ 在自己身上假绿）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def audit_overlay(text: str) -> list:
    problems = []
    code = strip_comments(text)
    for needle, why in (
            ("read_info->whole_file_reason", "诊断没抄进出参（`whole_file_reason` 没填）"),
            ("read_info->used_seek", "诊断没抄进出参（`used_seek` 没填）"),
            ("read_info->header_bytes", "诊断没抄进出参（`header_bytes` 没填）"),
            ("*read_info = DbReadInfo{}", "出参没先清零（调用方复用变量时会读到上一包的值）"),
    ):
        if needle not in code:
            problems.append("overlay.cpp：%s" % why)
    return problems


def audit_facade_impl(text: str) -> list:
    problems = []
    code = strip_comments(text)
    match = re.search(r"/\*\s*fs_package_entries(?:[^*]|\*(?!/))*\*/", text)
    if match is None:
        problems.append("engine_facade_impl.cpp：找不到 `fs_package_entries` 的实现块标签")
        return problems
    tail = text[match.start():match.start() + 2000]
    for needle, why in (
            ("FsPackageInfo* info", "实现没**收下**出参"),
            ("info->whole_file_reason", "实现没**填**出参（`whole_file_reason` 缺失）"),
            ("info->used_seek", "实现没填 `used_seek`"),
            ("&read_info", "实现没有向引擎要诊断（调用 `package_entries` 时没传 `&read_info`）"),
    ):
        if needle not in tail:
            problems.append("engine_facade_impl.cpp：%s" % why)
    return problems


def audit_py_host(text: str) -> list:
    problems = []
    code = strip_comments(text)
    match = re.search(r"py::object fs_package_entries\([^)]*\)\s*const\s*\{", code)
    if match is None:
        problems.append("py_host.cpp：找不到 `fs_package_entries` 的实现")
        return problems
    body = code[match.start():match.start() + 1200]
    for key in ("used_seek", "header_bytes", "whole_file_reason"):
        if 'out["%s"]' % key not in body:
            problems.append("py_host.cpp：Python dict 里没有 `%s`" % key)
    return problems


def audit_fs_page(text: str) -> list:
    problems = []
    code = strip_comments(text)
    # ★ 数"**调用**"要减掉**定义**那一处 —— 本判据第一版用 `(?<![\\w:])` 想把 `FsPage::note_package_read(`
    #   排掉，结果把 `efs::package_entries(` 这种**本该算**的调用也一起排掉了（同一条正则两种情形：
    #   `efs::` 后面是**调用**、`FsPage::` 后面是**定义**）。改用"按行看是不是定义行"：
    #   定义行的形状是"`<返回类型> FsPage::<名字>(`" ⇒ 出现两个词（类型 + 限定名）。
    def is_definition(line: str) -> bool:
        # 定义行的形状：`<返回类型> <类>::<名字>(`，即限定名里带**类名**（`FsPage::…`），
        # 而且名字**不属于**任何命名空间（`efs::package_entries` 是调用、不是定义 ——
        # 本判据第一版在这里翻过两次车，所以把"名字叫什么"也写进判据）。
        head = line.split("(")[0]
        if "::" not in head or len(head.split()) < 2:
            return False
        qualifier = head.split()[-1].split("::")[0].lstrip("&*")
        return qualifier not in ("efs", "fs", "std", "xrkit", "plugin", "core")

    def count(lines, name: str) -> int:
        total = 0
        for line in lines:
            total += line.count(name + "(")
        return total

    def count_definitions(lines, name: str) -> int:
        return sum(1 for line in lines if name + "(" in line and is_definition(line))

    lines = code.splitlines()
    calls = count(lines, "package_entries") - count_definitions(lines, "package_entries")
    notes = count(lines, "note_package_read") - count_definitions(lines, "note_package_read")
    with_out = len(re.findall(r"package_entries\([^;]*?&read_info\)", code, flags=re.S))
    if calls < 3:
        problems.append("fs_page.cpp：只找到 %d 处 `package_entries(` 调用（应当 ≥3 —— "
                        "加载 / 单源重载 / 加载选中）" % calls)
    if with_out != 3:
        problems.append("fs_page.cpp：只有 %d 处调用传了 `&read_info`（**三处都要**，"
                        "只改一处就是「分两处各修一半」）" % with_out)
    if notes != 3:
        problems.append("fs_page.cpp：只有 %d 处**调用** `note_package_read(...)`（三处消费点各一处；"
                        "`FsPage::note_package_read` 那个定义不计）" % notes)
    return problems


def audit_fixture(text: str, run_ci_text: str) -> list:
    problems = []
    for key in ("pkg_readinfo_keys", "pkg_readinfo_quiet"):
        if key not in text:
            problems.append("engine_demo.py：没有打 `%s` 标记" % key)
        if '"%s": "1"' % key not in run_ci_text:
            problems.append("run_ci.py：预期表里**要求** `%s` = 1（标记打了但没人判 ⇒ 假绿）" % key)
    return problems


def selfcheck() -> tuple:
    good_overlay = ("if (read_info != nullptr) *read_info = DbReadInfo{};\n"
                    "read_info->header_bytes = listed.header_bytes;\n"
                    "read_info->used_seek = listed.used_seek;\n"
                    "read_info->whole_file_reason = listed.whole_file_reason;\n")
    good_impl = ("/* fs_package_entries：… */\n"
                 "[](const std::string& p, const std::string& k, std::string& e,\n"
                 "   FsPackageInfo* info = nullptr) {\n"
                 "  efs::DbReadInfo read_info;\n"
                 "  efs::package_entries(p, k, e, nullptr, &read_info);\n"
                 "  info->used_seek = read_info.used_seek;\n"
                 "  info->whole_file_reason = read_info.whole_file_reason;\n"
                 "  return out;\n"
                 "}\n")
    good_pyhost = ('py::object fs_package_entries(const std::string& p, const std::string& k) const {\n'
                   '  out["used_seek"] = info.used_seek;\n'
                   '  out["header_bytes"] = info.header_bytes;\n'
                   '  out["whole_file_reason"] = info.whole_file_reason;\n'
                   '}\n')
    good_page = ("entries = efs::package_entries(a, b, error, registry_, &read_info);\n"
                 "note_package_read(QStringLiteral(\"列条目\"), read_info.header_bytes,"
                 " read_info.used_seek, read_info.whole_file_reason, raw);\n") * 3
    good_fixture = ('mark("pkg_readinfo_keys:%d" % 1)\nmark("pkg_readinfo_quiet:%d" % 1)\n')
    good_run_ci = '"pkg_readinfo_keys": "1",\n"pkg_readinfo_quiet": "1",\n'

    cases = [
        ("好形状", 0, audit_overlay(good_overlay) + audit_facade_impl(good_impl)
         + audit_py_host(good_pyhost) + audit_fs_page(good_page)
         + audit_fixture(good_fixture, good_run_ci)),
        ("出参没人填（修前形状）", 1,
         audit_overlay("if (read_info != nullptr) *read_info = DbReadInfo{};\n")),
        ("门面实现收下不填", 1, audit_facade_impl(
            "/* fs_package_entries：x */\n"
            "[](const std::string& p, const std::string& k, std::string& e,\n"
            "   FsPackageInfo* info = nullptr) { return out; }\n")),
        ("Python 少一个键", 1, audit_py_host(
            'py::object fs_package_entries(const std::string& p, const std::string& k) const {\n'
            '  out["used_seek"] = 1; out["header_bytes"] = 2;\n}\n')),
        ("官方页只改一处", 1, audit_fs_page(
            "package_entries(a, b, error, registry_, &read_info);\n"
            "note_package_read(x);\n")),
        ("夹具打了标记但没人判", 1, audit_fixture(
            good_fixture, '"pkg_readinfo_keys": "1",\n')),
    ]
    results, ok = [], True
    for label, want_min, problems in cases:
        good = len(problems) >= want_min if want_min else not problems
        ok &= good
        results.append((label, want_min, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-85：`01 F-09` 可观测链（引擎 → 门面 → 三处消费点）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_min, got, good, problems in results:
        print("   %s %-24s 期望≥%d 实得=%d" % ("✓" if good else "✗", label, want_min, got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("F09-READINFO fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (OVERLAY, FACADE_IMPL, PY_HOST, FS_PAGE, ENGINE_FIXTURE, RUN_CI):
        if not path.is_file():
            print("F09-READINFO fail 仓库自带文件缺失：%s" % path)
            return 1
    problems = audit_overlay(OVERLAY.read_text(encoding="utf-8"))
    problems += audit_facade_impl(FACADE_IMPL.read_text(encoding="utf-8"))
    problems += audit_py_host(PY_HOST.read_text(encoding="utf-8"))
    problems += audit_fs_page(FS_PAGE.read_text(encoding="utf-8"))
    problems += audit_fixture(ENGINE_FIXTURE.read_text(encoding="utf-8"),
                             RUN_CI.read_text(encoding="utf-8"))
    print("-- 实测 --")
    print("   overlay.cpp：DB 分支抄诊断（header_bytes / used_seek / whole_file_reason）＋ 出参清零")
    print("   engine_facade_impl.cpp：收下并填 FsPackageInfo")
    print("   py_host.cpp：Python dict 三键")
    print("   fs_page.cpp：三处调用点都传出参、都打 note_package_read")
    print("   engine_demo.py ＋ run_ci.py：两条新标记都在预期表里")
    print()
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("F09-READINFO fail %d 项不成立" % len(problems))
        return 1
    print("F09-READINFO ok `01 F-09` 的可观测链三处消费点全通（引擎诊断 → 门面出参 → 官方页/插件页）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
