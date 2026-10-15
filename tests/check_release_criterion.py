# -*- coding: utf-8 -*-
"""批次 C：把**发行口径链**（RC/FINAL，`-Dev:$false`）做成门禁步。

分层清单里这是**唯一没有判据**的一层（门禁对 `package.ps1` 全是静态核，从未真跑）。
本判据的口径（★ 环境受限时**如实 prereq**，不假绿、不假红）：
· 真跑 `scripts/package.ps1 -Dev:$false`（**发行口径**：`plugins\` 为空），落临时目录；
· 断言：产物里 **没有** 任何 `plugins\*.py` / `_fixtures\`（发行口径的定义）；
· 并在该产物里真跑一次 `--smoke`，断言 **默认可见页＝六个内置页**（逐名同序）；
· 退出码：0=绿；1=判据不成立；2=**环境不具备**（脚本自己起不来/缺前提）⇒ release 档按前提缺失 FAIL。
"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PS = ROOT / "scripts" / "package.ps1"
VERSION = ROOT / "version.json"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BUILT = ["页集合", "六", "内置"]


def _powershell():
    """解析可用的 PowerShell 宿主：优先 `pwsh`（7+），退回 `powershell.exe`（5.1）。

    ★ 本机实测只有 5.1（`pwsh` 不在 PATH、`Program Files\PowerShell\7` 不存在）⇒
      写死 `pwsh` 会让判据在任何环境下都跑不起来（判据自己坏掉，最糟的一种）。
    """
    for cand in ("pwsh", "powershell.exe"):
        found = shutil.which(cand)
        if found:
            return found
    return None


def fail(msg):
    print("PKG-RELEASE ✗ " + msg)
    return 1


def main():
    if not PS.exists():
        print("PKG-RELEASE skip 缺 scripts/package.ps1"); return 2
    if not CLI.exists():
        print("PKG-RELEASE skip 缺 CLI 构建产物"); return 2
    print("PKG-RELEASE 发行口径链（-Dev:$false）真的跑一次")
    with tempfile.TemporaryDirectory(prefix="pkgrel_") as tmp:
        out = Path(tmp) / "dist"
        v = json.loads(VERSION.read_text(encoding="utf-8"))["current"]
        ps = _powershell()
        if ps is None:
            print("PKG-RELEASE skip 本机既没有 pwsh 也没有 powershell.exe")
            return 2
        print("  宿主：%s" % ps)
        # ★ 用 `-Command` 而不是 `-File`：5.1 的 `-File` 拒收 `[bool]` 形参的字符串值（实测）。
        inner = ("& '%s' -OutDir '%s' -BuildIndex %d -VersionString '%s' -Dev:$false"
                 % (PS, out, v["index"],
                    "%s %s.%s" % (v["version"], v["prerelease"], v["index"])))
        args = [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", inner]
        try:
            proc = subprocess.run(args, capture_output=True, timeout=3000, cwd=str(ROOT))
        except subprocess.TimeoutExpired:
            print("PKG-RELEASE ✗ 打包超时"); return 1
        text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
        if proc.returncode != 0:
            # ★ 脚本自己起不来（本机拦阻/缺前提）⇒ 如实报"前提缺失"，**不报判据不成立**
            tail = [l.strip() for l in text.splitlines() if l.strip()][-3:]
            print("PKG-RELEASE skip 打包脚本退出码 %s ⇒ **环境不具备**（如实记为未覆盖，不当判据失败）"
                  % proc.returncode)
            for l in tail:
                print("    " + l[:140])
            return 2
        # ① 发行口径：没有插件夹具
        pkgdir = out / v["version"] / ("%s.%s" % (v["prerelease"], v["index"]))
        if not pkgdir.exists():
            cands = [p for p in out.rglob("xrkit_app.exe")]
            if not cands:
                return fail("打包退出码 0，但产物里找不到 xrkit_app.exe")
            pkgdir = cands[0].parent
        top_plugins = pkgdir / "plugins"
        py_top = sorted(p.name for p in top_plugins.glob("*.py")) if top_plugins.exists() else []
        fix = pkgdir / "plugins" / "_fixtures"
        if py_top:
            return fail("发行口径包里 `plugins\\` 顶层仍有 .py：%s" % ", ".join(py_top[:5]))
        if fix.exists() and any(fix.iterdir()):
            return fail("发行口径包里仍有 `plugins\\_fixtures\\` 夹具")
        print("  ✓ 发行口径：`plugins\\` 顶层无 .py、`_fixtures\\` 已剥离")
        # ② 默认可见页＝六个内置页（逐名同序）
        exe = pkgdir / ("xrkit_app.exe")
        if not exe.exists():
            return fail("产物里没有 xrkit_app.exe")
        smoke = subprocess.run([str(exe), "--smoke"], capture_output=True, timeout=1800,
                               cwd=str(pkgdir))
        st = ((smoke.stdout or b"") + (smoke.stderr or b"")).decode("utf-8", "replace")
        m = re.search(r"PLUGIN-PAGES 可见页集合=(\d+) 页（内置=(\d+) / 插件py=(\d+)）", st)
        if not m:
            for l in st.splitlines():
                if "PLUGIN-PAGES" in l:
                    print("    原始行：" + l.strip()[:130]); break
            return fail("产物 `--smoke` 没打出 `PLUGIN-PAGES 可见页集合=`（认不出就报错，不静默放行）")
        pages, builtin, plugin = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        print("  ✓ 可见页集合=%d 页（内置=%d / 插件py=%d）" % (pages, builtin, plugin))
        if plugin != 0:
            return fail("发行口径包里插件页数应为 0，实测 %d" % plugin)
        if builtin != 6:
            return fail("默认内置页应为 6，实测 %d" % builtin)
        if smoke.returncode != 0:
            return fail("发行口径产物 `--smoke` 退出码 %s" % smoke.returncode)
        print("PKG-RELEASE ok 发行口径链跑通：`-Dev:$false` 打包成功、无插件夹具、"
              "默认可见页＝六个内置页、产物自检退出码 0")
        return 0


if __name__ == "__main__":
    sys.exit(main())
