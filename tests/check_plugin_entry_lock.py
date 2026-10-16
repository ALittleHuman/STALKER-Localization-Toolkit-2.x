#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""插件改装的**加载入口唯一性**静态锁（P2b）。

口径（用户红线「GUI 的功能，CLI 肯定要全部拥有」+ 规格"只许一份实现"）：
  ① 两个前端（Hub / `tools/cli/main.cpp`）都必须走**同一个**统一入口
     `load_all_plugins_from_dir(...)`（它把 `*.dll` 与 `*.py` 两条路收在一起）；
  ② 谁也不许**绕过**它直接调 `load_plugins_from_dir(...)`（那只加载 DLL ⇒ Python 插件
     会"GUI 有、CLI 没有"，或者反过来）；
  ③ `load_python_plugins_from_dir` 只许被**统一入口自己**（`src/engine/plugin/loader.cpp`）调用
     —— 别处再调一次就是"第二条加载路径"。

为什么必须静态锁：这类分叉**测不出来**（两边各自跑各自的用例都绿），只有"调用点在哪"能判。

判据按**每一处**报 `文件:行号`（不写"文件里出现过"），去注释后判、保留行号。
带自证：先喂反例证明会红，再喂干净样本证明不误报。

输出：`PLUGINENTRY ok` / `PLUGINENTRY fail <详情>`；退出码 0/1。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from _cpp_source import function_body, strip_comments  # noqa: E402

FRONTS = {
    "src/shell/hub.cpp": "Unity",
    "tools/cli/main.cpp": "CLI",
}
UNIFIED = "load_all_plugins_from_dir"
DLL_ONLY = "load_plugins_from_dir"
PY_ONLY = "load_python_plugins_from_dir"
UNIFIED_OWNER = "src/engine/plugin/loader.cpp"
# 宿主探针的**唯一实现**也在这个集合里（P2c：探针从 CLI 搬进了引擎 —— 因为发行包里只有
# `xrkit_app.exe`，包内判据必须由 GUI exe 也能跑）。它**本来就该**直接调分路加载器（要计数），
# 所以它在白名单里、而**前端**不在白名单里（前端只能走统一入口这一条路）。
PY_OWNER = "src/engine/plugin/py_host.cpp"
SCANNED = list(FRONTS) + [UNIFIED_OWNER, PY_OWNER]

# `load_plugins_from_dir` 是 `load_all_plugins_from_dir` 的**真前缀** ⇒ 匹配时必须要求
# 左边不是 `_`（否则把统一入口也算成"绕过"）。
def _count(text: str, name: str) -> list[int]:
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    return [text.count("\n", 0, m.start()) + 1 for m in pattern.finditer(text)]


# 诊断探针的白名单：**逐条带理由**，而且**命中次数必须精确对上**（口径同 `check_px_only.py`
# 的 KNOWN_BARE）：多一处调用 ⇒ 数字对不上 ⇒ 红。这样"白名单"不会变成"整文件豁免"。
ALLOW = {
    ("tools/cli/main.cpp", "load_plugins_from_dir"):
        (1, "--plugin-init-rollback 探针：专门验 **DLL 那一路**的 init 失败回滚，故意不经过统一入口"),
    (PY_OWNER, "load_python_plugins_from_dir"):
        (1, "宿主探针的**唯一实现**（run_python_host_probe）：要 PyLoadSummary 计数，专门验 Python 那一路"),
}


def _call_lines(text: str, name: str, body_of: str = "") -> list[int]:
    """调用点行号。`body_of` 非空 ⇒ **只在那个函数体里数**（本仓纪律：按函数体计数，
    否则"函数定义那一行"自己也会命中，白名单就得靠数字凑 —— 实测第一版就是这么红的）。"""
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    if body_of:
        scope = function_body(text, body_of)
        if not scope:
            return []
        offset = text.find(scope)
        return [text.count("\n", 0, offset + m.start()) + 1 for m in pattern.finditer(scope)]
    return [text.count("\n", 0, m.start()) + 1 for m in pattern.finditer(text)]


