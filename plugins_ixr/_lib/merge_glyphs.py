#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""A-min：**字形级**合并（只补缺字，不动老字形）。

与 `fontTools.merge` 的区别（那是"合并两套字体"）：这里把**源字体缺的字形逐个拷进
target**，并且：

  · **只新增**：target 里已有的 glyph（glyf 数据与 hmtx 度量）一个字节都不改；
  · **复合字形递归拷贝**：组件也必须拷进来，且 `component.glyphName` 要重映射到
    target 里的新名字（照抄源 index/名字 = 指向错误字形，这是最容易糊过去的一条）；
  · **cmap 可达**：每个新码位都写进所有 Unicode cmap 子表（format 4/12 由 fontTools 维护）；
  · **hmtx 随字形拷**：advance/lsb 按 upem 比例缩放后写入；
  · **head.modified 固定**：保证两次运行 sha256 相同（幂等）。
  · **CFF/OTF 轮廓拒绝**：本工具只处理 `glyf`（TrueType）；源或 target 是 CFF 就明确报错。

用法（独立入口，便于判据直接对拍）：
  python merge_glyphs.py --target <target.ttf> --source <source.ttf> \
      --charset <chars.txt> --out <out.ttf> [--lang rus]

  chars.txt：每行一个码位，`U+4E00` 或 `4E00` 或十进制 `19968` 都可以。
  报告：最后一行 `MERGEGLYPHS {json}`（判据按 JSON 读，不数子串）。
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

# ★ 2026-11-06：发行包里的 python 有 `._pth`（隔离模式，不读 PYTHONPATH）⇒ 本脚本自己把
#   vendored fontTools 插进 sys.path（与 ixr_run.py 同一约定：../fontlib 或 ./fontlib）。
import sys as _sys
from pathlib import Path as _Path

_here = _Path(__file__).resolve().parent
for _cand in (_here, _here / "fontlib", _here.parent / "fontlib"):
    if (_cand / "fontTools").is_dir() and str(_cand) not in _sys.path:
        _sys.path.insert(0, str(_cand))

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph  # noqa: F401  (类型参考)

# 固定时间戳：2020-01-01T00:00:00Z。head.modified 固定 ⇒ 幂等可复现。
FIXED_MODIFIED = 3786825600


def log(msg: str) -> None:
    print(msg, flush=True)


def parse_charset(path: Path) -> list[int]:
    cps: list[int] = []
    seen: set[int] = set()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = raw.strip().lstrip("\ufeff").strip()
        if not s or s.startswith("#"):
            continue
        try:
            if s.upper().startswith("U+"):
                cp = int(s[2:], 16)
            elif all(c in "0123456789abcdefABCDEF" for c in s) and len(s) >= 4:
                cp = int(s, 16)
            else:
                cp = int(s, 10)
        except ValueError:
            continue
        if cp not in seen:
            seen.add(cp)
            cps.append(cp)
    return cps


def _require_glyf(font: TTFont, role: str) -> None:
    if "glyf" not in font:
        raise SystemExit("MERGEGLYPHS error reason=%s 不是 TrueType(glyf) 轮廓（CFF/OTF 不支持）"
                         % role)


def _copy_glyph(dst: TTFont, src: TTFont, src_name: str, new_name: str,
                order: list[str], order_set: set[str], copied: dict[str, str],
                scale: float) -> None:
    """把源字形（含复合依赖）拷进 target，组件名重映射到 target 内的新名字。"""
    if src_name in copied:
        return
    sglyf = src["glyf"]
    dglyf = dst["glyf"]
    # ★ 2026-11-06（实测缺陷 ①）：必须**深拷贝** —— 原先是源字体的同一个对象，
    #   下面 `comp.glyphName = …` / `comp.x = …` 是**原地改源对象**，任何"复用已加载源"
    #   的重构都会立刻串字体。
    glyph = copy.deepcopy(sglyf[src_name])
    # 先递归拷组件（复合字形），再写自己 —— 否则组件名指向的是源字体里不存在的名字
    if glyph.isComposite():
        for comp in glyph.components:
            dep = comp.glyphName
            if dep not in copied:
                dep_name = _new_name(dep, order_set, copied)
                _copy_glyph(dst, src, dep, dep_name, order, order_set, copied, scale)
        for comp in glyph.components:
            comp.glyphName = copied[comp.glyphName]
            if scale != 1.0:
                # ★ 2026-11-06（实测缺陷 ①）：组件**偏移**与 2×2 变换也必须按 upem 缩放，
                #   否则跨 upem（如 2048 → 1000）时附加符会飞出去（Adieresis 的两点浮到右上）。
                comp.x = int(round(comp.x * scale))
                comp.y = int(round(comp.y * scale))
                if hasattr(comp, "transform") and comp.transform is not None:
                    a, b, c, d = comp.transform
                    comp.transform = (a * scale, b * scale, c * scale, d * scale)
    dglyf.glyphs[new_name] = glyph
    order.append(new_name)
    order_set.add(new_name)
    copied[src_name] = new_name
    # ★ 每个拷进来的字形（含复合依赖）都必须有 hmtx 条目，否则 hmtx.compile 会报
    #   "not enough 'hmtx' table data"（字体根本写不出来）
    adv, lsb = src["hmtx"][src_name]
    dst["hmtx"].metrics[new_name] = (int(round(adv * scale)), int(round(lsb * scale)))


