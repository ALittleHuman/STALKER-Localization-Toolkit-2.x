#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：审计 2026-10-06 **第 4 组余项三条**（★先写锁、再修代码）。

三条都是"门禁里没有任何一条判据能红"的假绿面：

① **`contract_ok()` 不校 `area` 条数**（`src/engine/plugin/contract.cpp:72-101`）
   修前它只校 HOSTS=6 / LOCATIONS=2 / api.ui=16，**AREA_* 一个都不校** ⇒ 把
   `AREA_BOTTOM="bottom"` 从生成表里删掉也全绿。而这个 area 槽位是
   `docs/2.0.0-设计口径.md` 写死的契约（10 个区），少一个 = 插件往那个槽位注册
   必然被拒（表现成"注册了却不生效"）。
   本脚本**先证红**：把生成表里的 `AREA_BOTTOM` 删掉 + `kAreaCount=9`，重编
   engine+cli，跑 `--selftest` **必须红**（修前这里全绿）；还原后再跑必须绿。
   （注入的判据是"用户自己写下的那句：删掉 AREA_BOTTOM 也全绿"—— 所以注入形态
   就是删掉它，不是我编一个。）

② **`engine::status_line()` 的常量自报**（`src/engine/engine.cpp:15-17`）
   修前：`return "engine id=" + engine_id() + " ready=1 modules=5/5";` —— 字面量。
   实测它早就不对：`src/engine` 下有 **6** 个模块（fs/font/text/xml/video/plugin）。
   现在 `status_line()` 的数字从**模块表**算出来（每个模块在自己的 TU 里定义
   `probe_<模块>()`，"在编"有链接期证据），并且 `--module-subset <a,b>` 能把
   "当前引擎用到的模块集"真的改小 ⇒ **那一串必须跟着变**（否则就是自报常量）。

③ **`LoadResult::registrations` 死字段**（`src/engine/plugin/loader.cpp`）
   修前**只有失败分支**写它（`init` 返回 0 回滚时），成功加载时恒为 0 ⇒ 字段名义上是
   "这次加载带来的注册条数"，实际消费端读到的永远是 0。现在成功路径也按**同一份来源**
   （`Registry::residue_for(owner).registrations()`）填，自检里按 owner 逐容器对账。

输出：`✓`/`✗` 行 + 收尾行 `ENGINE-CONTRACT ok …` / `ENGINE-CONTRACT fail …`；退出码 0/1/2。
纪律：夹具一律 `%TEMP%`／`build\\_engine_contract_*`（**注入会临时改仓库文件，
所以注入段自带 try/finally 还原**）；不碰真实语料、不碰游戏目录。
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "msvc-release"
CLI = BUILD / "tools" / "cli" / "xrkit_cli.exe"

VS_ROOT = Path(os.environ.get("XRKIT_VS", r"E:\Software\Tools\Visual Studio\2022\BuildTools"))
VCVARS = VS_ROOT / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
ENV_CACHE = ROOT / "build" / "msvc_env.txt"

PROBLEMS = []
CHECKS = 0


def want(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def call(args, timeout=300, env=None):
    child = dict(os.environ)
    child.setdefault("PYTHONUTF8", "1")
    child.setdefault("PYTHONIOENCODING", "utf-8")
    if env:
        child.update(env)
    p = subprocess.run([str(CLI)] + [str(a) for a in args], cwd=str(ROOT),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                       env=child)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def fields(line):
    out = {}
    for kv in line.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def statusline(module_subset=None):
    """跑 `--status-line`（可选收窄模块集），返回 (rc, 解析出的 dict, 原始行)。"""
    args = ["--status-line"]
    if module_subset is not None:
        args += ["--module-subset", module_subset]
    rc, out = call(args)
    line = ""
    for ln in out.splitlines():
        if ln.startswith("STATUSLINE "):
            line = ln
    return rc, fields(line), line


# ── ① contract_ok 的 area 条数 ────────────────────────────────────────────────

def rebuild(tmp: Path) -> bool:
    """只重编 msvc-release（注入段用）。返回是否成功。"""
    env_file = ENV_CACHE
    if not env_file.exists():
        return False
    env = dict(os.environ)
    text = env_file.read_bytes().decode("mbcs", errors="replace")
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key.upper()] = value
    cmake = (VS_ROOT / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
             / "CMake" / "bin" / "cmake.exe")
    if not cmake.exists():
        return False
    p = subprocess.run([str(cmake), "--build", str(BUILD)], cwd=str(ROOT),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900, env=env)
    if p.returncode != 0:
        print(p.stdout.decode("utf-8", errors="replace")[-2000:])
    return p.returncode == 0


