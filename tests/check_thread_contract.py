# -*- coding: utf-8 -*-
"""**线程与取消契约**的静态判据（P17-1；审计 §一·1 / 03 F-01 / 03 F-02 / 05 PLG-01 / 05 PLG-07）。

为什么要有它（待办总表 §一·1「★先立判据（先证红，再修）」）
----------------------------------------------------------
一条判据覆盖三条缺陷，两条契约：

  ① **worker 不许碰 UI**（1.x `TaskRunner` 13.2 / `task_shell.h` 的 13.2 注释）；
  ② **取消必须真能读到并停**（用户红线：真停 + 留痕 + 理由可见）。

修前事实（门禁全绿，但两条契约一条都没有判据兜住 —— Hana 05 PLG-07 原话
「对比作者对『旧二进制假绿』『showIncludes 前缀假绿』的高度警觉，这一块是空白」）：

  · `src/shell/pages/fontpack_page.cpp`：`task_->run` 的 worker **直接**调 `build()`，
    而 `build()` 读下拉框、写 `result_label_`/`state_label_`、裸 `++refused_count_`
    ⇒ 跨线程碰 QWidget + 数据竞争；页析构时 `result_label_` 先于 `task_` 被删 ⇒ 访问违例
    （03 F-01，**Critical**）；
  · `plugins/{convert,text,fontpack,xml,video}_page.py`：worker 里出现 `ui.set_text` /
    `ui.log_view_append` / `ui.get_text`（都会碰控件），而门面的线程闸门把它们**静默**变成
    no-op ⇒ 页内状态与日志在 worker 阶段永远空白；
  · `ui.task_cancel_requested` 在 worker 里被闸门一刀切拒掉 ⇒ 取消关不掉、还谎报"已取消"
    （05 PLG-01，High）。

判据（两边各一条，都是**静态**的，跑门禁时不需要真机）
------------------------------------------------------
**A. C++ 内置页（`src/shell/pages/*.cpp`）**
  把传给任务壳的 worker 体（`task_->run(...)` 的第二个实参）抽出来，然后：
    A1. worker 体里**不许**直接出现控件成员（`xxx_label_` / `xxx_combo_` / `xxx_edit_` …）
        或控件写读方法（`->setText(` / `->currentData(` / `->isChecked(` / `->text()` …）；
    A2. worker 体里**不许**调用"会碰控件的页面成员函数" —— 逐个在同一个 .cpp 里查这些函数的
        定义体，体内出现控件成员/控件方法 ⇒ 该函数是"碰控件入口"（修前的 `build()`、
        `options_for()` 正落在这条上）。**除非**该函数体第一句就是 `ui_thread_guard(...)`
        （那是"可判形态"：碰控件的那条路自己会拒绝 + 记账 + smoke 里有运行期断言）。

**B. 官方 `.py` 插件（`plugins/*.py`）**
  用 AST 找出所有 `ui.task_run(panel, title, <worker>)` 的 worker 实参（函数名 / lambda），
  在 **worker 体内（含嵌套函数与 lambda）** 收集 `ui.<method>(...)` 调用：
    B1. 逐个必须落在**跨线程只读白名单**里（权威在 `src/engine/plugin/widget_facade.cpp` 的
        `kWorkerSafeMethods[]`，本脚本**读它**而不是抄它 —— 免得两处口径分叉）；
    B2. 其余 `ui.*` 一律红（要刷新界面就走 `ui.task_on_message`，它在主线程回调）。

输出：`THREAD-CONTRACT ok …` / `THREAD-CONTRACT fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证（`selfcheck()`）：每条判据各喂一个反例必须报红，干净样本必须报绿。
"""

import ast
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent

# ── B：跨线程只读白名单的**权威出处**（读它，不抄它）────────────────────────
FACADE_IMPL = ROOT / "src" / "engine" / "plugin" / "widget_facade.cpp"
FACADE_HEADER = ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h"

# ── A：控件成员 / 控件方法 / 任务壳起 worker 的入口 ──────────────────────────
_WIDGET_MEMBER_RE = re.compile(
    r"\b\w+_(?:label|combo|edit|box|button|list|tree|view|panel|bar|area|zone|picker|rows)\b")
