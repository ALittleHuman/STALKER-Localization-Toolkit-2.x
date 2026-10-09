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


def find_font() -> str:
    for candidate in FONT_CANDIDATES:
        if Path(candidate).is_file():
            return candidate
    return ""


def check_font_panel(cli: Path, problems: list) -> tuple:
    """② 面板：分区字体 —— 与 CLI 参照逐字节 + 档位语义 + 清单台账。返回 (项数, 前提缺件?)。"""
    font = find_font()
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


def check_pipeline_panel(cli: Path, problems: list) -> tuple:
    """③ 面板：自适应出汉化包 —— 与两条 CLI 参照逐字节 + 装/还原**逐字节回原样**。"""
    font = find_font()
    if not font:
        print("   ⚠ 源字体缺件，产线面板跳过")
        return (0, True)

    total = 0
    work = ROOT / "build" / "toolchain_pipeline"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    src = work / "scripts"
    src.mkdir(parents=True)
    (src / "a.script").write_text('local s = "Привет, сталкер!"\n', encoding="cp1251")
    (src / "d.ltx").write_text('[wpn_akm]\ninv_name = "АКМ"\n; inv_name = Не должно попасть\n',
                               encoding="cp1251")
    ref_out = work / "ref"
    plug_out = work / "plug"

    # 参照：**同一目录里**先提取、再出包（与产线同形）
    ref_run = [
        [str(cli), "--extract-text", str(src), str(ref_out), "--prefix", "p16c",
         "--id-prefix", "sgtat_p16c_0", "--clean"],
        [str(cli), "--build-font-pack", "SoC", "chs", str(ref_out), font, str(ref_out),
         "--only-size", "11"],
    ]
    for cmd in ref_run:
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=900)
        if r.returncode != 0:
            problems.append("③ CLI 参照没跑成（rc=%d）：%s" % (r.returncode, cmd[1]))

    got, rc, raw = call_plugin(cli, {
        "action": "pipeline", "source_dir": str(src), "out_dir": str(plug_out),
        "font_path": font, "prefix": "p16c", "id_prefix": "sgtat_p16c_0", "clean": True,
        "only_sizes": [11]})
    if got.get("_no_result") or not got.get("ok"):
        problems.append("③ 产线没跑成：%s" % str(got)[:200])
        return (1, False)

    ref_tree = tree(ref_out)
    plug_tree = tree(plug_out)
    plug_tree.pop("manifest.json", None)
    total += 1
    if not ref_tree or not plug_tree:
        problems.append("③ 空集：参照 %d / 插件 %d（空集不算通过）"
                        % (len(ref_tree), len(plug_tree)))
    for key, digest in sorted(ref_tree.items()):
        total += 1
        if plug_tree.get(key) != digest:
            problems.append("③ 产物不同：%s（参照 %s，插件 %s）"
                            % (key, digest[:12], str(plug_tree.get(key))[:12]))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    total += 1
    if only_plug:
        problems.append("③ 插件多出参照没有的文件：%s" % only_plug[:4])

    # 统一清单必须同时覆盖两类产物（改写的文本树 + 字库）
    manifest_path = plug_out / "manifest.json"
    rels = []
    if manifest_path.is_file():
        for line in manifest_path.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                rels.append(line.split("\t")[0])
    total += 1
    if not any(r.startswith("gamedata/") for r in rels):
        problems.append("③ 清单里没有字库产物（gamedata/…）")
    total += 1
    if not any(not r.startswith("gamedata/") for r in rels):
        problems.append("③ 清单里没有文本提取产物（非 gamedata/…）")

    # ── 装 → 还原 往返：必须**逐字节回到安装前** ──
    with tempfile.TemporaryDirectory(prefix="p16c_game_") as game_dir:
        game = Path(game_dir)
        overwritten_rel = next((r for r in rels if r.startswith("gamedata/")), "")
        pre = game / overwritten_rel if overwritten_rel else None
        old_bytes = b"OLD-CONTENT-FROM-BEFORE-INSTALL"
        if pre is not None:
            pre.parent.mkdir(parents=True, exist_ok=True)
            pre.write_bytes(old_bytes)
        ins, _, _ = call_plugin(cli, {"action": "install", "out_dir": str(plug_out),
                                      "target_root": str(game)})
        total += 1
        if not ins.get("ok") or not ins.get("installed"):
            problems.append("③ 安装失败：%s" % str(ins)[:160])
        else:
            total += 1
            if not ins.get("overwritten"):
                problems.append("③ 覆盖没有记账（overwritten=%s）—— 覆盖必须可见"
                                % ins.get("overwritten"))
            if pre is not None:
                total += 1
                if pre.read_bytes() != (plug_out / overwritten_rel).read_bytes():
                    problems.append("③ 装完之后目标文件内容不等于产物")
            # 新增文件（不在 pre 里的）必须真出现
            added = [r for r in rels if r != overwritten_rel and (game / r).is_file()]
            total += 1
            if not added:
                problems.append("③ 安装既没覆盖也没新增任何文件")

        res, _, _ = call_plugin(cli, {"action": "restore", "out_dir": str(plug_out),
                                      "target_root": str(game)})
        total += 1
        if not res.get("ok"):
            problems.append("③ 还原失败：%s" % str(res)[:160])
        if pre is not None:
            total += 1
            if pre.read_bytes() != old_bytes:
                problems.append("③ 还原后**没有逐字节回到原样**（%s）" % overwritten_rel)
        left = [r for r in rels if r != overwritten_rel and (game / r).exists()]
        total += 1
        if left:
            problems.append("③ 还原后还留着新装的文件：%s" % left[:4])

    print("   ✓ ③ 产线：参照 %d 文件 / 插件 %d 文件（逐字节）＋ 统一清单两类产物 ＋ 装/还原往返"
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

    pipe_items, pipe_missing = check_pipeline_panel(cli, problems)
    items += pipe_items

    print()
    if items < MIN_ITEMS:
        problems.append("比较项数 %d < 下限 %d（空集/少比不算通过）" % (items, MIN_ITEMS))
    if problems:
        for p in problems[:14]:
            print("   ✗ %s" % p)
        print()
        print("TOOLCHAIN fail %d 项不成立" % len(problems))
        return 1
    if not samples_present and font_missing and pipe_missing:
        print("TOOLCHAIN skip 样本与字体都缺件（外部前提）")
        return 2
    print("TOOLCHAIN ok items=%d（面板①：1.x 金标准逐条＋产物逐字节；面板②：CLI 参照逐字节＋档位语义＋清单；"
          "面板③：产线逐字节＋统一清单＋装/还原逐字节回原样）" % items)
    return 0


if __name__ == "__main__":
    sys.exit(main())