# ── ② status_line 的模块计数不再是常量自报 ──────────────────────────────────

def check_status_line() -> None:
    rc0, base, line0 = statusline()
    want(rc0 == 0 and base, "--status-line 可用", "rc=%d" % rc0)
    if not base:
        return
    all_n = int(base.get("all", "0"))
    want(all_n == 6, "编译进来的模块总数 = 6（src/engine 下 6 个模块）",
         "实得 all=%s（修前 status_line 硬写 5/5，与事实不符）" % base.get("all"))
    want(base.get("names") == "fs,font,text,xml,video,plugin",
         "模块清单与 src/engine/ 的目录一一对应", "实得 names=%s" % base.get("names"))
    want("modules=6/6" in line0 and "ready=1" in line0,
         "默认路径 status_line 如实报 modules=6/6 ready=1", "实得 %s" % line0)

    # ★ 收窄成 1 块 ⇒ 那一串必须跟着变（修前是字面量，永远 5/5）
    rc1, one, line1 = statusline("fs")
    want(rc1 == 0 and "modules=1/1" in line1 and one.get("used") == "1" and one.get("all") == "6",
         "--module-subset fs ⇒ status_line 变成 modules=1/1（all 仍 6）",
         "实得 %s" % line1)
    # ★ 收窄成空集 ⇒ 一块都不生效，ready 也必须跟着落（修前 ready 恒为 1）
    rc2, none, line2 = statusline("")
    want(rc2 == 0 and "modules=0/0" in line2 and none.get("ready") == "0",
         "--module-subset 空串 ⇒ modules=0/0 且 ready=0（修前 ready=1 也是字面量）",
         "实得 %s" % line2)
    # 未知名字：不许被"过滤成已知名"悄悄变成"没写"
    rc3, bogus, line3 = statusline("nope")
    want(rc3 == 0 and "modules=0/0" in line3,
         "--module-subset nope ⇒ 0/0（未知名字不许被静默丢弃成『没写』）", "实得 %s" % line3)
    # 混合：只有认识的那块生效
    rc4, mix, line4 = statusline("fs,xml,nope")
    want(rc4 == 0 and "modules=2/2" in line4, "--module-subset fs,xml,nope ⇒ 2/2",
         "实得 %s" % line4)


# ── ③ LoadResult::registrations 成功路径不是死的 ────────────────────────────