_WIDGET_CALL_RE = re.compile(
    r"->(?:setText|setChecked|setCurrentIndex|currentText|currentData|currentIndex|"
    r"isChecked|text|toPlainText|setPlainText|addItem|clear|setValue|value|count|"
    r"topLevelItemCount|setRowCount|rowCount|setEnabled|isEnabled|setVisible)\s*\(")
#   `this->X(...)` / 裸 `X(...)` 都收进来，再按"这个名字是不是本文件的成员函数"过滤
#   （不是 ⇒ 当作引擎/标准库调用放行）。
_CALLEE_ARROW_RE = re.compile(r"->\s*([A-Za-z_]\w*)\s*\(")
_CALLEE_BARE_RE = re.compile(r"(?<![\w.:>])([A-Za-z_]\w*)\s*\(")
_RUN_CALL_RE = re.compile(r"(?:task_\s*->|task\(\)\s*->)\s*run\s*\(")
_GUARD_RE = re.compile(r"ui_thread_guard\s*\(")
#   排队回主线程的合法通道（剥掉它之后剩下的才是"真在 worker 里跑"的代码）
_DISPATCH_RE = re.compile(r"(?:QMetaObject\s*::\s*)?invokeMethod\s*\(")


def callees_in(text: str) -> set:
    return set(_CALLEE_ARROW_RE.findall(text)) | set(_CALLEE_BARE_RE.findall(text))


def extract_balanced(text: str, open_index: int, open_ch: str = "(", close_ch: str = ")") -> str:
    """从 `text[open_index]`（该位置应是 open_ch）起取到配对的 close_ch（含两端）。"""
    depth = 0
    in_str = None
    i = open_index
    while i < len(text):
        ch = text[i]
        if in_str is not None:
            if ch == "\\":
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in "\"'":
            in_str = ch
            i += 1
            continue
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return text[open_index:i + 1]
        i += 1
    return text[open_index:]


def split_top_level(inner: str) -> list:
    """按顶层逗号切（忽略字符串与括号里的逗号）。"""
    depth = 0
    in_str = None
    pieces, current = [], []
    for ch in inner:
        if in_str is not None:
            current.append(ch)
            if ch == in_str:
                in_str = None
            continue
        if ch in "\"'":
            in_str = ch
            current.append(ch)
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            pieces.append("".join(current))
            current = []
            continue
        current.append(ch)
    pieces.append("".join(current))
    return pieces


def strip_queued_dispatches(text: str) -> str:
    """把 `QMetaObject::invokeMethod(...)` **整段**换成等长空白。

    ★ 为什么必须有这一步：`invokeMethod(this, lambda, Qt::QueuedConnection)` 里的 lambda
    **在主线程跑** —— 这是本仓"worker 不碰控件"的**合法通道**（`convert_page.cpp:464-471`
    就是这么写的：worker 只算，收尾排队回主线程画）。不剥掉它，判据会把"排队回主线程"
    误判成"worker 碰控件"（**假红**）；假红比不判更坏 —— 它会把正确写法逼着改坏。
    """
    out = text
    while True:
        match = _DISPATCH_RE.search(out)
        if match is None:
            return out
        span = extract_balanced(out, match.end() - 1)
        out = out[:match.start()] + (" " * len(span)) + out[match.start() + len(span):]


def worker_bodies_cpp(source: str):
    """C++：抽出所有"传给任务壳 run() 的 worker 体"。返回 [(行号, 真正的 worker 代码)]。

    ★ 返回的是**剥掉排队回主线程那段之后**的代码（见 `strip_queued_dispatches`）。
    """
    out = []
    for match in _RUN_CALL_RE.finditer(source):
        call = extract_balanced(source, match.end() - 1)
        pieces = split_top_level(call[1:-1])
        if len(pieces) < 2:
            continue
        line = source[:match.start()].count("\n") + 1
        out.append((line, strip_queued_dispatches(pieces[1].strip())))
    return out


