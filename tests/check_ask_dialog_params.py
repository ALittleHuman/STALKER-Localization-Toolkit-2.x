# -*- coding: utf-8 -*-
"""P17-83 静态判据：`ask_*` 的**参数缺口**补上了没有 —— pybind 收得下、宿主真的用起来。

为什么要它（这条缺口的事实）
----------------------------
1.x 的契约（`toolkit_plugin_ui.py`，AST 里的权威口径）：

```python
def ask_open_file(self, parent, title, filetypes=None, initialdir=""):            # :327
def ask_save_file(self, parent, title, initialdir="", initialfile="", filetypes=None):  # :336
```

2.x 原先只收 `title`（`py_host.cpp` 的 `.def("ask_open_file", …, py::arg("title"))`），
`widget_facade.h` 的 `std::function` 也只有 `title` ⇒ **插件照 1.x 写的三个参数被静默丢掉**：
对话框永远从默认目录开、也不带过滤类型（用户要的"对话框起点记忆"就是这条）。

★ 为什么这条链**只能**用静态判据（本批实测的结论，不是偷懒）
------------------------------------------------------------
真实宿主实现会**发真对话框**（`QFileDialog` + `exec()`）。而 `--smoke` 是**离屏、无人值守**：
真发会阻塞；就算发了，"过滤串/起点目录"在无人环境里也**观察不到**（要人眼看）。
另一个看似可行的办法（插件侧猴补 `QFileDialog`）**行不通**：宿主是 C++ 直接链的 Qt，
Python 侧**根本没有 `PySide6`**（本批实测：`sys.modules['PySide6.QtWidgets']` 不存在）。
⇒ 口径：**能静态判的一律静态判**，把"这条判据到底在守什么"写在判据自己头上。

判据（四组，都能判红、都自带反例自证）
--------------------------------------
① `widget_facade.h`：`ask_open_file` / `ask_save_file` 两个字段的签名**参数个数与名字**
   回到 1.x（`title` ＋ `filetypes` ＋ `initialdir`［＋ `initialfile`］）；
② `py_host.cpp`：pybind 的 `.def(...)` 逐项声明了这些**关键字参数**（插件照 1.x 写关键字也能调）；
③ `widget_facade_qt.cpp`：宿主实现把三个参数**真的用起来** —— 过滤串交给 `setNameFilters`、
   起点交给 `setDirectory`（经 `qt_start_dir`）、`initialfile` 交给 `selectFile`，
   并且**记住用户选过的目录**（`widget_facade_remember_dir`）供下一次当起点；
④ 宿主那条 `filetypes → Qt 过滤串` 的映射函数（`qt_name_filters`）**在**且被 `ask_*` 调用。

输出：`ASK-DIALOG ok …` / `ASK-DIALOG fail …`；退出码 0=绿 / 1=有断言不成立。
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
HEADER = ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h"
PY_HOST = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
QT_IMPL = ROOT / "src" / "shell" / "widget_facade_qt.cpp"


def strip_comments(text: str) -> str:
    """先剥注释再查（本仓踩过两次：判据自己的说明里引用了旧代码 ⇒ 在自己身上假绿）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def field_signature(header: str, name: str):
    """取结构体里 `<name>;` 那条 `std::function` 声明的**参数名序列**（取不到 ⇒ None）。"""
    body = strip_comments(header)
    start = body.find("std::function<")
    while start >= 0:
        end = body.find(";", start)
        if end < 0:
            return None
        decl = body[start:end]
        if re.search(r"\b%s\s*$" % re.escape(name), decl.strip().split("\n")[-1]) or \
           re.search(r"\b%s\b" % re.escape(name), decl):
            args = re.findall(r"\b([a-z_][a-z0-9_]*)\s*(?:,|$)", decl)
            return decl, args
        start = body.find("std::function<", end)
    return None