def scan(sources: dict[str, str]) -> list[str]:
    offenders: list[str] = []
    allow_hits: dict[tuple[str, str], int] = {}
    for rel, source in sources.items():
        text = strip_comments(source)
        if rel in FRONTS and not _call_lines(text, UNIFIED):
            offenders.append("%s：没有调用统一入口 %s(...)（两个前端必须走同一条路）"
                             % (rel, UNIFIED))
        if rel == UNIFIED_OWNER:
            # 统一入口**自己**就该调那两条分路加载器 —— 这不是"绕过"
            if not _call_lines(text, UNIFIED):
                offenders.append("%s 里没有实现统一入口 %s(...)" % (UNIFIED_OWNER, UNIFIED))
            continue
        # 其余被扫文件：任何分路调用都要么在白名单里、要么判红
        for name in (DLL_ONLY, PY_ONLY):
            body = "run_python_host_probe" if (rel == PY_OWNER and name == PY_ONLY) else ""
            lines = _call_lines(text, name, body)
            if not lines:
                continue
            budget = ALLOW.get((rel, name))
            if budget is not None and len(lines) == budget[0]:
                allow_hits[(rel, name)] = len(lines)
                continue
            for line in lines:
                offenders.append("%s:%d 调了 %s(...) —— 它只许被统一入口（%s）调用%s"
                                 % (rel, line, name, UNIFIED_OWNER,
                                    "" if budget is None
                                    else "（白名单登记 %d 处，实得 %d 处）"
                                         % (budget[0], len(lines))))

    if UNIFIED_OWNER not in sources:
        offenders.append("缺少统一入口的实现文件 %s（判据没跑成，不是通过）" % UNIFIED_OWNER)
    # 白名单不许当摆设：登记了却没命中 = 这条豁免已经失效（代码改了或本来就匹配不上）⇒ 红
    for key, (want, why) in ALLOW.items():
        got = allow_hits.get(key, 0)
        if got != want:
            offenders.append("白名单条目 %s:%s 期望命中 %d 处、实得 %d 处（%s）"
                             % (key[0], key[1], want, got, why))
    return offenders


_GREEN_FRONT = 'void f() { load_all_plugins_from_dir(dir, registry_, ui); }\n'
# CLI 里那条**DLL 回滚诊断探针**（白名单登记的就是它）—— 干净样本必须包含它，
# 否则"白名单零命中即红"那条规则会把干净样本误报。
_PROBE_CLI = 'void probe_dll() { load_plugins_from_dir(dir, registry, nullptr); }\n'
_GREEN_CLI = _GREEN_FRONT + _PROBE_CLI
_GREEN_OWNER = (
    'std::vector<RegistrationError> load_all_plugins_from_dir(...) {\n'
    '    auto a = load_plugins_from_dir(dir, registry, ui);\n'
    '    auto b = load_python_plugins_from_dir(dir, registry);\n'
    '    return a;\n}\n'
)
# 探针实现在引擎里（P2c 起）：它直接调 Python 分路加载器是**登记过的例外**
_GREEN_PY = 'int run_python_host_probe(...) { load_python_plugins_from_dir(dir, registry); }\n'
_GREEN_SOURCES = {
    "src/shell/hub.cpp": _GREEN_FRONT,
    "tools/cli/main.cpp": _GREEN_CLI,
    UNIFIED_OWNER: _GREEN_OWNER,
    PY_OWNER: _GREEN_PY,
}
_RED = {
    "前端绕过统一入口": dict(_GREEN_SOURCES, **{"src/shell/hub.cpp":
        'void f() { load_plugins_from_dir(dir, registry_, ui); }\n'}),
    "前端根本没加载插件": dict(_GREEN_SOURCES, **{"tools/cli/main.cpp": _PROBE_CLI}),
    "统一入口缺失": {"src/shell/hub.cpp": _GREEN_FRONT, "tools/cli/main.cpp": _GREEN_CLI,
                     PY_OWNER: _GREEN_PY},
    "别处也调 Python 加载": dict(_GREEN_SOURCES, **{"tools/cli/main.cpp":
        _GREEN_CLI + 'void g() { load_python_plugins_from_dir(dir, registry); }\n'}),
    "白名单零命中（登记过的那条探针没了）": dict(_GREEN_SOURCES, **{
        "tools/cli/main.cpp": _GREEN_FRONT + 'void g() { load_python_plugins_from_dir(dir, registry); }\n'}),
}