def cpp_member_functions(source: str) -> dict:
    """粗粒度地收集 `返回类型 类名::函数名(...)` 的定义体（本仓页面是这一种写法）。

    ★ 函数体同样剥掉排队回主线程那段：这样一个"自己把 UI 工作排队回去"的成员函数
    （`XmlPage::compare_dirs` 就是）不会被误判成"worker 碰控件"。
    """
    functions = {}
    for match in re.finditer(r"^[\w:<>,&\*\s]*?(\w+)::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{",
                             source, re.MULTILINE):
        name = match.group(2)
        body = strip_queued_dispatches(extract_balanced(source, match.end() - 1, "{", "}"))
        line = source[:match.start()].count("\n") + 1
        functions.setdefault(name, (line, body))
    return functions


def touches_widgets(text: str) -> str:
    """返回"碰控件"的证据（空串 ⇒ 不碰）。

    ★ 成员名后面紧跟 `(` 的**当函数调用**处理，不算"控件成员"—— 否则
    `refresh_rows()`（一个会碰树的方法）会被当成控件成员名匹配，把
    "调了个自己会判线程的方法"错判成"直接碰控件"。
    """
    for hit in _WIDGET_MEMBER_RE.finditer(text):
        tail = text[hit.end():].lstrip()
        if tail.startswith("("):
            continue
        return hit.group(0)
    hit = _WIDGET_CALL_RE.search(text)
    if hit:
        return "->" + hit.group(0)[2:].rstrip("(")
    return ""


# 「谁在碰控件」的**可判接缝**：写在第一次碰控件之前才作数。
#   · `ui_thread_guard(...)`    = 守卫：不在 UI 线程就**拒绝 + 记账**（本批的形态）
#   · `ui_thread_dispatch(...)` = 分发：自己判线程，把碰控件的部分排队回主线程
#   · `QCoreApplication::instance()->thread()` / `on_ui_thread()` = 同上的等价自判
# 只认"接缝在前"：接缝写在末尾、控件已经被改过 ⇒ 不算。
_SEAM_RE = re.compile(
    r"ui_thread_guard\s*\(|ui_thread_dispatch\s*\(|QCoreApplication\s*::\s*instance\s*\(\s*\)"
    r"|on_ui_thread\s*\(")


def seam_before_widgets(body: str) -> bool:
    seam = _SEAM_RE.search(body)
    if seam is None:
        return False
    at = len(body)
    for pattern in (_WIDGET_MEMBER_RE, _WIDGET_CALL_RE):
        for hit in pattern.finditer(body):
            if pattern is _WIDGET_MEMBER_RE and body[hit.end():].lstrip().startswith("("):
                continue
            at = min(at, hit.start())
            break
    return seam.start() < at


def guard_before_widgets(body: str) -> bool:
    """旧名（保留给自检/阅读）：= `seam_before_widgets`。"""
    return seam_before_widgets(body)