def audit_header(text: str) -> list:
    problems = []
    for name, want in (("ask_open_file", ["title", "filetypes", "initialdir"]),
                       ("ask_save_file", ["title", "filetypes", "initialdir", "initialfile"])):
        found = field_signature(text, name)
        if found is None:
            problems.append("widget_facade.h 里找不到 `%s` 的 std::function 声明" % name)
            continue
        decl, _ = found
        for arg in want:
            # 参数名要出现在**签名**里（形如 `const std::string& initialdir`）
            if not re.search(r"\b%s\b" % re.escape(arg), decl):
                problems.append("widget_facade.h：`%s` 的签名里没有参数 `%s` —— "
                                "1.x 有这个参数，丢了就是静默丢弃（P17-83）" % (name, arg))
    return problems


def audit_pybind(text: str) -> list:
    problems = []
    code = strip_comments(text)
    for name, want in (("ask_open_file", ["title", "filetypes", "initialdir"]),
                       ("ask_save_file", ["title", "filetypes", "initialdir", "initialfile"])):
        match = re.search(r'\.def\(\s*"%s"(?:[^()]|\([^()]*\))*\)' % re.escape(name), code,
                          flags=re.S)
        if match is None:
            problems.append("py_host.cpp 里没有 `.def(\"%s\", …)`" % name)
            continue
        decl = match.group(0)
        for arg in want:
            if 'py::arg("%s")' % arg not in decl:
                problems.append("py_host.cpp：`.def(\"%s\", …)` 没有声明关键字参数 `%s` —— "
                                "插件照 1.x 写关键字会 TypeError（P17-83）" % (name, arg))
    return problems


def audit_qt_impl(text: str) -> list:
    problems = []
    code = strip_comments(text)
    for needle, why in (
            ("qt_name_filters", "`filetypes` 没有映射到 Qt 过滤串（宿主实现里没有 `qt_name_filters`）"),
            ("setNameFilters", "宿主没有把过滤串交给对话框（找不到 `setNameFilters`）"),
            ("qt_start_dir", "起点目录没有归一（找不到 `qt_start_dir`）"),
            ("setDirectory", "宿主没有把起点交给对话框（找不到 `setDirectory`）"),
            ("selectFile", "`initialfile` 没有预填（找不到 `selectFile`）"),
            ("widget_facade_remember_dir", "宿主没有记住用户选过的目录 ⇒ 没有「起点记忆」"),
    ):
        if needle not in code:
            problems.append("widget_facade_qt.cpp：%s" % why)
    # `ask_*` 两处 lambda 必须**真的**用上那三件（不是只定义函数没人调）
    #   ★ 用**原文**找 lambda：`/* ask_open_file */` 那种标签是注释，`strip_comments` 会把它删掉
    #     （本批自己踩到一次：剥完注释就再也找不到标签了）⇒ 行为面查剥过的文本、标签面查原文。
    for name, arg_count in (("ask_open_file", 3), ("ask_save_file", 4)):
        # ★ 标签注释可能**跨行**（本文件的实际写法就是多行 `/* ask_open_file：… */`）⇒
        #   注释体按"非 `*/` 的任意字符"匹配，别用 `[^\n]*`（那会在跨行的标签上失效）。
        match = re.search(r"/\*\s*%s(?:[^*]|\*(?!/))*\*/\s*\[\]\(([^)]*)\)" % re.escape(name),
                          text, flags=re.S)
        if match is None:
            problems.append("widget_facade_qt.cpp：找不到 `%s` 的实现 lambda" % name)
            continue
        params = [p.strip() for p in match.group(1).split(",") if p.strip()]
        if len(params) != arg_count:
            problems.append("widget_facade_qt.cpp：`%s` 的实现只收 %d 个参数（门面契约要 %d 个）"
                            % (name, len(params), arg_count))
    return problems


