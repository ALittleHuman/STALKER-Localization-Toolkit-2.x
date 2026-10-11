# -*- coding: utf-8 -*-
"""官方六页「是不是插件」的静态判据（P16-0b）。

为什么要有它（另一个会话 2026-10-09 的只读核对结论，我逐条复核过）
------------------------------------------------------------------
· `tests/` 全目录搜 `replaces` = **0 命中**：六枚插件**确实都传了** replaces
  （`plugins/fs_page.py:648` 等六处），但**没有任何判据**锁这件事 —— 谁把 replaces 去掉、
  或让两枚插件指向同一个内置页，门禁一声不响。
· 插件模块**有意不进 `sys.modules`**（`py_host.cpp:3227-3230`）⇒ 插件间不许靠 import 互串。
· 内置六页仍在 `src/shell/pages/` 里直连引擎头（`.h` 14 处 + `.cpp` 8 处 = **22 处**），
  当初的目标是"六页只用 API 表"，做不到 ⇒ 那时做成**棘轮**：只许减不许增。

判据
----
① 六枚插件必须**逐枚**显式 `replaces` 指向对应内置页 id，且六个内置页**被覆盖齐**
   （不许缺、不许两枚指向同一个、不许指向不存在的内置页）；
② 插件页 id 与它要覆盖的内置页 id **不许相同**（覆盖要新 id；`hub.cpp:620-628` 运行时会拒，
   这里做静态锁，免得跑起来才发现）；
③ `plugins/*.py` 不许 `import` 以 `xrkit` 开头的模块、也不许 import 别的插件文件名；
④ **「内置页直连引擎头」的那张计数表**：**只观测、不判红**（见下）。

★★ P17-81：④ 的**语义重写**（用户 2026-10-10 票：选 2 ＝ 转信息性计数）
--------------------------------------------------------------------
★ 为什么改：用户拍板 **甲** ——「**产品实现 = C++ 内置六页**；`plugins/*_page.py` 降级为
**夹具**（非产品）」。甲之后内置页**就是产品**，产品直连自家引擎头**天经地义**；
插件侧那条"夹具不许直连"另有反向锁（P17-56：`plugins/**/*.py` 禁 `PageServices`/`xrkit/engine`）。
⇒ 原棘轮服务的目标（"官方页最终迁移成插件"）**不存在了**；留着"只许减不许增"
等于**把正确当违规压**（反向执法）。
★ 现语义：**"产品与引擎内部耦合面有多大"的观测值**，供将来重估甲/乙、或做页面重构时用。
  打印形态照 P17-79 §九·2 的三条约束：**打增量（N/基线 M/Δ）**、**不设阈值**、
  语义写进文档并**指回甲这条决策**（`STALKER_2x_插件改装_交接/P17-79复核与丙实施契约_20261010.md`
  §九）。一旦加阈值，它又变回判据、然后烂成格言 —— 这正是要躲的。
★ 仍然**判红**的只剩一种形状：**基线表记了、源码里却完全找不到那个文件** ——
  那不是"耦合面变小"，是"这张表与源码脱节"（判据自己烂掉）。文件还在就永远不判红。

输出：`OFFICIAL-PAGES ok …` / `OFFICIAL-PAGES fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证（`selfcheck()`）：①②③ 三条判据各自喂一个反例必须报红，一份干净样本必须报绿。
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

# ④ **观测基线**（不再是棘轮）：内置六页直连引擎头的逐文件计数（2026-10-09 实测，合计 22）。
#   ★★ P17-81：语义已随「甲」作废 —— 现在只用来**打印增量**（N/基线 M/Δ），**不判红**。
#   保留这张表的价值：它是"产品与引擎内部耦合面"的一把现成的尺（重估甲/乙或做页面重构时有用）。
#   唯一仍然判红的形状：表里记了、源码里却**找不到那个文件**（表与源码脱节 ⇒ 判据自己烂掉）。
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
    # ★ 「官方页」＝文件名就是 `<内置页>_page.py` 的那六枚 —— 只有它们**必须**覆盖内置页。
    #   新增页（例如 P16-1c 的 `patch_page.py`）不属于"六页对等"，不要求 replaces；
    #   但它若要 replaces，仍必须指向真实内置页（下面的检查一视同仁）。
    official = {b + "_page.py": b for b in builtins}
    for name, source in sorted(plugin_sources.items()):
        regs = plugin_registrations(source)
        if not regs:
            problems.append("%s：没有 `api.register_tool(...)`（插件页都要注册；新增页也一样）"
                            % name)
            continue
        want = official.get(name)
        for page_id, replaces in regs:
            if not replaces:
                if want is not None:
                    problems.append("%s：官方页 `register_tool(\"%s\", …)` **没给 replaces** —— "
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
    """④ **观测（不判红）**。返回 (问题清单, 实测计数)。

    ★★ P17-81：这里**不再**对"增/减"判红（甲之后内置页＝产品，直连自家引擎头合法；
    棘轮的目标"官方页迁移成插件"不存在了 —— 详见文件头那段）。
    唯一判红的形状：基线表里的文件在源码里**找不到**（表脱节）。文件还在 ⇒ 只有观测。
    计数取**最后一个写它的文件**（`.h` 与 `.cpp` 若同名会是同一个 key，基线表按 key 记账）。
    """
    problems = []
    counts = {}
    for path in sorted(list(pages_dir.glob("*.h")) + list(pages_dir.glob("*.cpp"))):
        hits = _INCLUDE_RE.findall(path.read_text(encoding="utf-8"))
        counts[path.name] = len(hits)
    for name, want in sorted(ENGINE_INCLUDE_BASELINE.items()):
        if not (pages_dir / name).is_file():
            problems.append("%s：观测基线表里记着它，但源码目录里**没有这个文件** —— "
                            "表与源码脱节（不是耦合面变小）" % name)
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
    # ★★ P17-81：④ 转**信息性计数** —— 打印逐文件 N 与基线 M、以及 Δ，**不判红、无阈值**。
    baseline_total = sum(ENGINE_INCLUDE_BASELINE.values())
    info_lines = []
    for name, got in sorted(counts.items()):
        want = ENGINE_INCLUDE_BASELINE.get(name, 0)
        info_lines.append("   观测 %-22s %d（基线 %d，Δ=%+d）" % (name, got, want, got - want))
    for line in info_lines:
        print(line)
    live_total = sum(counts.get(name, 0) for name in ENGINE_INCLUDE_BASELINE)
    print("   INFO 直连引擎头 %d 处（基线 %d，Δ=%+d）—— **信息项：不判红、无阈值**；"
          "棘轮语义随「甲（产品＝内置六页）」作废，现语义＝产品与引擎内部耦合面的大小"
          % (live_total, baseline_total, live_total - baseline_total))
    print()
    if problems:
        for p in problems:
            print("   ✗ %s" % p)
        print()
        print("OFFICIAL-PAGES fail %d 项不成立" % len(problems))
        return 1
    print("OFFICIAL-PAGES ok 六枚插件逐枚显式 replaces 覆盖六个内置页；"
          "引擎直连 %d 处（基线 %d，Δ=%+d）为**信息项**（棘轮随甲作废）"
          % (live_total, baseline_total, live_total - baseline_total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
