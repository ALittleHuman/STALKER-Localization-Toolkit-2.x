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

from _cpp_source import strip_comments  # noqa: E402

FRONTS = {
    "src/shell/hub.cpp": "Unity",
    "tools/cli/main.cpp": "CLI",
}
UNIFIED = "load_all_plugins_from_dir"
DLL_ONLY = "load_plugins_from_dir"
PY_ONLY = "load_python_plugins_from_dir"
UNIFIED_OWNER = "src/engine/plugin/loader.cpp"

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
    ("tools/cli/main.cpp", "load_python_plugins_from_dir"):
        (1, "--py-plugins 探针：要 PyLoadSummary 计数，专门验 **Python 那一路**"),
}


def scan(sources: dict[str, str]) -> list[str]:
    offenders: list[str] = []
    allow_hits: dict[tuple[str, str], int] = {}
    for rel, source in sources.items():
        text = strip_comments(source)
        if rel in FRONTS and not _count(text, UNIFIED):
            offenders.append("%s：没有调用统一入口 %s(...)（两个前端必须走同一条路）"
                             % (rel, UNIFIED))
        # 逐处报：哪些调用点越过了统一入口（`load_plugins_from_dir` = 只加载 DLL 那条老路）
        if rel != UNIFIED_OWNER:
            for name in (DLL_ONLY, PY_ONLY):
                lines = _count(text, name)
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
        if rel == UNIFIED_OWNER and not _count(text, UNIFIED):
            offenders.append("%s 里没有实现统一入口 %s(...)" % (UNIFIED_OWNER, UNIFIED))

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
# CLI 里那两条**诊断探针**（白名单登记的就是它们）—— 干净样本必须包含它们，
# 否则"白名单零命中即红"那条规则会把干净样本误报。
_PROBES = ('void probe_dll() { load_plugins_from_dir(dir, registry, nullptr); }\n'
           'void probe_py() { load_python_plugins_from_dir(dir, registry); }\n')
_GREEN_CLI = _GREEN_FRONT + _PROBES
_GREEN_OWNER = (
    'std::vector<RegistrationError> load_all_plugins_from_dir(...) {\n'
    '    auto a = load_plugins_from_dir(dir, registry, ui);\n'
    '    auto b = load_python_plugins_from_dir(dir, registry);\n'
    '    return a;\n}\n'
)
_GREEN_SOURCES = {
    "src/shell/hub.cpp": _GREEN_FRONT,
    "tools/cli/main.cpp": _GREEN_CLI,
    UNIFIED_OWNER: _GREEN_OWNER,
}
_RED = {
    "前端绕过统一入口": dict(_GREEN_SOURCES, **{"src/shell/hub.cpp":
        'void f() { load_plugins_from_dir(dir, registry_, ui); }\n'}),
    "前端根本没加载插件": dict(_GREEN_SOURCES, **{"tools/cli/main.cpp": _PROBES}),
    "统一入口缺失": {"src/shell/hub.cpp": _GREEN_FRONT, "tools/cli/main.cpp": _GREEN_CLI},
    "别处也调 Python 加载": dict(_GREEN_SOURCES, **{"tools/cli/main.cpp":
        _GREEN_CLI + 'void g() { load_python_plugins_from_dir(dir, registry); }\n'}),
}


def selfcheck() -> list[str]:
    broken: list[str] = []
    if scan(_GREEN_SOURCES):
        broken.append("干净样本被误报：%s" % scan(_GREEN_SOURCES))
    for name, sources in _RED.items():
        if not scan(sources):
            broken.append("反例「%s」没被判红" % name)
    return broken


def main() -> int:
    broken = selfcheck()
    if broken:
        print("PLUGINENTRY fail 判据自证不过（%d 处）：" % len(broken))
        for item in broken:
            print("           " + item)
        return 1
    sources: dict[str, str] = {}
    for rel in list(FRONTS) + [UNIFIED_OWNER]:
        path = ROOT / rel
        if not path.is_file():
            print("PLUGINENTRY fail 找不到 %s（判据没跑成，不是通过）" % rel)
            return 1
        sources[rel] = path.read_text(encoding="utf-8")
    offenders = scan(sources)
    if offenders:
        print("PLUGINENTRY fail %d 处：插件加载入口分叉了" % len(offenders))
        for item in offenders[:10]:
            print("           " + item)
        return 1
    print("PLUGINENTRY ok 两个前端都走 %s（判据自证 %d 红 + 1 绿；Python 加载只由统一入口调）"
          % (UNIFIED, len(_RED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