def selfcheck() -> tuple:
    """反例自证：喂"修前那种只收 title"的形状，必须报红。"""
    good_header = (
        "std::function<std::string(const std::string& title, const std::string& filetypes,\n"
        "                          const std::string& initialdir)> ask_open_file;\n"
        "std::function<std::string(const std::string& title, const std::string& filetypes,\n"
        "                          const std::string& initialdir, const std::string& initialfile)>\n"
        "    ask_save_file;\n")
    good_py = ('.def("ask_open_file", &PyUi::ask_open_file, py::arg("title"),\n'
               '     py::arg("filetypes") = py::none(), py::arg("initialdir") = std::string())\n'
               '.def("ask_save_file", &PyUi::ask_save_file, py::arg("title"),\n'
               '     py::arg("filetypes") = py::none(), py::arg("initialdir") = std::string(),\n'
               '     py::arg("initialfile") = std::string())\n')
    good_qt = (
        "QStringList qt_name_filters(const std::string& filetypes) { return {}; }\n"
        "QString qt_start_dir(const std::string& d) { return {}; }\n"
        "void widget_facade_remember_dir(const QString&) {}\n"
        "/* ask_open_file */\n"
        "[](const std::string& title, const std::string& filetypes, const std::string& initialdir) {\n"
        "    dialog.setNameFilters(qt_name_filters(filetypes));\n"
        "    dialog.setDirectory(qt_start_dir(initialdir));\n"
        "    widget_facade_remember_dir(QString());\n"
        "}\n"
        "/* ask_save_file */\n"
        "[](const std::string& title, const std::string& filetypes, const std::string& initialdir,\n"
        "   const std::string& initialfile) {\n"
        "    dialog.setNameFilters(qt_name_filters(filetypes));\n"
        "    dialog.setDirectory(qt_start_dir(initialdir));\n"
        "    dialog.selectFile(QString());\n"
        "    widget_facade_remember_dir(QString());\n"
        "}\n")
    cases = [
        ("好形状", 0, audit_header(good_header) + audit_pybind(good_py) + audit_qt_impl(good_qt)),
        ("门面只收 title（修前形状）", 1, audit_header(
            "std::function<std::string(const std::string& title)> ask_open_file;\n"
            "std::function<std::string(const std::string& title)> ask_save_file;\n")),
        ("pybind 只声明 title（修前形状）", 1, audit_pybind(
            '.def("ask_open_file", &PyUi::ask_open_file, py::arg("title"))\n'
            '.def("ask_save_file", &PyUi::ask_save_file, py::arg("title"))\n')),
        ("宿主收下参数却不用（静默丢弃）", 1, audit_qt_impl(
            "/* ask_open_file */\n"
            "[](const std::string& title, const std::string& filetypes, const std::string& initialdir) {\n"
            "    (void)filetypes; (void)initialdir;\n"
            "}\n"
            "/* ask_save_file */\n"
            "[](const std::string& title, const std::string& filetypes, const std::string& initialdir,\n"
            "   const std::string& initialfile) {}\n")),
        ("没有起点记忆", 1, audit_qt_impl(good_qt.replace("widget_facade_remember_dir", "noop"))),
    ]
    results, ok = [], True
    for label, want_min, problems in cases:
        good = len(problems) >= want_min if want_min else not problems
        ok &= good
        results.append((label, want_min, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-83：`ask_*` 参数缺口（pybind 收得下 ＋ 宿主真的用起来）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_min, got, good, problems in results:
        print("   %s %-26s 期望≥%d 实得=%d" % ("✓" if good else "✗", label, want_min, got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("ASK-DIALOG fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (HEADER, PY_HOST, QT_IMPL):
        if not path.is_file():
            print("ASK-DIALOG fail 仓库自带文件缺失：%s" % path)
            return 1
    problems = audit_header(HEADER.read_text(encoding="utf-8"))
    problems += audit_pybind(PY_HOST.read_text(encoding="utf-8"))
    problems += audit_qt_impl(QT_IMPL.read_text(encoding="utf-8"))
    print("-- 实测 --")
    print("   门面：ask_open_file(title, filetypes, initialdir) / "
          "ask_save_file(title, filetypes, initialdir, initialfile)")
    print("   宿主：qt_name_filters / qt_start_dir / setNameFilters / setDirectory / "
          "selectFile / widget_facade_remember_dir")
    print()
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("ASK-DIALOG fail %d 项不成立" % len(problems))
        return 1
    print("ASK-DIALOG ok `ask_*` 收下 1.x 的三个参数（pybind 关键字）＋ 宿主把过滤串/起点/"
          "默认文件名/起点记忆四处都接上了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