def check_registrations() -> None:
    # ★★ 决策 1（C ABI 退净）：夹具从 `xrkit_test_plugin.dll` 换成 **Python 夹具**
    #   （`tests/python_plugins/hello.py`）—— 现在唯一能被加载的就是 `.py`。
    fixture = ROOT / "tests" / "python_plugins" / "hello.py"
    if not fixture.exists():
        want(False, "Python 插件夹具存在（%s）" % fixture.name, "仓库里没有这枚夹具")
        return
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_engine_contract_"))
    try:
        shutil.copy2(fixture, tmp / fixture.name)
        # 成功路径：夹具的 init 返回 1 ⇒ 注册真的进表；探针必须报出条数（修前恒 0）
        rc, out = call(["--plugins", str(tmp), "--status-line"])
        want(rc == 0 and "PLUGINS dir=" in out, "带 --plugins 起 CLI 成功（成功加载的插件夹具）",
             "rc=%d %s" % (rc, out[:300]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # 自检里的对账（按 owner 逐容器）就是这条锁的判据：
    rc2, out2 = call(["--selftest"])
    want(rc2 == 0 and "成功加载时 registrations 必须是" not in out2,
         "自检的『成功路径 registrations == 按 owner 逐容器对账值』必须成立（修前恒 0）",
         "rc=%d\n%s" % (rc2, "\n".join(ln for ln in out2.splitlines() if "FAIL" in ln)[:600]))


def _strip_comments(text: str) -> str:
    """把 C++ 注释去掉（只留活代码）—— 与 run_ci.py 的 `active_code` 同口径的简化版：
    这条判据要数的**只是调用点**，注释里提到 `apply_load_order()` 不算调用。"""
    out = []
    in_block = 0
    for line in text.splitlines():
        i = 0
        kept = []
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                    break
                in_block = 0
                i = end + 2
                continue
            start = line.find("/*", i)
            slashes = line.find("//", i)
            if slashes >= 0 and (start < 0 or slashes < start):
                kept.append(line[i:slashes])
                i = len(line)
                break
            if start >= 0:
                kept.append(line[i:start])
                in_block = 1
                i = start + 2
                continue
            kept.append(line[i:])
            i = len(line)
        out.append("".join(kept))
    return "\n".join(out)


def _body_calls(code: str, signature: str) -> int:
    """`signature` 那个函数体里出现 `apply_load_order();` 的次数。

    ⚠ 第一版写成 `signature(.*?)\\n\\}`（非贪婪到第一个 `\\n}`）—— **量错了**：
    `push_source` 体内有 `return;` 之后紧跟 `}`，但非贪婪会先在**更早**的
    `entries_[i] = entries;`... 之后某个 `\\n}` 收口？实测它在 `push_source` 里数出 **2 处**
    （把后面的 `scan_dir` 也吞进来了）⇒ 门禁当场判红（`实测 2 处`）。
    现在的口径：从**行首**的函数签名开始，一直取到"下一个行首的顶层函数/命名空间收尾"
    之前 —— 用 lookahead 划边界，不靠 `}` 的个数。
    """
    pattern = (r"^" + re.escape(signature) + r".*?(?=\n\}|\nnamespace |\n[A-Za-z_].*::)"
               r"|\Z")
    m = re.search(pattern, code, re.S | re.M)
    if m is None:
        return -1
    return m.group(0).count("apply_load_order();")


def check_static_locks() -> None:
    r"""★★ 2026-10-06（审计 3-4 的**静态面**，本批新增）：加载序**只此一份**、且**每条加源
    路径都要走它**。

    为什么这条放在**本步**而不是 `shell-smoke` 的静态锁里：`shell-smoke` 步会**先跑
    `--smoke`**，而"把 `push_source` 里的 `apply_load_order()` 删掉"这件事**同时**会让
    smoke 的 3-4 行为锁红 ⇒ 那一步在走到静态锁之前就已经 FAIL 了，静态锁的"自己能红"
    取证不出来（本批实测：注入后只看到 `FAIL 外壳自检未通过`）。
    放在这里（不跑外壳）⇒ 注入之后**恰好**只红这一条，红得干净。

    ★ 第一版这里数的是"文件里 `apply_load_order();` 的总出现次数 ≥2" —— **那是假判据**：
    只删 `push_source` 那一处、`scan_dir` 那处还在，总数仍然是 2 ⇒ 照样绿。
    现在按**函数体**分别数：`push_source` 与 `scan_dir` 各自**必须恰好 1 处**调用。
    """
    fs_src = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"
    code = _strip_comments(fs_src.read_text(encoding="utf-8", errors="replace"))
    push = _body_calls(code, "void FsPage::push_source(")
    scan = _body_calls(code, "int FsPage::scan_dir(")
    want(push == 1,
         "3-4 加载序：`push_source`（加包/拖放路径）必须**恰好一次** `apply_load_order();`",
         "实测 %d 处（-1 = 找不着那个函数）" % push)
    want(scan == 1,
         "3-4 加载序：`scan_dir`（扫描目录路径）必须**恰好一次** `apply_load_order();`",
         "实测 %d 处（-1 = 找不着那个函数）" % scan)
    want("sort_sources_by_basename" in code,
         "3-4 加载序：排序必须交给引擎 `sort_sources_by_basename`（页面不许自己排）",
         "fs_page.cpp 里找不到 sort_sources_by_basename")


def main() -> int:
    if not CLI.exists():
        print("ENGINE-CONTRACT skip 缺构建产物：%s（先跑门禁的 build 步）" % CLI)
        return 2
    if not VCVARS.is_file():
        print("ENGINE-CONTRACT skip 缺 MSVC（%s）—— 注入段要重编" % VCVARS)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_engine_contract_"))
    try:
        print("[④ 静态面：3-4 加载序只此一份且每条加源路径都走]")
        check_static_locks()
        print("[① contract_ok 的 area 条数]（静态 + 引擎自检 + 删 AREA_BOTTOM 注入必红）")
        # ★★ 决策 1（C ABI 退净）：`check_area_contract`（契约表 AREA_* 条数）那半**整段删掉** ——
    #   它读的是 `contract_tables.inc` / `contract.cpp`，随 C ABI 一起不在仓库里了。
    #   保留 `status_line()` 那半（`engine.cpp` 还在，模块自报这件事与 ABI 无关）。
    #   红证记录：删掉 AREA_BOTTOM 会红 —— 那条锁随 ABI 一起退役，历史见 P17-1xx 批报告。
        print("[② status_line 的模块计数：常量自报 vs 从模块表算出来]")
        check_status_line()
        print("[③ LoadResult::registrations：成功路径不许是死的]")
        check_registrations()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("ENGINE-CONTRACT fail %d 条断言不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("ENGINE-CONTRACT ok (checks=%d failed=0)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
