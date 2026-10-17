# -*- coding: utf-8 -*-
r"""README ↔ 实现 逐条核对（用户 2026-10-16 指派：产品线从 README 起手）。

★ 纪律：**每条断言都要写清核查范围**（「审计方法节」第 5 条）。
  本脚本每条都打印"我核了什么、结论是什么"，不写全称断言。
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit_2.x")
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
readme = (ROOT / "README.md").read_text(encoding="utf-8")
bad = []


def check(label, ok, detail=""):
    print("  %s %-52s %s" % ("ok " if ok else "✗  ", label, detail[:90]))
    if not ok:
        bad.append(label)


print("== A. README 引用的**仓库内路径**是否存在 ==")
# ★ 本批自己踩的坑：第一版把所有 `` `xxx.ext` `` 都当"仓库内路径" ⇒ **3 条误报**：
#   · `cl.exe` / `xrEngine.exe` —— **外部工具与游戏文件**，本来就不在仓库里；
#   · `run_ci.py` —— README 是**按 basename** 引用的（"python tests\run_ci.py"），
#     而我按"根目录下有没有 run_ci.py"去查 ⇒ 必然找不到。
#   ⇒ 口径改成：**排除外部工具名、并按 basename 在仓库内搜**（写清范围，不给全称断言）。
EXTERNAL = {"cl.exe", "xrEngine.exe", "windeployqt.exe", "ffmpeg.exe", "ffprobe.exe",
            "cmake.exe", "ninja.exe", "python.exe"}
paths = sorted(set(re.findall(r"`([A-Za-z0-9_./\-]+\.(?:md|py|ps1|json|exe))`", readme)))
for rel in paths:
    if rel.startswith(("build/", "E:")) or rel in EXTERNAL:
        continue                      # 产物 / 本机绝对路径 / 外部工具，不属"仓库内引用"
    direct = (ROOT / rel).is_file()
    # 按 basename 在仓库里搜（README 常用裸名引用）
    by_name = direct or any(True for _ in ROOT.rglob(Path(rel).name)
                            if _.is_file() and ".git" not in str(_))
    check(rel, by_name,
          "" if direct else ("（按 basename 命中）" if by_name else "★ 仓库内找不到"))

print("== B. README 列的 CLI 能力清单 ⊂ `--help` 实际 ==")
if CLI.is_file():
    help_txt = ((subprocess.run([str(CLI), "--help"], capture_output=True, timeout=120).stdout or b"")
                .decode("utf-8", "replace"))
    listed = re.findall(r"`(--[a-z0-9\-]+)`", readme)
    missing = [f for f in listed if f not in help_txt]
    check("README 列的 CLI 动作都在 --help 里", not missing,
          ("缺：" + ",".join(missing)) if missing else "共核 %d 个" % len(listed))
else:
    check("CLI 构建产物存在", False, "缺 %s（判据没跑成，不是通过）" % CLI)

print("== C. README 里的**版本口径**是否与真来源一致 ==")
cur = json.loads((ROOT / "version.json").read_text(encoding="utf-8"))["current"]
vh = (ROOT / "include" / "xrkit" / "core" / "version.h").read_text(encoding="utf-8")
hdr = re.search(r'kPrerelease\s*=\s*"([^"]*)"', vh).group(1)
check("README 标题里的版本号 == version.json 的 version",
      ("2.0.0" in readme.splitlines()[0]) and cur["version"] == "2.0.0",
      "标题=%r version.json=%s" % (readme.splitlines()[0][:40], cur["version"]))
# ★★ P17-132 修正：**本条不许把 `version.json` 与 `version.h` 的同步当成自己的判据**。
#   事实（本批实测）：门禁里**有步骤走打包链** ⇒ `version.json` 被顶一格，而 `version.h`
#   的写回**滞后一格**（正规发布链才同步）⇒ 在这一步的时刻，两者**本来就可能不同步**。
#   而"版本同步"**已有专门的步骤**判它（`version-identity`：真来源 tree → version.h → .rc → 包内 exe）。
#   ⇒ 本条**只核 README 的说法**；两者不同步时**只观测不判红**。
#   ★ 纪律：**一个判据不越权判别人负责的事** —— 越权会让它**常红**，而常红 = 没有判据。
sync = (hdr == "%s.%s" % (cur["prerelease"], cur["index"]))
if sync:
    check("version.h 的 kPrerelease == version.json 的 标记.序号", True, "version.h=%s" % hdr)
else:
    print("  ·  version.h=%s vs version.json=%s.%s ⇒ **只观测**"
          "（版本同步由 `version-identity` 步判；门禁走打包链时 version.json 会被顶一格、"
          "version.h 滞后，属已知过渡态）" % (hdr, cur["prerelease"], cur["index"]))
# README 自己关于版本的说法（这才是本条该管的）
check("README 未把预发布号写死（避免与 version.h 脱同源）",
      not re.search(r"ALPHA\.\d+", readme),
      "标题只写主版本是稳的；出现 ALPHA.N 就要人工核对")

print("== D. README 的三档门禁说法 == run_ci 的实际参数 ==")
runc = (ROOT / "tests" / "run_ci.py").read_text(encoding="utf-8")
for flag in ("--fast", "--release", "--only"):
    check("run_ci 支持 %s" % flag, flag in runc)

print("== E. README 说「C ABI 已退役」是否与仓库现状一致（**需人工判读**）==")
abi = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "include" / "xrkit" / "plugin").glob("abi*.h")) \
    if (ROOT / "include" / "xrkit" / "plugin").is_dir() else []
print("    仓库里 include/xrkit/plugin/abi*.h =", abi or "(无)")
print("    README 原文：历史遗留头 `include/xrkit/plugin/abi.h` 仅供旧件读取，不是新插件的入口")
print("    ⇒ 本项**只观测不判红**（它说的是「保留但退役」，不是「删除」）")

print()
print("README-CHECK %s：%d 条不成立" % ("FAIL" if bad else "ok", len(bad)))
for b in bad:
    print("  · " + b)
sys.exit(1 if bad else 0)
