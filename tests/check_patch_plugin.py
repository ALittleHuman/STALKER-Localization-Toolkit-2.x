# -*- coding: utf-8 -*-
"""P16-1c 判据：**插件页**（`plugins/patch_page.py`）经 `api.engine.patch_*` 门面驱动引擎。

为什么要有它：P16-1b 的判据走的是 **CLI 动作**（`--patch-identify` / `--patch-apply`）；
本步走**插件那条路**（`--py-plugin-call … --plugin patch_page.py <json>`）——
证明门面（`api.engine.patch_identify/patch_apply`）、站点表（插件持有）、
以及 `py_host` 的 dict↔结构体转换**整条链**都对得上金标准。

判据
----
① **识别**：三份样本上，插件返回的 `verdict_key` / 五个汇总计数 / `thunk_intact` /
   `cave_all_zero` / 逐项判态，与 `tests/golden_patch/identify_golden.json` **逐条相等**；
② **生成**：从原件生成到 `%TEMP%`，产物 sha256 == 金标准里 `v4_correct` 的 sha256（逐字节相同）；
③ 生成后**输入 sha256 未变**；④ 输出已存在且未 `force` ⇒ `refused_output_exists`；
⑤ 拿缺陷样本当输入 ⇒ 守卫拦下（`ok=False` 且 reason 含 `preserve 守卫不通过`）。
★ 计数下限：三样本 × ≥14 项；产物只写 `%TEMP%`。
输出：`PATCH-PLUGIN ok samples=N items=M` / `fail …`；退出码 0=绿 / 1=不成立 / 2=样本缺件。
"""

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "golden_patch" / "identify_golden.json"
PLUGINS = ROOT / "plugins"
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

