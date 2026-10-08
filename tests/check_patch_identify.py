# -*- coding: utf-8 -*-
"""P16-1b 判定半判据：C++ 侧 `--patch-identify` 与 1.x 金标准**逐条对拍**。

为什么要有它：引擎侧的判态/指纹状态机是从 1.x `engine_utf8_patch.py` 逐条搬的，
"搬对了没有"必须拿 **1.x 自己的输出**当尺子。1.x 插件住在仓库外 ⇒ 不能把门禁挂在外部路径上，
所以由一次性引导脚本（工作区）把 1.x 的结果导出成仓库内的**金标准**
（`tests/golden_patch/identify_golden.json`，含 1.x 脚本 sha256 与样本 sha256 ⇒ 防漂移）。

判据（逐样本、逐项）
--------------------
① 结论 `verdict key` 相等；
② 五个汇总计数（orig/patched/kept/broken/other/context/range）逐项相等；
③ `thunk_intact` / `cave_all_zero` 相等；
④ **逐项判态**：金标准里每一项的 `state` 都必须相等（逐条报 `id: 期望 vs 实得`）；
⑤ 计数下限：每个样本至少比 **14 项**（空集/少比不算通过）。

★ `diff_bytes`（差异字节数）**只打印、不参与判定** —— 用户红线：
   实测 v3 与 v4 该数完全相同（都是 88），字节数分不出形态。

输出：`PATCH-IDENT ok samples=N items=M` / `PATCH-IDENT fail …`；
退出码：0=绿 / 1=有断言不成立 / 2=样本缺件（外部前提，逐条报出来）。
"""

import json
import re
import subprocess
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "golden_patch" / "identify_golden.json"
SITES = ROOT / "tests" / "golden_patch" / "sites_engine_utf8.txt"
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

MIN_ITEMS = 14

_VERDICT_RE = re.compile(
    r"^PATCH verdict key=(\S+) text=(.*?) thunk_intact=(\d) cave_all_zero=(\d) "
    r"file_size=(\d+) diff_bytes=(\d+)$")
_COUNT_RE = re.compile(
    r"^PATCH count orig=(\d+) patched=(\d+) kept=(\d+) broken=(\d+) other=(\d+) "
    r"context=(\d+) range=(\d+)$")
_ITEM_RE = re.compile(r"^PATCH item id=(\S+) kind=(\S+) offset=(\S+) state=(\S+) ")
_ERROR_RE = re.compile(r"^PATCH error reason=(.*)$")

# 金标准的 state 名 → 计数行里的键（同一个东西的两种叫法）
COUNT_KEYS = ["orig", "patched", "kept", "broken", "other"]
COUNT_EXTRA = {"context-mismatch": "context", "out-of-range": "range"}


def parse_cli_output(text: str):
    """把 CLI 的 PATCH 行解析成 (verdict, counts, items, error)。"""
    verdict = {}
    counts = {}
    items = {}
    error = ""
    for line in text.splitlines():
        line = line.rstrip()
        m = _ERROR_RE.match(line)
        if m:
            error = m.group(1)
            continue
        m = _VERDICT_RE.match(line)
        if m:
            verdict = {"key": m.group(1), "text": m.group(2),
                       "thunk_intact": m.group(3) == "1",
                       "cave_all_zero": m.group(4) == "1",
                       "file_size": int(m.group(5)), "diff_bytes": int(m.group(6))}
            continue
        m = _COUNT_RE.match(line)
        if m:
            keys = COUNT_KEYS + ["context", "range"]
            counts = {k: int(v) for k, v in zip(keys, m.groups())}
            continue
        m = _ITEM_RE.match(line)
        if m:
            items[m.group(1)] = m.group(4)
    return verdict, counts, items, error


def golden_counts_to_cli(counts: dict) -> dict:
    """金标准的 counts（1.x 的 state 名）→ CLI 计数行的键。"""
    out = {k: 0 for k in COUNT_KEYS + ["context", "range"]}
    for name, n in counts.items():
        if name in COUNT_KEYS:
            out[name] = n
        elif name in COUNT_EXTRA:
            out[COUNT_EXTRA[name]] = n
        else:
            out[name] = n  # 未知状态名照原样留着，比较时报出来
    return out