def _new_name(src_name: str, order_set: set[str], copied: dict[str, str]) -> str:
    name = src_name
    while name in order_set:
        name += ".agm"
    return name


def merge(target_path: Path, source_path: Path, cps: list[int], out_path: Path) -> dict:
    dst = TTFont(str(target_path), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
    src = TTFont(str(source_path), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
    _require_glyf(dst, "target 字体")
    _require_glyf(src, "源字体")

    dst_cmap = dst.getBestCmap() or {}
    src_cmap = src.getBestCmap() or {}
    dst_upem = int(dst["head"].unitsPerEm)
    src_upem = int(src["head"].unitsPerEm)
    scale = dst_upem / src_upem if src_upem else 1.0

    order = list(dst.getGlyphOrder())
    order_set = set(order)
    copied: dict[str, str] = {}
    added: dict[int, str] = {}
    skipped_have: list[int] = []
    missing_src: list[int] = []
    before = len(order)

    for cp in cps:
        if cp in dst_cmap:
            skipped_have.append(cp)
            continue
        src_name = src_cmap.get(cp)
        if src_name is None:
            missing_src.append(cp)
            continue
        if src_name not in copied:
            _copy_glyph(dst, src, src_name, _new_name(src_name, order_set, copied), order,
                        order_set, copied, scale)
        added[cp] = copied[src_name]

    if added:
        dst.setGlyphOrder(order)
        dst["maxp"].numGlyphs = len(order)
        for sub in dst["cmap"].tables:
            if not sub.isUnicode():
                continue
            for cp, name in added.items():
                # ★ 2026-11-06（实测缺陷 ②）：format 4 的码位上界是 0xFFFF ——
                #   往 format 4 里写星平面码位（CJK 扩展 B / emoji）会 OverflowError，
                #   整次合并直接崩。星平面只许进 format 12/13。
                if sub.format == 4 and cp > 0xFFFF:
                    continue
                if sub.format in (0, 6) and cp > 0xFF:
                    continue
                sub.cmap[cp] = name
        # hmtx：新字形的度量已在 _copy_glyph 里写入（老字形一个都没动）
        # head.modified 固定 ⇒ 幂等
        dst["head"].modified = FIXED_MODIFIED

    out_path.parent.mkdir(parents=True, exist_ok=True)
    if added:
        # ★ 2026-11-06（硬化）：先写临时文件再 rename —— 中途失败不会把包内字体写坏。
        tmp_out = out_path.with_suffix(out_path.suffix + ".tmp")
        dst.save(str(tmp_out))
        # 重新读一次，确保 head.modified 是固定值（fontTools 某些路径会写当前时间）
        check = TTFont(str(tmp_out), fontNumber=0, recalcBBoxes=False, recalcTimestamp=False)
        if int(check["head"].modified) != FIXED_MODIFIED:
            check["head"].modified = FIXED_MODIFIED
            check.save(str(tmp_out))
        if out_path.exists():
            out_path.unlink()
        tmp_out.replace(out_path)
    else:
        # 没有可补的字形 ⇒ **原样复制**（不做 save 往返：否则字节会变，'只增不改' 就不成立了）
        out_path.write_bytes(Path(target_path).read_bytes())

    return {
        "ok": True,
        "target": str(target_path),
        "source": str(source_path),
        "out": str(out_path),
        "numGlyphs_before": before,
        "numGlyphs_after": len(order),
        "added": len(added),
        "copied_glyphs": len(copied),
        "skipped_present": len(skipped_have),
        "missing_in_source": len(missing_src),
        "missing_cps": ["U+%04X" % cp for cp in missing_src[:32]],
        "scale": round(scale, 6),
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--target", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--charset", required=True)
    ap.add_argument("--out", required=True)
    try:
        args, _unknown = ap.parse_known_args(argv)
    except SystemExit:
        log("MERGEGLYPHS error reason=用法：--target <ttf> --source <ttf> --charset <txt> --out <ttf>")
        return 2
    try:
        report = merge(Path(args.target), Path(args.source), parse_charset(Path(args.charset)),
                       Path(args.out))
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001
        log("MERGEGLYPHS error reason=%s" % (exc,))
        return 1
    log("MERGEGLYPHS " + json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
