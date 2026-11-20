# -*- coding: utf-8 -*-
"""**真机样本的唯一定位器**（2026-11-09 建立；2026-11-20 改为「主目录 + 三类子目录」）。

口径（用户 2026-11-09 / 2026-11-20）
------------------------------------
· 源码里只允许出现**通用别名**，不许出现真机模组名/绝对路径；
· 配置放 `tests/samples.local.json`（gitignore）或 `XRKIT_SAMPLES_CONFIG` 指向的文件；
· ★ 门禁**不与本机样本绑定**：样本要么给路径，要么在 `skip` 里显式声明跳过；
· ★ 目录结构（用户 2026-11-20）：
      <root>/
        tmp/        每次跑**先清空**，跑完保留（日志与产物供分析）
        samples/    样本侧（输入），按模组/主题细分
        expected/   对照侧（正确结果），与 samples 同构
· ★ 对照分两类：
      deterministic = true   → **逐字节**比对（自动化 + 精准），不一致即失败；
      deterministic = false  → **建议性**比对，不一致只告警。

配置格式
--------
{
  "root": "E:/samples-root",
  "skip": {"golden.real_corpus": "本机没有这套语料"},
  "cases": [
    {"id": "cop-xml", "module": "cop/chronicles",
     "tests": [
       {"id": "stats", "cmd": ["{python}", "tests/check_xml_stats.py", "{case}", "{tmp}"],
        "deterministic": true},
       {"id": "text",  "cmd": ["{python}", "tests/check_xml_text.py", "{case}", "{tmp}"]}
     ]}
  ],
  "flows": [ ... ]            // 兼容旧写法：不套 samples/expected 的裸流程
}

token：`{python}` 解释器、`{root}` 主目录、`{case}` samples/<module>、
       `{expect}` expected/<module>、`{tmp}` tmp/<case>/<test>、`{alias:名}` 旧别名。
"""
from __future__ import annotations

import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = Path(os.environ.get("XRKIT_SAMPLES_CONFIG", str(HERE / "samples.local.json")))

ROOTS: list[Path] = []
ALIASES: dict[str, str] = {}
SKIP: dict[str, str] = {}
FLOWS: list[dict] = []
CASES: list[dict] = []
ROOT: Path | None = None

if os.environ.get("XRKIT_SAMPLES"):
    ROOTS.append(Path(os.environ["XRKIT_SAMPLES"]))

if CONFIG.is_file():
    try:
        data = json.loads(CONFIG.read_text(encoding="utf-8-sig"))
    except Exception:  # noqa: BLE001 - 坏配置按"没配"处理，判据自己会报样本缺失
        data = {}
    if data.get("root"):
        ROOT = Path(str(data["root"]))
        ROOTS.insert(0, ROOT)
    for root in data.get("roots", []) or []:
        if str(root).strip():
            ROOTS.append(Path(str(root)))
    ALIASES = {str(k): str(v) for k, v in (data.get("aliases") or {}).items()}
    raw_skip = data.get("skip") or {}
    if isinstance(raw_skip, list):
        SKIP = {str(k): "" for k in raw_skip}
    elif isinstance(raw_skip, dict):
        SKIP = {str(k): str(v) for k, v in raw_skip.items()}
    for flow in data.get("flows") or []:
        if isinstance(flow, dict) and flow.get("cmd"):
            FLOWS.append(flow)
    for case in data.get("cases") or []:
        if isinstance(case, dict) and case.get("module"):
            CASES.append(case)

if not ROOTS:
    ROOTS = [Path("samples")]


def find(*parts: str) -> Path:
    for root in ROOTS:
        cand = root.joinpath(*parts)
        if cand.exists():
            return cand
    return ROOTS[0].joinpath(*parts)


def find_alias(name: str) -> Path:
    rel = ALIASES.get(name)
    if not rel:
        return ROOTS[0] / name
    path = Path(rel)
    if path.is_absolute():
        return path
    return find(*path.parts)




# ── 跳过声明 ─────────────────────────────────────────────────────────────
def skip_reason(name: str) -> str | None:
    return SKIP.get(name)


def skipped_names() -> list[str]:
    return sorted(SKIP)


def has_alias(name: str) -> bool:
    return name in ALIASES


def flows() -> list[dict]:
    return list(FLOWS)


# ── 主目录布局（2026-11-20）──────────────────────────────────────────────
def base_dir() -> Path:
    """主目录：配置里 `root` 优先，否则第一个根。"""
    return ROOT if ROOT is not None else ROOTS[0]


def tmp_dir() -> Path:
    return base_dir() / "tmp"


def sample_dir(module: str) -> Path:
    return base_dir() / "samples" / module


def expect_dir(module: str) -> Path:
    return base_dir() / "expected" / module


def cases() -> list[dict]:
    return list(CASES)


def case_tests(case: dict) -> list[dict]:
    return [t for t in (case.get("tests") or []) if isinstance(t, dict) and t.get("cmd")]


def is_deterministic(test: dict) -> bool:
    """缺省按**确定性**处理（逐字节）；显式写 false 的走建议性比对。"""
    return bool(test.get("deterministic", True))


def case_tmp(case_id: str, test_id: str) -> Path:
    return tmp_dir() / case_id / test_id


def expand(tokens: list[str], *, module: str = "", case_id: str = "",
           test_id: str = "") -> list[tuple[str, bool]]:
    """替换 token；别名缺失/已跳过时返回 resolved=False（调用方据此跳过）。"""
    out: list[tuple[str, bool]] = []
    for token in tokens:
        text = str(token)
        if text == "{python}":
            import sys

            out.append((sys.executable, True))
        elif text == "{root}":
            out.append((str(base_dir()), True))
        elif text == "{case}":
            out.append((str(sample_dir(module)), True))
        elif text == "{expect}":
            out.append((str(expect_dir(module)), True))
        elif text == "{tmp}":
            out.append((str(case_tmp(case_id, test_id) if case_id and test_id else tmp_dir()), True))
        elif text.startswith("{alias:"):
            name = text[len("{alias:"):].rstrip("}")
            if not has_alias(name) or skip_reason(name) is not None:
                out.append(("", False))
            else:
                out.append((str(find_alias(name)), True))
        else:
            out.append((text, True))
    return out
