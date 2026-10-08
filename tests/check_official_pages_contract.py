# -*- coding: utf-8 -*-
"""官方六页「是不是插件」的静态判据（P16-0b）。

为什么要有它（另一个会话 2026-10-09 的只读核对结论，我逐条复核过）
------------------------------------------------------------------
· `tests/` 全目录搜 `replaces` = **0 命中**：六枚插件**确实都传了** replaces
  （`plugins/fs_page.py:648` 等六处），但**没有任何判据**锁这件事 —— 谁把 replaces 去掉、
  或让两枚插件指向同一个内置页，门禁一声不响。
· 插件模块**有意不进 `sys.modules`**（`py_host.cpp:3227-3230`）⇒ 插件间不许靠 import 互串。
· 内置六页仍在 `src/shell/pages/` 里直连引擎头（`.h` 14 处 + `.cpp` 8 处 = **22 处**），
  目标是"六页只用 API 表"，但**今天做不到** ⇒ 做成**棘轮**：逐文件计数**只许减不许增**。

判据
----
① 六枚插件必须**逐枚**显式 `replaces` 指向对应内置页 id，且六个内置页**被覆盖齐**
   （不许缺、不许两枚指向同一个、不许指向不存在的内置页）；
② 插件页 id 与它要覆盖的内置页 id **不许相同**（覆盖要新 id；`hub.cpp:620-628` 运行时会拒，
   这里做静态锁，免得跑起来才发现）；
③ `plugins/*.py` 不许 `import` 以 `xrkit` 开头的模块、也不许 import 别的插件文件名；
④ **棘轮**：`src/shell/pages/*.{h,cpp}` 里 `#include "xrkit/engine/...` 的**逐文件计数**
   必须等于基线表；**增一处即红**（六页对等的方向）；**减了要同步下调基线**（升级锁，不放宽）。

输出：`OFFICIAL-PAGES ok …` / `OFFICIAL-PAGES fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证（`selfcheck()`）：四条判据各自喂一个反例必须报红，一份干净样本必须报绿。
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

# ④ 棘轮基线：内置六页直连引擎头的**逐文件计数**（2026-10-09 实测，合计 22）
ENGINE_INCLUDE_BASELINE = {
    "page_base.h": 1,
    "fs_page.h": 3,
    "convert_page.h": 3,
    "text_page.h": 1,
    "xml_page.h": 2,
    "video_page.h": 2,
    "fontpack_page.h": 2,
    "fs_page.cpp": 1,
    "convert_page.cpp": 2,
    "xml_page.cpp": 3,
    "fontpack_page.cpp": 2,
}

_INCLUDE_RE = re.compile(r'#\s*include\s+"(xrkit/engine/[^"]*)"')
_BUILTIN_ID_RE = re.compile(r'\{\s*"([a-z]+)"\s*,')
_REGISTER_TOOL_RE = re.compile(r'register_tool\(\s*"([^"]+)"\s*,\s*[^,]+,\s*\d+\s*,\s*"([^"]*)"')
_IMPORT_RE = re.compile(r"^\s*(?:import|from)\s+([\w\.]+)")


def builtin_ids(factory_source: str) -> list[str]:
    """`page_factory.cpp` 的 `kBuiltinPages[]` 里的内置页 id（按声明顺序）。"""
    start = factory_source.find("kBuiltinPages[]")
    if start < 0:
        return []
    seg = factory_source[start:]
    end = seg.find("};")
    if end >= 0:
        seg = seg[:end]
    return _BUILTIN_ID_RE.findall(seg)


def plugin_registrations(plugin_source: str):
    """返回 [(插件页 id, replaces)]。"""
    return _REGISTER_TOOL_RE.findall(plugin_source)


def audit_pages(plugin_sources: dict, builtins: list[str]):
    """① ② ③ 的核对。plugin_sources: {文件名: 源码}。返回问题清单。"""
    problems = []
    if not builtins:
        problems.append("没从 `page_factory.cpp` 解析到任何内置页 id（解析器与源码脱节）")
        return problems
    if not plugin_sources:
        problems.append("`plugins/` 下没有 .py（集合为空不算通过）")
        return problems

    covered: dict[str, list[str]] = {}
    for name, source in sorted(plugin_sources.items()):
        regs = plugin_registrations(source)
        if not regs:
            problems.append("%s：没有 `api.register_tool(...)`（官方页必须是插件）" % name)
        for page_id, replaces in regs:
            if not replaces:
                problems.append("%s：`register_tool(\"%s\", …)` **没给 replaces** —— "
                                "官方页覆盖内置页必须有显式声明（禁止静默顶掉）"
                                % (name, page_id))
                continue
            if replaces not in builtins:
                problems.append("%s：replaces=\"%s\" 不是内置页 id（合法：%s）"
                                % (name, replaces, ", ".join(builtins)))
                continue
            if page_id == replaces:
                problems.append("%s：插件页 id 与内置页 id 都是 \"%s\" —— 覆盖要新 id"
                                % (name, page_id))
            covered.setdefault(replaces, []).append(name)

    for builtin in builtins:
        who = covered.get(builtin, [])
        if not who:
            problems.append("内置页 \"%s\" **没有任何插件页覆盖它**（六页对等尚未成立）" % builtin)
        elif len(who) > 1:
            problems.append("内置页 \"%s\" 被多个插件覆盖：%s（一个内置页只许一个覆盖者）"
                            % (builtin, ", ".join(who)))

    # ③ 插件间不许互串 / 不许直接 import 引擎绑定模块
    stems = {p[:-3] for p in plugin_sources}
    for name, source in sorted(plugin_sources.items()):
        for line_no, line in enumerate(source.splitlines(), start=1):
            match = _IMPORT_RE.match(line)
            if not match:
                continue
            module = match.group(1).split(".")[0]
            if module.startswith("xrkit"):
                problems.append("%s:%d：不许 import 引擎绑定模块（`%s`）—— 一律经 api 面"
                                % (name, line_no, module))
            elif module in stems and module + ".py" != name:
                problems.append("%s:%d：不许 import 别的插件（`%s`）—— 插件模块不进 sys.modules，"
                                "插件间一律经宿主" % (name, line_no, module))
    return problems


def audit_ratchet(pages_dir: Path):
    """④ 棘轮。返回 (问题清单, 实测计数)。"""
    problems = []
    counts = {}
    for path in sorted(list(pages_dir.glob("*.h")) + list(pages_dir.glob("*.cpp"))):
        hits = _INCLUDE_RE.findall(path.read_text(encoding="utf-8"))
        counts[path.name] = len(hits)
    for name, want in sorted(ENGINE_INCLUDE_BASELINE.items()):
        got = counts.get(name, 0)
        if got > want:
            problems.append("%s：直连引擎头 %d 处 > 基线 %d 处 —— **六页对等方向只许减不许增**"
                            % (name, got, want))
        elif got < want:
            problems.append("%s：直连引擎头 %d 处 < 基线 %d 处 —— 干得好，但**必须同步下调基线**"
                            "（改锁要升级，不许留在旧数上）" % (name, got, want))
    for name, got in sorted(counts.items()):
        if name not in ENGINE_INCLUDE_BASELINE and got > 0:
            problems.append("%s：新增文件直连引擎头 %d 处（基线表里没有它）" % (name, got))
    return problems, counts


# ── 反例自证 ────────────────────────────────────────────────────────────
_SYN_FACTORY = 'constexpr PageInfo kBuiltinPages[] = {\n    {"fs", "FS", "", 0},\n    {"xml", "XML", "", 1},\n};'


def selfcheck():
    cases = []
    builtins = builtin_ids(_SYN_FACTORY)
    ok_src = {
        "fs_page.py": 'api.register_tool("fs_py", build, 0, "fs", "sub", "title")',
        "xml_page.py": 'api.register_tool("xml_py", build, 0, "xml", "sub", "title")',
    }
    cases.append(("好形状（两枚覆盖齐、无 import）", ok_src, 0))

    no_repl = dict(ok_src)
    no_repl["fs_page.py"] = 'api.register_tool("fs_py", build, 0, "", "sub", "title")'
    cases.append(("缺 replaces", no_repl, 1))

    dup = dict(ok_src)
    dup["xml_page.py"] = 'api.register_tool("xml_py", build, 0, "fs", "sub", "title")'
    cases.append(("两枚指向同一内置页 + 漏一个", dup, 2))

    same_id = dict(ok_src)
    same_id["fs_page.py"] = 'api.register_tool("fs", build, 0, "fs", "sub", "title")'
    cases.append(("插件页 id 与内置 id 相同", same_id, 1))

    bad_import = dict(ok_src)
    bad_import["fs_page.py"] = ('import xrkit\n'
                                'api.register_tool("fs_py", build, 0, "fs", "sub", "title")')
    cases.append(("import 引擎绑定模块", bad_import, 1))

    cross_import = dict(ok_src)
    cross_import["fs_page.py"] = ('import xml_page\n'
                                  'api.register_tool("fs_py", build, 0, "fs", "sub", "title")')
    cases.append(("插件间互 import", cross_import, 1))

    results, ok = [], True
    for label, sources, want_min in cases:
        problems = audit_pages(sources, builtins)
        good = len(problems) >= want_min if want_min else not problems
        ok &= good
        results.append((label, want_min, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== 官方六页「是不是插件」静态判据 ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_min, got, good, problems in results:
        print("   %s %-30s 期望≥%d 实得=%d" % ("✓" if good else "✗", label, want_min, got))
        if not good:
            for p in problems[:3]:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("OFFICIAL-PAGES fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    factory = ROOT / "src" / "shell" / "pages" / "page_factory.cpp"
    if not factory.exists():
        print("OFFICIAL-PAGES fail 仓库自带文件缺失：%s" % factory)
        return 1
    builtins = builtin_ids(factory.read_text(encoding="utf-8"))

    plugin_dir = ROOT / "plugins"
    plugin_sources = {p.name: p.read_text(encoding="utf-8")
                      for p in sorted(plugin_dir.glob("*.py"))}
    problems = audit_pages(plugin_sources, builtins)
    ratchet_problems, counts = audit_ratchet(ROOT / "src" / "shell" / "pages")
    problems += ratchet_problems

    print("-- 实测 --")
    print("   内置页 id（%d 个）：%s" % (len(builtins), ", ".join(builtins)))
    print("   插件页（%d 枚）：%s" % (len(plugin_sources), ", ".join(sorted(plugin_sources))))
    for name, got in sorted(counts.items()):
        want = ENGINE_INCLUDE_BASELINE.get(name, 0)
        mark = "=" if got == want else ("↑" if got > want else "↓")
        print("   棘轮 %-22s %d %s 基线 %d" % (name, got, mark, want))
    print("   棘轮合计 %d（基线 %d）" % (sum(counts.values()), sum(ENGINE_INCLUDE_BASELINE.values())))
    print()
    if problems:
        for p in problems:
            print("   ✗ %s" % p)
        print()
        print("OFFICIAL-PAGES fail %d 项不成立" % len(problems))
        return 1
    print("OFFICIAL-PAGES ok 六枚插件逐枚显式 replaces 覆盖六个内置页；棘轮 22 处未增")
    return 0


if __name__ == "__main__":
    sys.exit(main())
