# -*- coding: utf-8 -*-
"""P16-1c 判据：**插件页**（`plugins/toolchain_page.py`）经 `api.engine.*` 门面驱动引擎。

覆盖两个面板：
  ① **自动二进制补丁**（`patch_identify` / `patch_apply`）—— 与 1.x 金标准逐条对拍 + 产物逐字节；
  ② **分区字体**（`font_pack` + `pkg_write_manifest`）—— 与 CLI 参照 `--build-font-pack`
     **逐字节对拍** + **档位语义**（只出所选档，别的档一个都不许出现）+ **清单台账**逐条核对。

为什么走插件这条路：P16-1b 的两步走的是 **CLI 动作**；本步走
`--py-plugin-call … --plugin toolchain_page.py <json>`，证明「门面 + 站点表（插件持有）+
py_host 的 dict↔结构体转换 + 清单写入」**整条链**都对，而不是只有 CLI 那条路对。

★ 所有产物只写 `%TEMP%` 与 `build/`（绝不在仓库里留产物）。
输出：`TOOLCHAIN ok …` / `fail …`；退出码 0=绿 / 1=不成立 / 2=外部前提缺件（样本/字体）。
"""

import hashlib
import json
import os
import re
import shutil
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
PAGE = "toolchain_page.py"
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

