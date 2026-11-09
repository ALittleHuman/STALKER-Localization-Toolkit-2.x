# -*- coding: utf-8 -*-
"""判据：**缺前置的插件必须被自动禁用**（启动期），且清单可被插件管理器结构化读取。

口径（用户 2026-11-09）：
  · 插件清单统一字段：稳定 `id` / `version` / `repo`（仅元数据）/ `depends`（id→版本范围）/
    `provides` / `replaces`；依赖判定只看 **id**，不看仓库地址（改名/转移/镜像都会变）。
  · 启动时：前置缺失 / 版本不满足 / 前置自身被禁用 / 循环依赖 ⇒ **该插件禁用**
    （不调用 `register(api)`，一条注册都不留），并把"缺什么"记下来。
  · `--py-plugins --json` 输出结构化清单（id/version/repo/depends/provides/replaces），
    供插件管理器读；`PYPLUGINS disabled ...` 行是人类可读的同一事实。

夹具（`tests/python_plugins_deps/`，全通用名，不含真机模组名）：
  aa_provider  id=demo.base 1.2.0（含 provides 别名）
  bb_consumer  id=demo.addon 1.0.0 depends demo.base >=1.0,<2     ⇒ 启用
  cc_missing   id=demo.broken depends demo.absent >=1              ⇒ 禁用（未安装）
  dd_version   id=demo.old depends demo.base >=2.0                 ⇒ 禁用（版本不满足）
  ee/ff_cycle  demo.cycle_a ↔ demo.cycle_b                          ⇒ 禁用（循环依赖）

退出码：0=全绿 / 1=有断言不成立 / 2=CLI 缺失（外部前提）。
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                         "cli" / "xrkit_cli.exe")))
FIXTURES = HERE / "python_plugins_deps"

PROBLEMS: list[str] = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("（%s）" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append(label + (("（%s）" % detail) if detail else ""))
    return False


def run_cli(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    if not CLI.is_file():
        print("!! 找不到 CLI：%s" % CLI)
        print("PLUGIN-DEPS: SKIP (cli-missing)")
        return 2
    if not FIXTURES.is_dir():
        print("!! 缺夹具目录：%s" % FIXTURES)
        print("PLUGIN-DEPS: SKIP (fixtures-missing)")
        return 2

    rc, out = run_cli(["--py-plugins", str(FIXTURES), "--json"])
    print("== 插件前置：缺前置自动禁用 + 结构化清单 ==")
    want(rc == 0, "CLI 退出码 0", "rc=%d" % rc)
    lines = out.splitlines()
    summary = [ln for ln in lines if ln.startswith("PYPLUGINS files=")]
    want(bool(summary), "有 `PYPLUGINS files=` 汇总行", summary[0] if summary else "")
    if summary:
        text = summary[0]
        want("files=6" in text, "扫到 6 个 .py", text)
        want("loaded=2" in text, "只加载 2 个（provider + 正常 consumer）", text)
        want("errors=0" in text, "错误为 0（被禁用的插件不算加载失败）", text)

    disabled = [ln for ln in lines if ln.startswith("PYPLUGINS disabled plugin=")]
    want(len(disabled) == 4, "4 个插件因前置问题被禁用", "实得 %d：%s" % (len(disabled), disabled[:2]))
    joined = "\n".join(disabled)

    def has_id(pid: str) -> bool:
        return any((" id=%s " % pid) in (ln + " ") for ln in disabled)

    want(has_id("demo.broken"), "缺前置（未安装）的 demo.broken 被禁用")
    want("demo.absent(>=1)=未安装" in joined,
         "理由点名「未安装」和缺失 id（零静默）", joined[:200])
    want(has_id("demo.old"), "版本不满足的 demo.old 被禁用")
    want("已安装 1.2.0，不满足要求" in joined, "理由写出「已安装 x，不满足要求」")
    want(has_id("demo.cycle_a") and has_id("demo.cycle_b"), "循环依赖两个都被禁用")
    want(has_id("demo.base") is False, "provider 本身没被禁用")

    json_lines = [ln for ln in lines if ln.startswith("PYPLUGINS-JSON ")]
    want(bool(json_lines), "有 `PYPLUGINS-JSON` 结构化清单")
    if json_lines:
        try:
            items = json.loads(json_lines[0][len("PYPLUGINS-JSON "):])
        except Exception as exc:  # noqa: BLE001
            items = []
            want(False, "JSON 可解析", repr(exc))
        else:
            want(True, "JSON 可解析", "%d 条" % len(items))
        by_id = {str(it.get("id")): it for it in items}
        want(len(items) == 6 and set(by_id) >= {"demo.base", "demo.addon", "demo.broken"},
             "JSON 覆盖 6 个插件且带稳定 id", "ids=%s" % sorted(by_id)[:6])
        base = by_id.get("demo.base", {})
        want(base.get("version") == "1.2.0", "provider 的 version 进了清单", str(base.get("version")))
        want(base.get("repo") == "https://github.com/demo/base",
             "repo 只作元数据（原样保留，不参与判定）", str(base.get("repo")))
        want("demo.base.alias" in (base.get("provides_list") or []),
             "provides 别名进了清单", str(base.get("provides_list")))
        addon = by_id.get("demo.addon", {})
        want(addon.get("depends") == {"demo.base": ">=1.0,<2"},
             "depends（id→版本范围）进了清单", str(addon.get("depends")))

    print("")
    if PROBLEMS:
        print("PLUGIN-DEPS fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("PLUGIN-DEPS ok %d 项（缺前置自动禁用；JSON 清单可供插件管理器读）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