def owner_boundary_offenders(sources: dict) -> list:
    """★★ P17-126（复审计「`owner/kind` 边界」的**运行期半**）：
    **插件侧不得有口子自己填 owner**。

    为什么是这个形状（先查件结论）：用户要的是「注册入口对跨用**当场拒绝**」，
    而实测：`py_host.cpp` 的 `PyPluginApi` 内部一律用 `owner_`，
    插件侧绑定签名里**没有** `plugin`/`owner` 参数
    ⇒ 插件**选不了**自己的 owner ⇒ 「跨用」在当前表面上**无处可拒**。
    ⇒ 本判据把「**没口子**」这件事**锁住**：谁加了 owner 参数，谁就红（而不是去造一个永远为真的拒绝）。
    """
    offenders = []
    src = sources.get("src/engine/plugin/py_host.cpp", "")
    if not src:
        return ["拿不到 py_host.cpp（判据没跑成，不是通过）"]
    # ① 扩号内找 `.def("register_xxx", &PyPluginApi::register_xxx` 的整段，
    #   看它有没有 `py::arg("plugin")` / `py::arg("owner")` 这种**让插件填归属**的口子。
    for ln, line in enumerate(src.splitlines(), 1):
        if ".def(\"register_" not in line:
            continue
        # 取该 `.def(...)` 开始的若干行（绑定写法会换行）
        block = "\n".join(src.splitlines()[ln - 1:ln + 6])
        for bad in ('py::arg("plugin")', 'py::arg("owner")'):
            if bad in block:
                offenders.append("%d 行附近：`%s` 出现在插件侧绑定里 —— "
                                 "插件不得自己填归属" % (ln, bad))
    # ② 正面锁：内部必须真的用 `owner_` 填（不是口头约定）
    if src.count("owner_") < 4:
        offenders.append("插件侧归属只能由宜主 `owner_` 填 —— "
                         "但 `owner_` 出现次数不足（%d）" % src.count("owner_"))
    return offenders


_REAL_SOURCES: dict = {}
# ★★ P17-126：⑤（owner 归属边界）带来的**额外反例数** —— 收尾那句自证计数
#   原来只数 `len(_RED)`（写法上就漏了后加的判据）⇒ 这里把它加上，
#   免得又出现「新判据没计进自证」这种微缩版的“记录了、没人判”。
OWNER_CASES = 1


def selfcheck() -> list[str]:
    broken: list[str] = []
    if scan(_GREEN_SOURCES):
        broken.append("干净样本被误报：%s" % scan(_GREEN_SOURCES))
    for name, sources in _RED.items():
        if not scan(sources):
            broken.append("反例「%s」没被判红" % name)
    # ★★ P17-126：⑤（owner 归属边界）也要自证：
    #   · 干净样本（现实源码）必须**不报**；
    #   · 喂一个"给插件开了 owner 口子"的反例必须**报红**。
    if owner_boundary_offenders(_REAL_SOURCES):
        broken.append("⑤ 干净样本被误报：%s" % owner_boundary_offenders(_REAL_SOURCES))
    _leak = dict(_REAL_SOURCES)
    _leak["src/engine/plugin/py_host.cpp"] = (
        _REAL_SOURCES["src/engine/plugin/py_host.cpp"]
        + "\n            .def(\"register_command\", &PyPluginApi::register_command,\n"
          "                 py::arg(\"action_id\"), py::arg(\"plugin\"))\n")
    if not owner_boundary_offenders(_leak):
        broken.append("反例「插件侧绑定多了 owner 口子」没被判红")
    return broken


def main() -> int:
    # ★★ P17-126：**先把真源码读进 `_REAL_SOURCES`，再跑自证** ——
    #   因为 ⑤（owner 归属边界）的自证要用真源码当"干净样本"。
    #   ★ 本批自己踩的坑：我最初把读源码留在 `selfcheck()` **之后** ⇒ `_REAL_SOURCES` 是空的
    #     ⇒ ⑤ 的自证在空串上跑，**永远不报**（等于没自证）。
    missing = [rel for rel in SCANNED if not (ROOT / rel).is_file()]
    if missing:
        print("PLUGINENTRY fail 找不到 %s（判据没跑成，不是通过）" % "、".join(missing))
        return 1
    for rel in SCANNED:
        _REAL_SOURCES[rel] = (ROOT / rel).read_text(encoding="utf-8")

    broken = selfcheck()
    if broken:
        print("PLUGINENTRY fail 判据自证不过（%d 处）：" % len(broken))
        for item in broken:
            print("           " + item)
        return 1
    sources: dict[str, str] = {}
    for rel in SCANNED:
        path = ROOT / rel
        if not path.is_file():
            print("PLUGINENTRY fail 找不到 %s（判据没跑成，不是通过）" % rel)
            return 1
        sources[rel] = path.read_text(encoding="utf-8")
    offenders = scan(sources)
    offenders = offenders + owner_boundary_offenders(sources)
    if offenders:
        print("PLUGINENTRY fail %d 处：插件加载入口分叉了" % len(offenders))
        for item in offenders[:10]:
            print("           " + item)
        return 1
    print("PLUGINENTRY ok 两个前端都走 %s（判据自证 %d 红 + 1 绿；Python 加载只由统一入口调）"
          % (UNIFIED, len(_RED) + OWNER_CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