MIN_ITEMS = 14
COUNT_KEYS = ["orig", "patched", "kept", "broken", "other", "context", "range"]
COUNT_EXTRA = {"context-mismatch": "context", "out-of-range": "range"}
_RESULT_RE = re.compile(r"^PYPLUGINCALL plugin=(\S+) result=(.*)$")
FONT_CANDIDATES = (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\msyh.ttf",
                   r"C:\Windows\Fonts\simsun.ttc")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def call_plugin(cli: Path, payload: dict, timeout=900):
    cmd = [str(cli), "--py-plugin-call", str(PLUGINS), "--plugin", PAGE,
           json.dumps(payload, ensure_ascii=False)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
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


def tree(root: Path) -> dict:
    out = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            out[str(path.relative_to(root)).replace("\\", "/")] = sha256_file(path)
    return out


def check_patch_panel(cli: Path, problems: list) -> int:
    """① 面板：三样本识别 + 产物逐字节 + 两条拒绝路径。返回比较项数。"""
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    orig = Path(golden["samples"][0]["path"])
    if not orig.exists():
        return 0
    v4 = next(s for s in golden["samples"] if s["name"] == "v4_correct")
    v2 = next(s for s in golden["samples"] if s["name"] == "v2_defect")
    orig_sample = next(s for s in golden["samples"] if s["name"] == "orig")
    total = 0
    for sample in golden["samples"]:
        exe = Path(sample["path"])
        if not exe.exists():
            print("   ⚠ %-10s 样本缺件，跳过" % sample["name"])
            continue
        got, rc, raw = call_plugin(cli, {"action": "identify", "path": str(exe)})
        if got.get("_no_result") or got.get("_json_error"):
            problems.append("[%s] 插件没打出 result 行（rc=%d）" % (sample["name"], rc))
            continue
        if got.get("error"):
            problems.append("[%s] 识别报错：%s" % (sample["name"], got["error"]))
            continue
        if got.get("verdict_key") != sample["verdict_key"]:
            problems.append("[%s] verdict_key 不等：金标准 %s，实得 %s"
                            % (sample["name"], sample["verdict_key"], got.get("verdict_key")))
        total += 1
        want_counts = golden_counts(sample)
        counts = got.get("counts") or {}
        for key in COUNT_KEYS:
            total += 1
            if counts.get(key) != want_counts.get(key):
                problems.append("[%s] 计数 %s 不等：金标准 %s，实得 %s"
                                % (sample["name"], key, want_counts.get(key), counts.get(key)))
        for key in ("thunk_intact", "cave_all_zero"):
            total += 1
            if bool(got.get(key)) != bool(sample[key]):
                problems.append("[%s] %s 不等：金标准 %s，实得 %s"
                                % (sample["name"], key, sample[key], got.get(key)))
        items = {i["id"]: i["state"] for i in (got.get("items") or [])}
        for want in sample["items"]:
            total += 1
            if items.get(want["id"]) != want["state"]:
                problems.append("[%s] 逐项 %s：金标准 %s，实得 %s"
                                % (sample["name"], want["id"], want["state"], items.get(want["id"])))
        print("   ✓ %-10s verdict=%s 比较累计 %d 项" % (sample["name"], got.get("verdict_key"), total))

    with tempfile.TemporaryDirectory(prefix="p16c_tc_") as tmpdir:
        out = Path(tmpdir) / "out.exe"
        got, rc, raw = call_plugin(cli, {"action": "apply", "src": str(orig), "out": str(out)})
        total += 1
        if not got.get("ok") or not out.exists():
            problems.append("① 插件生成失败：%s" % str(got.get("reason") or got)[:160])
        elif sha256_file(out) != v4["sha256"]:
            problems.append("① 产物 sha256 不等于 v4 参照：%s" % sha256_file(out))
        else:
            print("   ✓ ① 产物 == v4 参照（sha256 全等）")
        total += 1
        if sha256_file(orig) != orig_sample["sha256"]:
            problems.append("① 生成过程中输入被改动了！")
        got2, _, _ = call_plugin(cli, {"action": "apply", "src": str(orig), "out": str(out)})
        total += 1
        if got2.get("ok") or not got2.get("refused_output_exists"):
            problems.append("① 输出已存在未按预期拒绝：%s" % str(got2)[:120])
        if v2["path"] and Path(v2["path"]).exists():
            out2 = Path(tmpdir) / "from_v2.exe"
            got3, _, _ = call_plugin(cli, {"action": "apply", "src": v2["path"], "out": str(out2)})
            total += 1
            reason = str(got3.get("reason", ""))
            if got3.get("ok") or "preserve 守卫不通过" not in reason:
                problems.append("① 缺陷输入未被守卫拦下：%s" % str(got3)[:140])
            if out2.exists():
                problems.append("① 守卫失败却产出了文件")
    return total


def check_font_panel(cli: Path, problems: list) -> tuple:
    """② 面板：分区字体 —— 与 CLI 参照逐字节 + 档位语义 + 清单台账。返回 (项数, 前提缺件?)。"""
    font = ""
    for candidate in FONT_CANDIDATES:
        if Path(candidate).is_file():
            font = candidate
            break
    if not font:
        print("   ⚠ 源字体缺件（msyh/simsun），分区字体面板跳过")
        return (0, True)

    total = 0
    work = ROOT / "build" / "toolchain_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    xml_dir = work / "xml_in"
    xml_dir.mkdir(parents=True)
    (xml_dir / "a.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '\t<string id="a"><text>合成语料：汉字与西里尔 Привет，标点！？</text></string>\n'
        '</string_table>\n', encoding="utf-8")
    (xml_dir / "b.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '\t<string id="b"><text>第二条：换行与标点。？——</text></string>\n'
        '</string_table>\n', encoding="utf-8")

    ref_out = work / "ref"
    plug_out = work / "plug"
    ref = subprocess.run([str(cli), "--build-font-pack", "SoC", "chs", str(xml_dir), font,
                          str(ref_out), "--only-size", "11"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace",
                         timeout=900)
    payload = {"action": "font_regions", "game": "SoC", "lang": "chs", "xml_dir": str(xml_dir),
               "font_path": font, "out_dir": str(plug_out), "only_sizes": [11]}
    got, rc, raw = call_plugin(cli, payload)
    if got.get("_no_result") or got.get("_json_error") or not got.get("ok"):
        problems.append("② 分区字体出包失败：%s" % str(got)[:200])
        return (1, False)

    ref_tree, plug_tree = tree(ref_out), tree(plug_out)
    # 清单自身不算产物
    plug_tree = {k: v for k, v in plug_tree.items() if k != "manifest.json"}
    total += 1
    if not ref_tree or not plug_tree:
        problems.append("② 空集：参照 %d 个文件 / 插件 %d 个（空集不算通过）"
                        % (len(ref_tree), len(plug_tree)))
    for key, digest in sorted(ref_tree.items()):
        total += 1
        if plug_tree.get(key) != digest:
            problems.append("② 产物不同：%s（参照 %s，插件 %s）"
                            % (key, digest[:12], str(plug_tree.get(key))[:12]))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    total += 1
    if only_plug:
        problems.append("② 插件多出参照没有的文件：%s" % only_plug[:4])
    # 档位语义：只出所选档（文件名里的 ui_font_<档>）
    others = [k for k in plug_tree if "ui_font_" in k and "ui_font_11" not in k]
    total += 1
    if others:
        problems.append("② 档位语义不对：只选 11 档，却出现了别的档：%s" % others[:4])
    if not any("ui_font_11" in k for k in plug_tree):
        problems.append("② 产物里没有 11 档的字库（档位语义的另一面）")

    # 清单台账（★真实形状＝TSV 文本：`# xrkit package manifest v1` + 每行 `rel\tsha256\tsource`）
    manifest_path = plug_out / "manifest.json"
    total += 1
    if not manifest_path.is_file():
        problems.append("② 清单没写成：%s" % manifest_path)
    else:
        raw_lines = manifest_path.read_text(encoding="utf-8").splitlines()
        rows = [ln.split("\t") for ln in raw_lines if ln.strip() and not ln.startswith("#")]
        if not rows:
            problems.append("② 清单里一条都没有（首行应是 `# xrkit package manifest v1`）")
        else:
            total += 1
            if len(rows) != len(plug_tree):
                problems.append("② 清单条数 %d != 产物文件数 %d" % (len(rows), len(plug_tree)))
            rel, digest = rows[0][0], rows[0][1]
            total += 1
            if plug_tree.get(rel) != digest:
                problems.append("② 清单里 %s 的 sha256 与实算不符（清单 %s，实算 %s）"
                                % (rel, digest[:12], str(plug_tree.get(rel))[:12]))
    if ref.returncode != 0:
        problems.append("② CLI 参照 rc=%d（参照本身没跑成）" % ref.returncode)
    print("   ✓ ② 分区字体：参照 %d 文件 / 插件 %d 文件（逐字节）＋ 档位语义 ＋ 清单台账"
          % (len(ref_tree), len(plug_tree)))
    return (total, False)


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P16-1c：插件页（汉化产线）经 api.engine.* 驱动引擎 ==")
    for path in (GOLDEN, PLUGINS / PAGE):
        if not path.exists():
            print("TOOLCHAIN fail 仓库自带文件缺失：%s" % path)
            return 1
    if not cli.exists():
        print("TOOLCHAIN fail 仓库自带产物缺失：%s（先构建）" % cli)
        return 1

    problems = []
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    samples_present = any(Path(s["path"]).exists() for s in golden["samples"])

    items = check_patch_panel(cli, problems) if samples_present else 0
    if not samples_present:
        print("   ⚠ 二进制补丁三样本全缺件（外部前提），面板① 跳过")

    font_items, font_missing = check_font_panel(cli, problems)
    items += font_items

    print()
    if items < MIN_ITEMS:
        problems.append("比较项数 %d < 下限 %d（空集/少比不算通过）" % (items, MIN_ITEMS))
    if problems:
        for p in problems[:14]:
            print("   ✗ %s" % p)
        print()
        print("TOOLCHAIN fail %d 项不成立" % len(problems))
        return 1
    if not samples_present and font_missing:
        print("TOOLCHAIN skip 样本与字体都缺件（外部前提）")
        return 2
    print("TOOLCHAIN ok items=%d（面板①：与 1.x 金标准逐条相等＋产物逐字节；"
          "面板②：与 CLI 参照逐字节＋档位语义＋清单台账）" % items)
    return 0


if __name__ == "__main__":
    sys.exit(main())