def compare_sample(sample: dict, cli: Path) -> tuple:
    """返回 (问题列表, 比较项数, 是否样本缺件)。"""
    exe = Path(sample["path"])
    if not exe.exists():
        return ([], 0, True)
    run = subprocess.run([str(cli), "--patch-identify", str(exe), str(SITES)],
                         capture_output=True, text=True, encoding="utf-8",
                         errors="replace", timeout=600)
    verdict, counts, items, error = parse_cli_output(run.stdout + run.stderr)
    problems = []
    if error:
        problems.append("CLI 报错：%s" % error)
        return (problems, 0, False)
    if run.returncode != 0:
        problems.append("CLI 退出码 %d（期望 0）" % run.returncode)
    if not verdict:
        problems.append("没解析到 `PATCH verdict` 行（输出形状变了？）")
        return (problems, 0, False)

    compared = 0
    if verdict["key"] != sample["verdict_key"]:
        problems.append("结论不等：金标准 %s，实得 %s"
                        % (sample["verdict_key"], verdict["key"]))
    compared += 1

    want_counts = golden_counts_to_cli(sample["counts"])
    for key in COUNT_KEYS + ["context", "range"]:
        compared += 1
        if counts.get(key) != want_counts.get(key):
            problems.append("计数 %s 不等：金标准 %s，实得 %s"
                            % (key, want_counts.get(key), counts.get(key)))

    for key in ("thunk_intact", "cave_all_zero"):
        compared += 1
        if verdict[key] != bool(sample[key]):
            problems.append("%s 不等：金标准 %s，实得 %s" % (key, sample[key], verdict[key]))

    for item in sample["items"]:
        compared += 1
        got = items.get(item["id"])
        if got is None:
            problems.append("逐项：CLI 没报 `%s` 这一项" % item["id"])
        elif got != item["state"]:
            problems.append("逐项 `%s`：金标准 %s，实得 %s" % (item["id"], item["state"], got))
    if len(items) != len(sample["items"]):
        problems.append("项数不等：金标准 %d，实得 %d" % (len(sample["items"]), len(items)))

    if compared < MIN_ITEMS:
        problems.append("比较项数 %d < 下限 %d（空集/少比不算通过）" % (compared, MIN_ITEMS))
    print("   %s %-10s verdict=%s counts=%s thunk_intact=%s cave_all_zero=%s 比较 %d 项"
          % ("✓" if not problems else "✗", sample["name"], verdict["key"],
             {k: counts.get(k) for k in ("orig", "patched", "kept", "broken", "other")},
             verdict["thunk_intact"], verdict["cave_all_zero"], compared))
    if verdict.get("diff_bytes") is not None:
        print("        参考（**不参与判定**）：diff_bytes=%s / 金标准记 %s"
              % (verdict["diff_bytes"], sample.get("changed_bytes_reference_only")))
    return (problems, compared, False)


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P16-1b 判定半：C++ 侧与 1.x 金标准逐条对拍 ==")
    if not GOLDEN.exists() or not SITES.exists():
        print("PATCH-IDENT fail 仓库自带文件缺失：%s / %s" % (GOLDEN, SITES))
        return 1
    if not cli.exists():
        print("PATCH-IDENT fail 仓库自带产物缺失：%s（先构建）" % cli)
        return 1
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    print("   金标准来源：1.x 插件 sha256=%s…（%d 条站点表）"
          % (golden["provenance"]["onex_plugin_sha256"][:12], golden["provenance"]["sites_count"]))
    print()

    all_problems = []
    total_items = 0
    missing = []
    for sample in golden["samples"]:
        problems, compared, is_missing = compare_sample(sample, cli)
        if is_missing:
            missing.append(sample["name"])
            print("   ⚠ %-10s 样本缺件（外部前提）：%s" % (sample["name"], sample["path"]))
            continue
        total_items += compared
        for p in problems:
            all_problems.append("[%s] %s" % (sample["name"], p))
    print()
    if all_problems:
        for p in all_problems:
            print("   ✗ %s" % p)
        print()
        print("PATCH-IDENT fail %d 项不成立" % len(all_problems))
        return 1
    if missing:
        print("PATCH-IDENT skip 样本缺件：%s（外部前提；release 档按本仓规则处理）"
              % ", ".join(missing))
        return 2
    print("PATCH-IDENT ok samples=%d items=%d（与 1.x 金标准逐条相等；diff_bytes 只作参考）"
          % (len(golden["samples"]), total_items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