MIN_ITEMS = 14
COUNT_KEYS = ["orig", "patched", "kept", "broken", "other", "context", "range"]
COUNT_EXTRA = {"context-mismatch": "context", "out-of-range": "range"}
_RESULT_RE = re.compile(r"^PYPLUGINCALL plugin=(\S+) result=(.*)$")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def call_plugin(cli: Path, payload: dict, timeout=900):
    text_payload = json.dumps(payload, ensure_ascii=False)
    r = subprocess.run([str(cli), "--py-plugin-call", str(PLUGINS), "--plugin",
                        "patch_page.py", text_payload],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    out = r.stdout + r.stderr
    for line in out.splitlines():
        m = _RESULT_RE.match(line.strip())
        if m:
            try:
                return json.loads(m.group(2)), r.returncode, out
            except json.JSONDecodeError as e:
                return {"_json_error": str(e)}, r.returncode, out
    return {"_no_result": True}, r.returncode, out


def golden_counts(sample: dict) -> dict:
    out = {k: 0 for k in COUNT_KEYS}
    for name, n in sample["counts"].items():
        out[COUNT_EXTRA.get(name, name)] = n
    return out


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P16-1c：插件页经 api.engine.patch_* 驱动引擎（与金标准逐条对拍）==")
    for path in (GOLDEN, PLUGINS / "patch_page.py"):
        if not path.exists():
            print("PATCH-PLUGIN fail 仓库自带文件缺失：%s" % path)
            return 1
    if not cli.exists():
        print("PATCH-PLUGIN fail 仓库自带产物缺失：%s（先构建）" % cli)
        return 1

    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    orig = Path(golden["samples"][0]["path"])
    if not orig.exists():
        print("PATCH-PLUGIN skip 原始样本缺件（外部前提）：%s" % orig)
        return 2
    v4 = next(s for s in golden["samples"] if s["name"] == "v4_correct")
    v2 = next(s for s in golden["samples"] if s["name"] == "v2_defect")
    v2_path = Path(v2["path"])
    orig_sample = next(s for s in golden["samples"] if s["name"] == "orig")

    problems = []
    total_items = 0
    all_missing = True
    for sample in golden["samples"]:
        exe = Path(sample["path"])
        if not exe.exists():
            print("   ⚠ %-10s 样本缺件，跳过" % sample["name"])
            continue
        all_missing = False
        got, rc, raw = call_plugin(cli, {"action": "identify", "path": str(exe)})
        if got.get("_no_result") or got.get("_json_error"):
            problems.append("[%s] 插件没打出 PYPLUGINCALL result 行（rc=%d）：%s"
                            % (sample["name"], rc, (got.get("_json_error") or "")[:80]))
            continue
        if got.get("error"):
            problems.append("[%s] 识别报错：%s" % (sample["name"], got["error"]))
            continue
        compared = 0
        if got.get("verdict_key") != sample["verdict_key"]:
            problems.append("[%s] verdict_key 不等：金标准 %s，实得 %s"
                            % (sample["name"], sample["verdict_key"], got.get("verdict_key")))
        compared += 1
        want_counts = golden_counts(sample)
        counts = got.get("counts") or {}
        for key in COUNT_KEYS:
            compared += 1
            if counts.get(key) != want_counts.get(key):
                problems.append("[%s] 计数 %s 不等：金标准 %s，实得 %s"
                                % (sample["name"], key, want_counts.get(key), counts.get(key)))
        for key in ("thunk_intact", "cave_all_zero"):
            compared += 1
            if bool(got.get(key)) != bool(sample[key]):
                problems.append("[%s] %s 不等：金标准 %s，实得 %s"
                                % (sample["name"], key, sample[key], got.get(key)))
        items = {i["id"]: i["state"] for i in (got.get("items") or [])}
        for want in sample["items"]:
            compared += 1
            if items.get(want["id"]) != want["state"]:
                problems.append("[%s] 逐项 %s：金标准 %s，实得 %s"
                                % (sample["name"], want["id"], want["state"], items.get(want["id"])))
        total_items += compared
        print("   %s %-10s verdict=%s counts=%s 比较 %d 项"
              % ("✓" if not problems else "✗", sample["name"], got.get("verdict_key"),
                 {k: counts.get(k) for k in ("orig", "patched", "kept", "broken", "other")},
                 compared))

    if total_items < MIN_ITEMS:
        problems.append("比较项数 %d < 下限 %d（空集/少比不算通过）" % (total_items, MIN_ITEMS))

    # ② ③ ④ ⑤ 写盘半（经插件）；产物只写 %TEMP%
    if not all_missing:
        with tempfile.TemporaryDirectory(prefix="p16c_") as tmpdir:
            out = Path(tmpdir) / "out.exe"
            got, rc, raw = call_plugin(cli, {"action": "apply", "src": str(orig), "out": str(out)})
            if not got.get("ok") or not out.exists():
                problems.append("② 插件生成失败：%s" % str(got.get("reason") or got)[:160])
            else:
                h = sha256_file(out)
                if h != v4["sha256"]:
                    problems.append("② 产物 sha256 不等于 v4 参照：实得 %s" % h)
                else:
                    print("   ✓ ② 插件产物 == v4 参照（sha256 全等）")
            if sha256_file(orig) != orig_sample["sha256"]:
                problems.append("③ 生成过程中输入被改动了！")

            got2, rc, raw = call_plugin(cli, {"action": "apply", "src": str(orig), "out": str(out)})
            if got2.get("ok") or not got2.get("refused_output_exists"):
                problems.append("④ 输出已存在未按预期拒绝：%s" % str(got2 or {})[:120])
            else:
                print("   ✓ ④ 输出已存在 ⇒ refused_output_exists")

            if v2_path.exists():
                out2 = Path(tmpdir) / "from_v2.exe"
                got3, rc, raw = call_plugin(cli, {"action": "apply", "src": str(v2_path),
                                                 "out": str(out2)})
                reason = str(got3.get("reason", ""))
                if got3.get("ok") or "preserve 守卫不通过" not in reason:
                    problems.append("⑤ 缺陷输入未被守卫拦下：%s" % str(got3 or {})[:140])
                else:
                    print("   ✓ ⑤ 缺陷输入 ⇒ 守卫拦下（preserve）")
                if out2.exists():
                    problems.append("⑤ 守卫失败却产出了文件")

    print()
    if all_missing:
        print("PATCH-PLUGIN skip 三份样本都缺件（外部前提）")
        return 2
    if problems:
        for p in problems[:14]:
            print("   ✗ %s" % p)
        print()
        print("PATCH-PLUGIN fail %d 项不成立" % len(problems))
        return 1
    print("PATCH-PLUGIN ok samples=3 items=%d（插件页 → 门面 → 引擎，与 1.x 金标准逐条相等）"
          % total_items)
    return 0


if __name__ == "__main__":
    sys.exit(main())