def strip_comments(source: str) -> str:
    """把 C/C++ 注释换成空白（保行号）—— 本仓注释密度极高，注释里的示例代码不算判据。"""
    out, i, n = [], 0, len(source)
    in_line = in_block = False
    in_str = None
    while i < n:
        ch = source[i]
        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue
        if in_block:
            if source.startswith("*/", i):
                in_block = False
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        if in_str is not None:
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(source[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if source.startswith("//", i):
            in_line = True
            out.append("  ")
            i += 2
            continue
        if source.startswith("/*", i):
            in_block = True
            out.append("  ")
            i += 2
            continue
        if ch in "\"'":
            in_str = ch
        out.append(ch)
        i += 1
    return "".join(out)


def audit_cpp_pages(pages_dir: Path):
    """A1/A2。返回 (问题清单, 统计)。"""
    problems = []
    stats = {"files": 0, "workers": 0}
    for path in sorted(pages_dir.glob("*.cpp")):
        raw = path.read_text(encoding="utf-8", errors="replace")
        source = strip_comments(raw)
        if not _RUN_CALL_RE.search(source):
            continue
        stats["files"] += 1
        members = cpp_member_functions(source)
        for line, worker in worker_bodies_cpp(source):
            stats["workers"] += 1
            hit = touches_widgets(worker)
            if hit:
                problems.append("%s:%d：worker 体里直接碰控件（`%s`）—— worker 只许调引擎 + "
                                "两个 sink；要刷新界面走主线程收尾回调/`task_on_message`"
                                % (path.name, line, hit))
                continue
            for callee in sorted(callees_in(worker)):
                if callee not in members:
                    continue
                def_line, body = members[callee]
                evidence = touches_widgets(body)
                if not evidence:
                    continue
                if guard_before_widgets(body):
                    continue  # 有 UI 线程守卫 ⇒ 可判形态（拒绝 + 记账，smoke 有运行期断言）
                problems.append("%s:%d：worker 体调了会碰控件的成员函数 `%s()`"
                                "（定义在 %s:%d，证据 `%s`）—— 要么把它拆成"
                                "「主线程读值 / worker 只算引擎 / 主线程画控件」，要么在该函数"
                                "首句加 `ui_thread_guard(\"%s\")`"
                                % (path.name, line, callee, path.name, def_line, evidence, callee))
    return problems, stats


# ── B：插件侧 AST 判据 ──────────────────────────────────────────────────────

def worker_safe_methods():
    """从 C++（唯一一份口径）读出跨线程只读白名单。读不到 ⇒ None（判红）。"""
    if not FACADE_IMPL.is_file():
        return None
    source = FACADE_IMPL.read_text(encoding="utf-8", errors="replace")
    at = source.find("kWorkerSafeMethods[]")
    if at < 0:
        return None
    body = source[at:]
    end = body.find("};")
    if end >= 0:
        body = body[:end]
    return set(re.findall(r'"([^"]+)"', body))


def collect_ui_calls(node: ast.AST, out: list) -> None:
    """收集 worker 体内（含嵌套函数/lambda）的 `ui.<method>(...)` 调用。"""
    for child in ast.walk(node):
        if not isinstance(child, ast.Call):
            continue
        func = child.func
        if not isinstance(func, ast.Attribute):
            continue
        value = func.value
        if isinstance(value, ast.Name):
            owner = value.id
        elif isinstance(value, ast.Attribute):
            owner = value.attr
        else:
            continue
        if owner == "ui":
            out.append((child.lineno, func.attr))


def worker_functions(tree: ast.AST) -> list:
    """找 `ui.task_run(panel, title, <worker>)` 的 worker **定义节点**。

    ★ 两个坑都在这里堵：
      ① 插件里几乎都是 `ui.task_run(panel, "t", worker)` —— 传的是**名字**；只往回走实参
         节点是不够的（名字节点里没有函数体）⇒ 先建"函数名 → 定义"表，把名字解析成
         `FunctionDef`/`Lambda` 再遍历它的体内；
      ② 同一个文件里往往有**多个同名 `worker`**（convert_page.py 三个任务各一个）⇒
         按"行号最靠近调用点的那个定义"解析，否则一处违规会被重复报 N 次。
    """
    defs = {}
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            defs.setdefault(node.name, []).append(node)

    found = []

    def resolve(node, call_line):
        if isinstance(node, ast.Name):
            candidates = defs.get(node.id, [])
            if not candidates:
                return
            before = [item for item in candidates if item.lineno <= call_line]
            chosen = max(before, key=lambda item: item.lineno) if before else candidates[0]
            found.append(chosen)
        else:
            found.append(node)   # Lambda / 内联表达式

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not isinstance(func, ast.Attribute) or func.attr != "task_run":
            continue
        if len(node.args) >= 3:
            resolve(node.args[2], node.lineno)
        else:
            for keyword in node.keywords:
                if keyword.arg in ("worker", "fn", "func", "run"):
                    resolve(keyword.value, node.lineno)
    return found


def audit_plugins(plugin_dir: Path, safe_methods: set):
    """B1/B2。返回 (问题清单, 统计)。"""
    problems = []
    stats = {"files": 0, "workers": 0, "ui_calls": 0}
    for path in sorted(plugin_dir.glob("*.py")):
        source = path.read_text(encoding="utf-8")
        try:
            tree = ast.parse(source)
        except SyntaxError as exc:
            problems.append("%s：语法错误（%s）" % (path.name, exc))
            continue
        stats["files"] += 1
        for worker in worker_functions(tree):
            stats["workers"] += 1
            calls = []
            collect_ui_calls(worker, calls)
            for line, method in calls:
                stats["ui_calls"] += 1
                if method in safe_methods:
                    continue
                problems.append(
                    "%s:%d：worker 体里调 `ui.%s(...)` —— 工作线程只许调两个 sink"
                    "（`progress`/`message`）＋跨线程只读白名单（%s）；要刷新界面走 "
                    "`ui.task_on_message`（回调在主线程）"
                    % (path.name, line, method, "、".join(sorted(safe_methods))))
    return problems, stats


# ── 反例自证 ────────────────────────────────────────────────────────────────
_CASE_A1 = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink& progress, const MessageSink& message) {
        result_label_->setText(QStringLiteral("x"));
        progress(100);
    });
}
"""
_CASE_A2 = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink&, const MessageSink&) { build(); });
}
void Page::build() {
    const QString v = combo_->currentText();
    status_label_->setText(v);
}
"""
_CASE_A_GOOD = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink& progress, const MessageSink& message) {
        const Outcome outcome = run_engine();
        message(outcome.text);
        publish(outcome);
        progress(100);
    });
}
void Page::apply() { status_label_->setText(QStringLiteral("done")); }
"""
_CASE_A_QUEUED = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this, box](const ProgressSink& progress, const MessageSink&) {
        box->outcome = run_engine();
        QMetaObject::invokeMethod(this, [this, box] {
            status_label_->setText(QStringLiteral("x"));
            set_busy(false);
        }, Qt::QueuedConnection);
        progress(100);
    });
}
void Page::set_busy(bool busy) { start_button_->setEnabled(!busy); }
"""
_CASE_A_QUEUED_BAD = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink& progress, const MessageSink&) {
        QMetaObject::invokeMethod(this, [this] { status_label_->setText(QStringLiteral("ok")); },
                                  Qt::QueuedConnection);
        status_label_->setText(QStringLiteral("worker 直接改的"));
        progress(100);
    });
}
"""
_CASE_A_GUARDED = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink&, const MessageSink&) { build(); });
}
void Page::build() {
    if (!ui_thread_guard("Page::build")) return;
    status_label_->setText(QStringLiteral("x"));
}
"""
_CASE_A_ON_MAIN = """
void Page::go() {
    task_->run(QStringLiteral("t"), [this](const ProgressSink&, const MessageSink&) { compare_dirs(); });
}
int Page::compare_dirs() {
    results_.clear();
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    if (on_main) {
        refresh_rows();
        refresh_stats_label();
    } else {
        QMetaObject::invokeMethod(this, [this] { refresh_rows(); refresh_stats_label(); },
                                  Qt::QueuedConnection);
    }
    return static_cast<int>(results_.size());
}
"""
_CASE_B1 = """
def register(api):
    def build():
        status = api.ui.label(None, "x")
        panel = api.ui.task_panel(None, True)
        def worker(progress, message):
            api.ui.set_text(status, "x")
            progress(100)
        api.ui.task_run(panel, "t", worker)
"""
_CASE_B2 = """
def register(api):
    def build():
        panel = api.ui.task_panel(None, True)
        api.ui.task_run(panel, "t",
                        lambda progress, message: api.ui.log_view_append(None, "x"))
"""
_CASE_B_GOOD = """
def register(api):
    def build():
        panel = api.ui.task_panel(None, True)
        api.ui.task_on_message(panel, lambda text: api.ui.set_text(None, text))
        def worker(progress, message):
            if api.ui.task_cancel_requested(panel):
                return
            progress(100)
        api.ui.task_run(panel, "t", worker)
"""


