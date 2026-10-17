# -*- coding: utf-8 -*-
r"""判据：**截断信号**必须一路透到 Python（复审计 09 分册 ③）。

链条：`fs::loose_entries` → `EngineFacade::fs_loose_entries` → `py_host` 的 dict → 插件侧。

★ 为什么必须**双向**（本判据的关键，也是我这几轮反复栽的地方）：
  只测"深目录 ⇒ truncated=True"是**不完整的**——一个恒返回 True 的实现也能过。
  ⇒ 同时测"浅目录 ⇒ truncated=False"，两头都要成立，这条判据才有牙。

★ 夹具为什么会真的截断：引擎的深度上限是 `kMaxDepth = 32`（`overlay.cpp:482`），
  造 35 层嵌套必然到顶 ⇒ `LooseInfo.truncated = true`。
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PLUGINS = ROOT / "tests" / "python_plugins_trunc"
PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ " + label + (("（%s）" % detail) if detail else ""))
        return True
    print("   ✗ " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def probe(root: Path) -> dict:
    args = json.dumps({"action": "probe", "root": str(root)})
    p = subprocess.run([str(CLI), "--py-plugin-call", str(PLUGINS), "probe_truncation.py", args],
                       capture_output=True, timeout=600)
    txt = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
    for line in txt.splitlines():
        if " result=" in line:
            try:
                return json.loads(line.split(" result=", 1)[1])
            except Exception:
                pass
    return {"_raw": txt.strip()[-400:]}


def main() -> int:
    print("== 截断链路：引擎 → 门面 → py_host → Python（P17-134）==")
    if not CLI.is_file():
        print("TRUNC skip 缺 CLI 构建产物")
        return 2
    with tempfile.TemporaryDirectory(prefix="trunc_") as tmp:
        base = Path(tmp)

        # ① 深目录（35 层 > kMaxDepth=32）⇒ 必须截断
        deep = base / "deep"
        cur = deep
        for i in range(35):
            cur = cur / ("d%02d" % i)
        cur.mkdir(parents=True)
        (cur / "leaf.txt").write_text("x", encoding="utf-8")
        got_deep = probe(deep)
        want(got_deep.get("available") is True, "① 门面可用", str(got_deep)[:90])
        want(got_deep.get("has_key") is True,
             "① dict 里有 `truncated` 键（修前**没有** ⇒ 信号丢在门面/Python）",
             "keys 缺失" if not got_deep.get("has_key") else "")
        want(got_deep.get("truncated") is True, "① 深目录 ⇒ truncated=True",
             "count=%s" % got_deep.get("count"))
        want(bool(got_deep.get("reason")), "① 截断**理由**非空（给人看，不解析它）",
             str(got_deep.get("reason"))[:70])
        want(got_deep.get("error", "?") == "",
             "① 截断**不是错误**：`error` 必须为空（修前它被写进 error ⇒ 被当失败）",
             "error=%r" % got_deep.get("error"))

        # ② 浅目录 ⇒ 反向：不许截断（防"恒 True"的假绿）
        shallow = base / "shallow"
        (shallow / "sub").mkdir(parents=True)
        (shallow / "sub" / "a.txt").write_text("x", encoding="utf-8")
        got_shallow = probe(shallow)
        want(got_shallow.get("truncated") is False,
             "② 浅目录 ⇒ truncated=False（**反向格**：防「恒 True」的实现蒙混）",
             str(got_shallow.get("truncated")))
        want(got_shallow.get("count", 0) >= 1, "② 浅目录仍应读到条目",
             "count=%s" % got_shallow.get("count"))

    print("-" * 66)
    if PROBLEMS:
        print("TRUNC-CHAIN fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("TRUNC-CHAIN ok %d 项（截断双向：深⇒真、浅⇒假；reason 有、error 空；复审计 09 分册 ③）"
          % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
