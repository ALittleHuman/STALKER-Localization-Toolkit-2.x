#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：插件 `init` 返回 0 时**必须按 owner 回滚**它这次 init 期间的注册。

为什么要有这个脚本 —— `load_plugin` 调 `stk_plugin_init(&api)`，插件在 init 期间是
"成功一条进一条"地往注册表里写的；官方夹具的写法就是**每条失败就 `return 0`**
（`tools/test_plugin/test_plugin.c`，最后一条是 `register_compare_mode`）。
修前宿主拿到 0 只做 `FreeLibrary(handle)` 就返回 ⇒ 表里留着**指向已 unmap 代码段**的裸函数
指针（命令 handler / 菜单项 / 面板 builder / 解密器 / 比较模式 …）⇒ 用户之后点到它注册的
按钮就是 AV（`Registry::invoke` 只 catch C++ 异常，**接不住** AV）。

夹具（本构建产出，见 `tools/cli/CMakeLists.txt`）＝ `xrkit_test_plugin_badinit.dll`：
与官方夹具**同一份写法**，只有最后一句 `register_compare_mode` 的 compare 传 0（"缺 compare"
⇒ 宿主拒绝 ⇒ 插件 `return 0`）⇒ 走到这里时**前 6 条注册已经在表里**，而 init 返回 0。

判据（改前红 / 改后绿，逐条断言，缺一条都不算过）：

  ① 夹具**真的触发了那条路**：宿主如实报"入口返回 0"（否则这一趟什么都没测到，
     "表里干净"是**空判据**）；
  ② ★主判据：**按 owner 名**（这枚 dll 的文件名）逐容器断言表里**零残留** ——
     `residue_for(owner)` 的 9 个注册容器逐项为 0，且 `registration_count()` 回到
     加载前的 `mark`、`loaded_count()` 没把它记进已加载名单；
     ★ 为什么必须按 owner 逐项、不能只看总数：`registration_count()` 是 9 个容器的**总和**，
     "总数相等"也可能是"撤错了别家的、留了自己的"（总量 ≠ 归属）。
  ③ 留痕**仍在**（零静默）：失败理由必须看得见 —— `errors_` **不在**"零残留"的范围里
     （本批对判据草案②的复核修正：那条把 `registration_errors()` 也列进"逐项为空"，
     与①/零静默冲突，且 errors_ 只存字符串、不可能悬垂）。
  ④【S2-2，B 批】**扩展名大小写不敏感**：同一份夹具改名成 `NLC.DLL` 也必须被认成插件
     （修前静默漏载：探针自己都打 `PLUGINROLLBACK skip reason=目录里没有 *.dll`、rc=2）。
  ⑤ 【补充·**弱判据**，本脚本**不**据此判红】"之后调用它曾注册的入口不崩" ——
     `FreeLibrary` 未必立刻 unmap，"不崩 ≠ 干净"。（红取证里单独做过一次：真调用 ⇒ AV。）

输出：`✓` / `✗` 行 + 收尾行 `PLUGIN-INIT-ROLLBACK ok …` / `PLUGIN-INIT-ROLLBACK fail …`；
退出码 0=全绿、1=有断言不成立、2=构建产物缺失（外部前提，按档位判定）。

★ 夹具一律拷到 `%TEMP%` 的**空目录**里再跑（`load_plugins_from_dir` 吃目录里**所有** dll，
  把构建目录指过去会把好插件也装进来）。不碰真机语料、不碰游戏目录。
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
FIXTURE = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_test_plugin_badinit.dll"

# 9 个注册容器（`PluginResidue` 的逐项名）—— 逐项为 0 才算零残留
REG_KEYS = ("decryptors", "formats", "commands", "menu_items", "option_entries", "options",
            "panels", "tools", "compare_modes")