def audit_cpp_source(source: str) -> list:
    """A1/A2 的单文件版（自检用；与 audit_cpp_pages 走同一条判定路径）。"""
    problems = []
    members = cpp_member_functions(source)
    for line, worker in worker_bodies_cpp(source):
        hit = touches_widgets(worker)
        if hit:
            problems.append(hit)
            continue
        for callee in sorted(callees_in(worker)):
            if callee in members and touches_widgets(members[callee][1]):
                if not guard_before_widgets(members[callee][1]):
                    problems.append(callee)
    return problems


def audit_py_source(source: str, safe_methods: set) -> list:
    """B1/B2 的单文件版（自检用）。"""
    problems = []
    for worker in worker_functions(ast.parse(source)):
        calls = []
        collect_ui_calls(worker, calls)
        for line, method in calls:
            if method not in safe_methods:
                problems.append("%d:%s" % (line, method))
    return problems


def selfcheck():
    results = []
    ok = True
    cases = [
        ("A1 worker 体直接碰控件", _CASE_A1, True),
        ("A2 worker 调碰控件的成员函数", _CASE_A2, True),
        ("A 干净样本（只调引擎 + sink）", _CASE_A_GOOD, False),
        ("A 带 UI 线程守卫的碰控件函数", _CASE_A_GUARDED, False),
        ("A 排队回主线程（合法通道）", _CASE_A_QUEUED, False),
        ("A 排队之外仍直接碰控件 ⇒ 红", _CASE_A_QUEUED_BAD, True),
        ("A 自己判线程 + 排队（on_main）", _CASE_A_ON_MAIN, False),
    ]
    for label, source, want in cases:
        problems = audit_cpp_source(source)
        good = bool(problems) == want
        ok &= good
        results.append((label, want, bool(problems), good))

    safe = {"task_cancel_requested"}
    py_cases = [
        ("B1 worker 里 set_text", _CASE_B1, True),
        ("B2 lambda worker 里 log_view_append", _CASE_B2, True),
        ("B 干净样本（白名单 + task_on_message）", _CASE_B_GOOD, False),
    ]
    for label, source, want in py_cases:
        problems = audit_py_source(source, safe)
        good = bool(problems) == want
        ok &= good
        results.append((label, want, bool(problems), good))
    return ok, results


