# -*- coding: utf-8 -*-
r"""批次 C：把**发行口径链**（RC/FINAL，`-Dev:$false`）做成门禁步。

分层清单里这是**唯一没有判据**的一层（门禁对 `package.ps1` 全是静态核，从未真跑）。
本判据的口径（★ 环境受限时**如实 prereq**，不假绿、不假红）：
· 真跑 `scripts/package.ps1 -Dev:$false`（**发行口径**：`plugins/` 为空），落临时目录；
· 断言：产物里 **没有** 任何 `plugins/*.py` / `_fixtures/`（发行口径的定义）；
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
    r"""解析可用的 PowerShell 宿主：优先 `pwsh`（7+），退回 `powershell.exe`（5.1）。

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
    # ★ 已知产品侧前提（本判据不绕过它、也不为它改产品）：
    #   `package.ps1` 自设 `$ErrorActionPreference='Stop'` ⇒ `windeployqt.exe` /
    #   `ffmpeg.exe` 往 **stderr** 写的**正常 warning** 会被当成**终止错误**
    #   ⇒ 脚本中断（实测："Translations will not be available…"）。
    #   ★ 打包器（`--build-package`）走另一套宿主偏好所以正式打包没事 ——
    #     这正是"门禁里从没跑过它"的又一层原因：换个宿主就换一套 stderr 语义。

    # ★★ P17-128 修正②：**期望值从 `version.h` 取，不从 `version.json` 取**。
    #
    # 我先前的两版都错了，记在这里免得再犯：
    #   · 第一版：两边不一致就 `return 2`（skip）⇒ 全量门禁里这一步**永远 SKIP**；
    #   · 第二版：**在判据里改写 `version.h` 并重建** ⇒ **判据动了产品状态源**，
    #     而且只重建了 shell、没重建 release ⇒ 包内 exe 仍是旧串 ⇒ 打包到核对那步必炸。
    # ⇒ 正解：`version.h` 是**包内 exe ProductVersion 的直接来源** ⇒ 期望值就取它；
    #   `version.json` 只是"最后一次发包"的记账，**与本次打包是否成功无关**。
    #   （★ 本批实测：门禁脚本里**没有任何一处**写 `version.json` —— 我先前把手工残留
    #     读成了"门禁顶号"，那是错的。）
    vh = ROOT / "include" / "xrkit" / "core" / "version.h"
    vcur = json.loads(VERSION.read_text(encoding="utf-8"))["current"]
    if not vh.exists():
        print("PKG-RELEASE skip 缺 version.h（判据没跑成，不是通过）")
        return 2
    vh_text = vh.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'kPrerelease\s*=\s*"([^"]*)"', vh_text)
    hdr = (m.group(1) if m else "").strip()
    if not hdr:
        print("PKG-RELEASE skip version.h 里读不出 kPrerelease")
        return 2
    # ★★ P17-136（复审计 09 分册 ⑩）：**单源**。判据自称"期望值取 `version.h`"，
    #   但修前 `expect` 与产物目录都用了 `version.json` 的 `version`/`prerelease`/`index`
    #   ⇒ **混用两个来源**，两者不一致时行为说不清。
    #   ⇒ 现在**三件事全从 `version.h` 出**：主版本、标记、序号。
    mv = re.search(r"kVersionMajor\s*=\s*(\d+)", vh_text)
    mn = re.search(r"kVersionMinor\s*=\s*(\d+)", vh_text)
    mp = re.search(r"kVersionPatch\s*=\s*(\d+)", vh_text)
    if not (mv and mn and mp):
        print("PKG-RELEASE skip version.h 里读不出 kVersionMajor/Minor/Patch")
        return 2
    mainver = "%s.%s.%s" % (mv.group(1), mn.group(1), mp.group(1))
    # `kPrerelease` = "<标记>.<序号>"
    if "." not in hdr:
        print("PKG-RELEASE skip kPrerelease 形状不对：%r（期望「标记.序号」）" % hdr)
        return 2
    mark, idx = hdr.split(".", 1)
    expect = "%s %s" % (mainver, hdr)
    if hdr != "%s.%s" % (vcur["prerelease"], vcur["index"]):
        print("  前提：`version.h`=%r、`version.json`=%r 不一致 —— 本次按 **version.h** 打包"
              "（它是包内 exe 的直接来源；不一致本身由 `version-identity` 步负责判）"
              % (hdr, "%s.%s" % (vcur["prerelease"], vcur["index"])))
    # ★ 不用 tempfile：`package.ps1:607` 是 `$dist = Join-Path $root $OutDir`，
    #   所以 `-OutDir` **必须是相对仓库根**的目录名（默认 dist）——
    #   传绝对路径会被拼成 `E:/.../2.x/C:/.../dist` ⇒ `New-Item` 报
    #   「The given path's format is not supported」。
    rel_out = "build/_pkgrel_check/dist"
    out = ROOT / rel_out
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    try:
        v = json.loads(VERSION.read_text(encoding="utf-8"))["current"]
        ps = _powershell()
        if ps is None:
            print("PKG-RELEASE skip 本机既没有 pwsh 也没有 powershell.exe")
            return 2
        print("  宿主：%s" % ps)
        # ★ 用 `-Command` 而不是 `-File`：5.1 的 `-File` 拒收 `[bool]` 形参的字符串值（实测）。
        # ★★ 局部覆盖宿主偏好：`package.ps1` 自己设 `$ErrorActionPreference='Stop'`，
        #   而 `windeployqt`/`ffmpeg` 往 stderr 写的**正常 warning** 会被当成终止错误
        #   ⇒ 脚本在**我们自己调它**的时候中断（打包器走另一套宿主偏好，所以正式打包没这问题）。
        #   ★ 判据在自己这一侧覆盖，**不动产品脚本一个字节**。
        inner = ("$ErrorActionPreference='Continue'; "
                 "& '%s' -OutDir '%s' -BuildIndex %d -VersionString '%s' -Dev:$false"
                 % (PS, rel_out, v["index"], expect))
        args = [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", inner]
        try:
            proc = subprocess.run(args, capture_output=True, timeout=3000, cwd=str(ROOT))
        except subprocess.TimeoutExpired:
            print("PKG-RELEASE ✗ 打包超时"); return 1
        text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
        if proc.returncode != 0:
            # ★★ P17-133（复审计 09 分册 High #1，本批修正）：**默认判红（fail-closed）**。
            #
            #   修前（错）：只要退出码非零就 `return 2`（环境不具备）⇒ 门禁走 `prereq` ⇒
            #     `--fast` 档 **PASS**。而 `package.ps1` 里十几条 `throw` 是**产品断言**
            #     （版本身份 / 包内自检 / 可见页 / 插件目录）⇒ **任何一条破了都会被伪装成
            #     「机器不行」**。这是我这套东西一路上最反对的那种假绿，却由我亲手造出来。
            #
            #   现在：**只有**能给出**正面环境证据**时才降级成"环境不具备"；
            #     证据 = 实测过的**本机拦阻签名**（文件拷贝被拒 / 可执行起不来 / 工具不在）。
            #     其余一律 `fail`（判据不成立）。
            #
            #   ★ 与 `ffmpeg` 版本判定那条同族：「**认不出就报错**」——
            #     这里禁止"认不出就降级成环境"。
            ENV_SIGNATURES = (
                "PermissionDenied",              # 本机对特定文件名的写入/拷贝拦阻（实测过）
                "Access is denied",
                "UnauthorizedAccessException",
                "is not recognized as the name",  # 宿主/工具不在 PATH
                "not found. Looked in",          # package.ps1 自己的 ffmpeg/ffprobe 缺件文案
                "找不到 windeployqt",
            )
            env_hits = [s for s in ENV_SIGNATURES if s in text]
            tail = [l.strip() for l in text.splitlines() if l.strip()][-4:]
            print("PKG-RELEASE 打包脚本退出码 %s（输出 %d 行）" % (proc.returncode, len(text.splitlines())))
            for l in tail:
                print("    " + l[:150])
            if env_hits:
                print("  ⇒ 命中**正面环境证据** %r ⇒ 记为「环境不具备」（未覆盖），**不当判据失败**"
                      % env_hits[:2])
                return 2
            print("  ⇒ **没有**正面环境证据 ⇒ **判红**（不允许把产品断言失败伪装成"
                  "「机器不行」；复审计 09 分册 High #1 / P17-133）")
            return 1
        # ① 发行口径：没有插件夹具
        # ★★ P17-136（复审计 09 分册 ⑧）：真实布局是 `dist/<版本>/<标记>/<序号>`（**三层**）。
        #   修前算成 `dist/2.0.0/ALPHA.208`（两层）⇒ **永远不存在** ⇒ 每次走 `rglob` 兜底
        #   ⇒ 那条"确定性分支"从未被验证过、也从不命中（"对着空气"的又一种形状）。
        pkgdir = out / mainver / mark / idx
        if not pkgdir.exists():
            cands = [p for p in out.rglob("xrkit_app.exe")]
            print("  ⚠ 确定性产物目录没命中：期望 `%s`（实际布局可能变了）" % pkgdir.relative_to(ROOT))
            print("    已找到 %d 个候选 xrkit_app.exe%s"
                  % (len(cands), ("：" + str(cands[0].parent.relative_to(ROOT))) if cands else ""))
            if not cands:
                return fail("打包退出码 0，但产物里找不到 xrkit_app.exe")
            pkgdir = cands[0].parent
        else:
            print("  ✓ 确定性产物目录命中：`%s`（三层布局：版本/标记/序号）"
                  % pkgdir.relative_to(ROOT))
        top_plugins = pkgdir / "plugins"
        py_top = sorted(p.name for p in top_plugins.glob("*.py")) if top_plugins.exists() else []
        # ★ P17-128：输出文案改用**正斜杠** —— 原来是反引号+反斜杠，那个 `\`` 是**非法转义**
        #   （现在只是 SyntaxWarning，将来会变错误；门禁输出里也难看）。
        fix = pkgdir / "plugins" / "_fixtures"
        if py_top:
            return fail("发行口径包里 `plugins/` 顶层仍有 .py：%s" % ", ".join(py_top[:5]))
        if fix.exists() and any(fix.iterdir()):
            return fail("发行口径包里仍有 `plugins/_fixtures/` 夹具")
        print("  ✓ 发行口径：`plugins/` 顶层无 .py、`plugins/_fixtures/` 已剥离")
        # ② 默认可见页＝六个内置页（逐名同序）
        exe = pkgdir / ("xrkit_app.exe")
        if not exe.exists():
            return fail("产物里没有 xrkit_app.exe")
        smoke = subprocess.run([str(exe), "--smoke"], capture_output=True, timeout=1800,
                               cwd=str(pkgdir))
        st = ((smoke.stdout or b"") + (smoke.stderr or b"")).decode("utf-8", "replace")
        # ★★ B-4（第三方只读审计 2026-10-18）：**优先认新 ASCII 行**（`PLUGIN-PAGES pages=…`），
        #   认不到再退回旧的中文行（迁移期两种都在；老包只发旧行）。两套正则**同一个出口**：
        #   解出来的三个数字与 id 名单必须同源，不许各解析一半。
        m = re.search(r"PLUGIN-PAGES pages=(\d+) builtin=(\d+) plugin_py=(\d+) owner_matches_id=(\d)", st)
        ids: list = []
        used_new_line = m is not None
        if m:
            pages, builtin, plugin = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            m_list = re.search(r"PLUGIN-PAGES pages=[^\n]*?names=\[([^\]]*)\]", st)
            ids = re.findall(r"\d+=([A-Za-z0-9_]+)", m_list.group(1)) if m_list else []
            owner_ok = m.group(4) == "1"
            print("  ✓（新 ASCII 行）可见页集合=%d 页（内置=%d / 插件py=%d）" % (pages, builtin, plugin))
        else:
            m = re.search(r"PLUGIN-PAGES 可见页集合=(\d+) 页（内置=(\d+) / 插件py=(\d+)）", st)
            if not m:
                for l in st.splitlines():
                    if "PLUGIN-PAGES" in l:
                        print("    原始行：" + l.strip()[:130]); break
                return fail("产物 `--smoke` 没打出 `PLUGIN-PAGES`（新 ASCII 行与旧中文行都没有）"
                            "（认不出就报错，不静默放行）")
            pages, builtin, plugin = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            m_list = re.search(r"PLUGIN-PAGES[^\n]*?\[([^\]]*)\]", st)
            owner_ok = True
            print("  ✓（旧中文行）可见页集合=%d 页（内置=%d / 插件py=%d）" % (pages, builtin, plugin))
        if plugin != 0:
            return fail("发行口径包里插件页数应为 0，实测 %d" % plugin)
        if builtin != 6:
            return fail("默认内置页应为 6，实测 %d" % builtin)
        # ★★ P17-136（复审计 09 分册 ⑦）：**验名，不只数数**。
        #   事实：smoke 那行本来就带 `[1=fs= 2=convert= …]`（旧）或 `names=[1=fs,2=convert,…]`（新）
        #   的有序 id 列表，而修前只用正则抓了三个计数 ⇒ "换了哪六个页"**完全没被检查**。
        #   ⇒ 解析方括号里的有序 id，断言**顺序与名字**都对。
        if not m_list:
            return fail("产物 `--smoke` 的 PLUGIN-PAGES 行里没有页名列表 `[...]`"
                        "（无法验名 ⇒ 认不出就报错，不静默放行）")
        if not ids:
            # 旧行：`1=fs=`（内置，尾部带标记串）或 `2=fs_py`（插件）⇒ 两种都要认
            for item in m_list.group(1).split():
                naming = re.match(r"^\d+=(.+)$", item)
                if naming is not None:
                    ids.append(naming.group(1).rstrip("="))
        EXPECT_PAGES = ["fs", "convert", "text", "xml", "video", "fontpack"]
        if ids != EXPECT_PAGES:
            return fail("发行口径包的**内置页名单/顺序**不符：期望 %s，实测 %s"
                        % (EXPECT_PAGES, ids))
        print("  ✓ 内置页**实名与顺序**都对：%s" % ", ".join(ids))
        if not owner_ok:
            return fail("`owner_matches_id=0`：内置标记与 id 后缀不自洽（smoke 自己算的那条不变量红了）")
        # 旧中文行才有这一格；新 ASCII 行用 `owner_matches_id=` 承载（上面已经判过）——
        # 迁移期两行并存，这里只在"本轮认的是旧行"时要求它。
        if used_new_line:
            m_ok = re.search(r"内置标记与 id 后缀一致=(\d)", st)
            if m_ok and m_ok.group(1) != "1":
                return fail("产物自报「内置标记与 id 后缀一致=%s」⇒ 页 id 与内置标记不同源"
                            % m_ok.group(1))
        else:
            m_ok = re.search(r"内置标记与 id 后缀一致=(\d)", st)
        if not m_ok:
            return fail("产物 `--smoke` 没打出「内置标记与 id 后缀一致=」（认不出就报错）")
        if m_ok.group(1) != "1":
            return fail("产物自报「内置标记与 id 后缀一致=%s」⇒ 页 id 与内置标记不同源"
                        % m_ok.group(1))
        if smoke.returncode != 0:
            return fail("发行口径产物 `--smoke` 退出码 %s" % smoke.returncode)
        print("PKG-RELEASE ok 发行口径链跑通：`-Dev:$false` 打包成功、无插件夹具、"
              "默认可见页＝六个内置页、产物自检退出码 0")
        return 0
    finally:
        shutil.rmtree(ROOT / "build" / "_pkgrel_check", ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