PROBLEMS = []


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def fields(line):
    """把 `k=v` 串解析成 dict（值里不含空格）。"""
    out = {}
    for kv in line.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def main():
    parser = argparse.ArgumentParser(description="插件 init 返回 0 的回滚判红锁")
    parser.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists() or not FIXTURE.exists():
        missing = [str(p) for p in (CLI, FIXTURE) if not p.exists()]
        print("!! 缺构建产物：%s（先跑 python tests/run_ci.py --only build）" % "、".join(missing))
        print("PLUGIN-INIT-ROLLBACK: SKIP (build-missing)")
        return 2

    fx = Path(tempfile.mkdtemp(prefix="xrkit_plugin_rollback_"))
    try:
        shutil.copy2(FIXTURE, fx / FIXTURE.name)
        proc = subprocess.run([str(CLI), "--plugin-init-rollback", str(fx)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        rc = proc.returncode
        text = proc.stdout.decode("utf-8", errors="replace")
    finally:
        shutil.rmtree(fx, ignore_errors=True)

    print("[plugin-init-rollback] 夹具=%s（现拷到 %%TEMP%% 的空目录里跑）" % FIXTURE.name)
    for line in text.splitlines():
        if line.startswith("PLUGINROLLBACK"):
            print("    " + line.strip())

    lines = [ln.strip() for ln in text.splitlines() if ln.startswith("PLUGINROLLBACK")]
    res_line = [ln for ln in lines if ln.startswith("PLUGINROLLBACK residue ")]
    info_line = [ln for ln in lines if ln.startswith("PLUGINROLLBACK info ")]
    res = fields(res_line[-1]) if res_line else {}
    info = fields(info_line[-1]) if info_line else {}

    # ① 夹具真的触发了"入口返回 0"这条失败路（否则后面断言的是"空判据"）
    want(any(ln.startswith("PLUGINROLLBACK load-error ") and "入口返回 0" in ln for ln in lines),
         "① 夹具真的触发：宿主如实报『入口返回 0』",
         "输出=%r" % text.strip()[:300])
    want(info.get("dll") == FIXTURE.name, "① 加载的 owner 就是坏 init 夹具",
         "info=%r" % info)

    # ② 主判据：按 owner 逐容器零残留 + mark 对账 + 没记进已加载名单
    want(bool(res), "② 有 `PLUGINROLLBACK residue` 逐项行（否则没有按 owner 的判据）",
         "输出=%r" % text.strip()[:300])
    dirty = {k: res.get(k) for k in REG_KEYS if res.get(k) not in (None, "0")}
    want(bool(res) and not dirty, "② ★主判据：9 个注册容器按 owner 逐项为 0（零残留）",
         "还留着的容器=%r（整行 %r）" % (dirty, res_line[-1] if res_line else ""))
    want(res.get("registrations") == "0", "② `registrations=0`（9 个容器之和）",
         "实得 %r" % res.get("registrations"))
    want(bool(info) and info.get("mark") == info.get("count"),
         "② `registration_count()` 回到加载前的 mark",
         "mark=%r count=%r" % (info.get("mark"), info.get("count")))
    want(bool(info) and info.get("loaded_before") == info.get("loaded_after"),
         "② `loaded_count()` 没把加载失败的它记进去",
         "before=%r after=%r" % (info.get("loaded_before"), info.get("loaded_after")))

    # ③ 留痕仍在（零静默）—— errors_ 特意不在"零残留"范围里
    errors = res.get("errors")
    want(errors is not None and errors.isdigit() and int(errors) >= 1,
         "③ 失败留痕仍在（errors>=1；零静默：理由必须看得见）",
         "实得 errors=%r" % errors)
    want(any(ln.startswith("PLUGINROLLBACK error ") for ln in lines),
         "③ `PLUGINROLLBACK error` 逐条打印出留痕原文", "输出=%r" % text.strip()[:300])

    # ④【S2-2，B 批】扩展名判定必须**大小写不敏感**：`NLC.DLL` 也是插件。
    #   修前：`load_plugins_from_dir` 判原名的 `.dll`（小写化结果丢弃＝死代码），CLI 探针又抄了
    #   同样一份大小写敏感判定 ⇒ 同一份夹具改名成 `NLC.DLL` 后**静默漏载**：不报错、不进
    #   errors、连探针都找不到 owner（实测 `PLUGINROLLBACK skip reason=目录里没有 *.dll`、rc=2）。
    fx_upper = Path(tempfile.mkdtemp(prefix="xrkit_plugin_upper_"))
    try:
        shutil.copy2(FIXTURE, fx_upper / "NLC.DLL")
        proc_u = subprocess.run([str(CLI), "--plugin-init-rollback", str(fx_upper)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        rc_u = proc_u.returncode
        text_u = proc_u.stdout.decode("utf-8", "replace")
    finally:
        shutil.rmtree(fx_upper, ignore_errors=True)
    lines_u = [ln.strip() for ln in text_u.splitlines() if ln.startswith("PLUGINROLLBACK")]
    info_u_lines = [ln for ln in lines_u if ln.startswith("PLUGINROLLBACK info ")]
    info_u = fields(info_u_lines[-1]) if info_u_lines else {}
    print("    [大写扩展名] %s" % (lines_u[0] if lines_u else text_u.strip()[:200]))
    want("PLUGINROLLBACK skip" not in text_u and info_u.get("dll") == "NLC.DLL",
         "④ 大写扩展名 `NLC.DLL` 被认成插件（不许静默漏载）",
         "输出=%r" % text_u.strip()[:300])
    want(any("入口返回 0" in ln for ln in lines_u),
         "④ 大写扩展名那条也真跑到『入口返回 0』（不是空判据）",
         "输出=%r" % text_u.strip()[:300])
    want(rc_u == 0, "④ 大写扩展名那条探针自身退出码 0", "exit=%d" % rc_u)

    # 收尾：探针自己必须报 ok（退出码与结论行一起才算过）
    want(rc == 0 and any(ln.startswith("PLUGINROLLBACK ok") for ln in lines),
         "探针退出码 0 且结论行 = `PLUGINROLLBACK ok`", "exit=%d 输出=%r" % (rc, text.strip()[:300]))

    print("-" * 66)
    if PROBLEMS:
        print("PLUGIN-INIT-ROLLBACK fail 不成立的断言 %d 条：%s"
              % (len(PROBLEMS), "；".join(PROBLEMS)))
        return 1
    print("PLUGIN-INIT-ROLLBACK ok 覆盖=按owner逐容器零残留+mark对账+留痕可见")
    return 0


if __name__ == "__main__":
    sys.exit(main())