def main() -> int:
    print("== 线程与取消契约（静态判据；P17-1 / 审计 §一·1） ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good in results:
        print("   %s %-34s 期望%s红 实得%s"
              % ("✓" if good else "✗", label, "有" if want else "无", "有" if got else "无"))
    if not self_ok:
        print()
        print("THREAD-CONTRACT fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    safe_methods = worker_safe_methods()
    if safe_methods is None:
        print("THREAD-CONTRACT fail 读不到 C++ 侧白名单（%s 的 kWorkerSafeMethods[]）—— "
              "口径缺失按失败处理（fail-closed）" % FACADE_IMPL.relative_to(ROOT))
        return 1
    header = FACADE_HEADER.read_text(encoding="utf-8", errors="replace")
    dangling = [name for name in sorted(safe_methods) if name not in header]
    if dangling:
        print("THREAD-CONTRACT fail 白名单里的 %s 在门面头文件里**不存在**（悬空名）"
              % "、".join(dangling))
        return 1

    pages_dir = ROOT / "src" / "shell" / "pages"
    plugin_dir = ROOT / "plugins"
    if not pages_dir.is_dir() or not plugin_dir.is_dir():
        print("THREAD-CONTRACT fail 仓库自带目录缺失（%s / %s）"
              % (pages_dir.relative_to(ROOT), plugin_dir.relative_to(ROOT)))
        return 1

    print("-- 实测 --")
    print("   跨线程只读白名单（读自 %s）：%s"
          % (FACADE_IMPL.relative_to(ROOT), "、".join(sorted(safe_methods))))
    cpp_problems, cpp_stats = audit_cpp_pages(pages_dir)
    py_problems, py_stats = audit_plugins(plugin_dir, safe_methods)
    print("   C++ 页面：%d 个文件含任务壳 worker（共 %d 个 worker）"
          % (cpp_stats["files"], cpp_stats["workers"]))
    print("   .py 插件：%d 个文件、%d 个 worker、%d 处 ui.* 调用"
          % (py_stats["files"], py_stats["workers"], py_stats["ui_calls"]))
    problems = cpp_problems + py_problems
    print()
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("THREAD-CONTRACT fail %d 项不成立（worker 不许碰 UI / 取消要能读到，两条契约）"
              % len(problems))
        return 1
    print("THREAD-CONTRACT ok worker 只碰引擎与两个 sink；取消位读得到（白名单 %d 项）"
          % len(safe_methods))
    return 0


if __name__ == "__main__":
    sys.exit(main())
