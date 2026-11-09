#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本**提取层**黄金语料：整棵产物树逐字节对拍（2.x C++ vs 1.x `text_extract_app`）。

判据：同一份源目录、同一个固定 id_prefix，双方产出的
  ① 改写后的源文件树（每个文件的字节）② 汇总的文本 XML
**逐字节相同**（按 rel 路径逐个比 sha256）。

为什么必须比"整棵树"而不是"条数"：1.x 这条链上有三件事会改字节 ——
  · 正文按**检测到的编码**写回（utf-8-sig 会补 BOM、cp1251 严格编码）；
  · Python 文本模式**写**会把 \n 翻成 \r\n（Windows）；
  · XML 规范化 / 实体转义 / 1000 字符切行。
只看条数或只看文本 XML，这三件全都照不出来。

    python tests/golden_text_extract.py --write    # 用 1.x 生成基线（只记 sha256）
    python tests/golden_text_extract.py            # 对拍（跑 2.x 的 --extract-text）
    python tests/golden_text_extract.py --id synth # 只跑某个夹具
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BASELINE = HERE / "golden" / "text_extract.json"
WORK = ROOT / "build" / "golden_text_extract"
# ★ 2026-11-09（用户口径）：1.x 参照路径也走样本别名（工作区配置），源码不写死。
sys.path.insert(0, str(Path(__file__).resolve().parent))
import samples  # noqa: E402

TOOLKIT_1X = samples.find_alias("toolkit.1x")

# 真语料：gameplay 全取；scripts 取 `gamedata\scripts`（468 个 .script，实测 446 个能提到文本）
#   ★ 别用 `gamedata\config\scripts`：那 3695 个里 3693 个是纯 logic 的 .ltx（一条都提不到），
#     只有 2 个 .script 有文本 —— 拿它当语料等于只测"原样写回"（实测踩过）。
REAL_GAMEPLAY = samples.find_alias("text.real_gameplay")
REAL_SCRIPTS = samples.find_alias("text.real_scripts")
SCRIPT_SAMPLE = 60

FIXED_TS = 1700000000
PREFIX = "gstest"
ID_PREFIX = "sgtat_%s_{abbr}_%d" % (PREFIX, FIXED_TS)

RUS = "Привет, сталкер!"


# ── 合成夹具 ──────────────────────────────────────────────────────────
def synth_gameplay(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "a.xml").write_bytes(
        ('<?xml version="1.0" encoding="windows-1251"?>\n'
         "<config>\n"
         "  <text>Привет, сталкер!</text>\n"
         "  <bio>Описание персонажа</bio>\n"
         "  <title>Название</title>\n"
         "  <name>Имя</name>\n"
         "</config>\n").encode("cp1251"))
    # 属性分支：hint / name；len>2 才收（"ok" 两项长度不足 ⇒ 必须被丢掉）
    (root / "b.xml").write_text(
        '<string_table>\n'
        '  <string id="s1"><text hint="Подсказка" name=\'Имя два\'/></string>\n'
        '  <string id="s2"><text hint="ok" name="ab"/></string>\n'
        "</string_table>\n", encoding="cp1251")
    # 多根 + 实体 + 注释 + 非法实体
    (root / "c.xml").write_text(
        "<a><text>Текст &amp; ещё</text></a>\n"
        "<b><text>Плохой & amp; и &foo; тут</text></b>\n"
        "<!-- комментарий --><c><text>После комментария</text></c>\n", encoding="cp1251")
    # 全是 ID 形态：仍要产出（正文规范化后原样写回），但一条都不提
    (root / "d.xml").write_text(
        '<config><text>some_string_id</text><text>другой</text></config>\n', encoding="cp1251")
    # UTF-8 BOM + 中文
    (root / "e.xml").write_bytes(
        b"\xef\xbb\xbf" + '<config><text>你好，潜行者</text></config>\n'.encode("utf-8"))
    sub = root / "sub"
    sub.mkdir(exist_ok=True)
    (sub / "f.xml").write_text("<config><text>Вложенный файл</text></config>\n", encoding="cp1251")
    # 超长正文（>1000 字符）→ 走切行分支
    (root / "g.xml").write_text("<config><text>%s</text></config>\n" % ("Длинный текст. " * 200),
                                encoding="cp1251")
    # ★★ 2026-10-10（P17-9，审计 02 F-04 Medium）：**正文（转义后）长度恰为 1000 的整数倍**。
    #   事实（修前）：`split_text_at_length` 在 `full = chars / n` 之后**无条件**再推一段
    #   `text.substr(offsets[full * n])`；整倍时那一段是空串 ⇒ 调用处 `join(..., "\n")` 让
    #   `<text>` 末尾多出一个 `\n`，与 1.x（Python `text[i:i+1000]`，整倍时不产生空尾块）
    #   **逐字节不一致**。触发窄但完全静默，只有"整棵产物树逐字节对拍"能发现 ⇒ 这里补合成夹具。
    #   字符串**含标点与西里尔字母**（不会被两侧"看起来像 ID/脚本"的判据丢掉 —— 实测第一版用
    #   `("wordy text " * 100)[:1000]` 被 1.x 判成 ID 形态、整条跳过，夹具自己先红了），
    #   且无 `&<>"'` ⇒ 转义后不变；`"Текст для проверки. "` 恰 20 码位 ⇒ ×50 = 1000。
    #   ★★ 末尾必须**不是空格**：规范化会裁掉行尾空白（实测 `…проверки. ` 被裁成 999 ⇒ 夹具
    #   绕开了那个"整倍"分支、修前也判绿 —— 第一版夹具就是这么自欺的）。把最后一格换成 `.`
    #   既保持 1000 码位，又让"规范化后仍是 1000"。
    exact1000 = ("Текст для проверки. " * 50)[:-1] + "."
    exact2000 = ("Текст для проверки. " * 100)[:-1] + "."
    assert len(exact1000) == 1000 and len(exact2000) == 2000
    (root / "k_exact1000.xml").write_text("<config><text>%s</text></config>\n" % exact1000,
                                          encoding="cp1251")
    # 再来一个恰 2000 码位的（跨两个整块，确认只有**末尾**那一段不许是空串）
    (root / "l_exact2000.xml").write_text("<config><text>%s</text></config>\n" % exact2000,
                                          encoding="cp1251")
    # ★★ 2026-10-10（P17-44，审计 01 F-10 Low）：**大小写折叠的次序判据**（ASCII 折 vs Unicode 折**次序不同**）。
    #   这两个文件名是精心挑的：`Б.xml`（西里尔大写 Б = D0 91）与 `а.xml`（西里尔小写 а = D0 B0）。
    #     · **只看 ASCII** 折（只折 A-Z）⇒ 键还是 D0 91 与 D0 B0 ⇒ **Б 在前**；
    #     · **Unicode** 折（Python `str.lower()` / LCMapStringEx）⇒ б(D0 B1) 与 а(D0 B0) ⇒ **а 在前**。
    #   1.x 的排序键是 `str(path).lower()` ⇒ 真次序是"а 在前"。夹具把这一点**钉进基线**（基线由 1.x 真跑生成）。
    (root / "Б.xml").write_text("<config><text>Заглавная Бэ</text></config>\n", encoding="cp1251")
    (root / "а.xml").write_text("<config><text>Строчная а</text></config>\n", encoding="cp1251")
    # 需要转义的字符
    (root / "h.xml").write_text(
        '<config><text>Кавычки "двойные" и \'одинарные\' &amp; амперсанд</text></config>\n',
        encoding="cp1251")
    # 排除项：translated_ / text/ / 本工具自己的产物
    (root / "translated_x").mkdir(exist_ok=True)
    (root / "translated_x" / "i.xml").write_text("<config><text>Не должно попасть</text></config>\n",
                                                 encoding="cp1251")
    (root / "text").mkdir(exist_ok=True)
    (root / "text" / "j.xml").write_text("<config><text>Тоже не должно</text></config>\n",
                                         encoding="cp1251")
    (root / ("%s_gameplay_texts.xml" % PREFIX)).write_text(
        '<string_table><string id="x"><text>Свой продукт</text></string></string_table>\n',
        encoding="cp1251")


def synth_scripts(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "a.script").write_text(
        'local s = "Привет, сталкер!"\n'
        "local t = 'Одинарные кавычки'\n"
        'local u = "С экранированием \\"внутри\\" текста"\n',
        encoding="cp1251")
    (root / "b.script").write_text(
        'local msg = "message строка"\n'
        'local txt = "text строка"\n'
        'local tex = "texture не текст"\n'
        'local nws = "news строка"\n'
        'local ex = "exec команда"\n'
        'local sc = "script файл"\n'
        'local st = "set свойство"\n'
        'local rn = "return значение"\n'
        "-- local cmt = \"комментарий не считается\"\n"
        'local eq = "a = b"\n'
        'local at = "user@host"\n'
        'local tm = "12:30"\n'
        'local ld = "load ~~~~"\n'
        'local idn = "some_identifier"\n',
        encoding="cp1251")
    (root / "c.script").write_text('local s = "Строка_с_подчёркиванием"\n', encoding="cp1251")
    # .ltx：三种键 + 引号/裸值 + {=...} 标记 + 逗号拆条 + 无法安全定位的那一条
    (root / "d.ltx").write_text(
        "[wpn_akm]\n"
        'inv_name = "АКМ"\n'
        "inv_name_short = 'АКМС'\n"
        "description = Нож\n"
        'title = "Название", "Второе"\n'
        "descr = {=показ}Часть1, Часть2\n"
        "; descr = Не должно попасть\n"
        "inv_name_empty =\n"
        "не_то = Без равно в ключе\n",
        encoding="cp1251")
    (root / "e.ltx").write_text('inv_name = \'Сломанная"\n', encoding="cp1251")


def _kept_count(f: Path) -> int:
    """该真文件按 1.x 的规则能提到几条（用来挑"有货"的样本）。"""
    from apps.text_extract_app import (_does_text_look_like_id, _does_text_look_like_script,
                                      _ltx_source_literal, _parse_ltx, _parse_script)
    is_ltx = f.suffix.lower() == ".ltx"
    try:
        wt, cands = (_parse_ltx(str(f), quiet=True) if is_ltx
                     else _parse_script(str(f), quiet=True))
    except Exception:  # noqa: BLE001
        return 0
    n = 0
    for c in sorted(cands):
        if is_ltx:
            if not c.strip() or _does_text_look_like_id(c) or _does_text_look_like_script(c):
                continue
            if _ltx_source_literal(wt, c) is None:
                continue
        else:
            if len(c) < 2:
                continue
            inner = c[1:-1]
            if _does_text_look_like_id(inner) or _does_text_look_like_script(inner):
                continue
        n += 1
    return n


def stage_real_scripts() -> Path:
    """真 scripts 树取样：先收"有货"的文件（能提到 ≥1 条），再补几个"没货"的。

    只按前 N 个文件抽会因为前 120 个恰好一条都提不到，测不到提取路径（实测踩过）。
    """
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    stage = WORK / "stage" / "scripts"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    files = sorted(list(REAL_SCRIPTS.rglob("*.script")) + list(REAL_SCRIPTS.rglob("*.ltx")))
    picked: list[Path] = []
    empty: list[Path] = []
    for f in files:
        if len(picked) < SCRIPT_SAMPLE and _kept_count(f) > 0:
            picked.append(f)
        elif len(empty) < 10:
            empty.append(f)
        if len(picked) >= SCRIPT_SAMPLE and len(empty) >= 10:
            break
    for f in picked + empty:
        rel = f.relative_to(REAL_SCRIPTS)
        dst = stage / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, dst)
    print("  [取样] 有货 %d 个 + 没货 %d 个（共扫 %d 个文件）"
          % (len(picked), len(empty), len(files)))
    return stage


FIXTURES: list[tuple[str, str]] = []  # (id, kind)


def prepare() -> list[tuple[str, str, Path]]:
    """→ [(id, kind, 源目录)]"""
    WORK.mkdir(parents=True, exist_ok=True)
    out: list[tuple[str, str, Path]] = []
    gp = WORK / "synth" / "gameplay"
    if gp.exists():
        shutil.rmtree(gp)
    synth_gameplay(gp)
    out.append(("synth-gameplay", "gameplay", gp))
    sc = WORK / "synth" / "scripts"
    if sc.exists():
        shutil.rmtree(sc)
    synth_scripts(sc)
    out.append(("synth-scripts", "scripts", sc))
    if REAL_GAMEPLAY.is_dir():
        out.append(("real-gameplay", "gameplay", REAL_GAMEPLAY))
    if REAL_SCRIPTS.is_dir():
        out.append(("real-scripts", "scripts", stage_real_scripts()))
    return out


def tree_hashes(root: Path) -> dict[str, dict]:
    out: dict[str, dict] = {}
    if not root.exists():
        return out
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = str(p.relative_to(root)).replace("\\", "/")
        data = p.read_bytes()
        out[rel] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return out


def run_1x(kind: str, src: Path, out_dir: Path) -> dict:
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from apps.text_extract_app import (_extract_gameplay, _extract_scripts,  # noqa: PLC0415
                                       _generate_text_xml)

    if out_dir.exists():
        shutil.rmtree(out_dir)
    sub = out_dir / kind
    sub.mkdir(parents=True)
    id_prefix = ID_PREFIX.format(abbr="gp" if kind == "gameplay" else "sc")
    counts: dict = {}
    if kind == "gameplay":
        texts = _extract_gameplay(str(src), str(sub), id_prefix, verbose=False, counts_out=counts)
    else:
        texts = _extract_scripts(str(src), str(sub), id_prefix, verbose=False, counts_out=counts)
    if texts:
        _generate_text_xml(str(out_dir / ("%s_%s_texts.xml" % (PREFIX, kind))), texts)
    return {"files": counts.get("files", 0), "strings": len(texts), "chars": counts.get("chars", 0)}


def run_2x(kind: str, src: Path, out_dir: Path) -> dict:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    id_prefix = ID_PREFIX.format(abbr="gp" if kind == "gameplay" else "sc")
    argv = [str(CLI), "--extract-text", str(src), str(out_dir), "--prefix", PREFIX,
            "--id-prefix", id_prefix]
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    if proc.returncode != 0:
        raise RuntimeError(text[:300])
    line = [ln for ln in text.splitlines() if ln.startswith("TEXT ok")]
    if not line:
        raise RuntimeError("找不到 TEXT ok 收尾行：%s" % text[:200])
    fields = dict(kv.split("=", 1) for kv in line[-1].split()[2:])
    return {"files": int(fields["files"]), "strings": int(fields["strings"]),
            "chars": int(fields["chars"]), "skipped": int(fields["skipped"]),
            "tail": line[-1]}


def diff_trees(exp: dict, got: dict) -> list[str]:
    problems: list[str] = []
    for rel in sorted(set(exp) | set(got)):
        if rel not in got:
            problems.append("缺文件 %s（基线 %d 字节）" % (rel, exp[rel]["size"]))
        elif rel not in exp:
            problems.append("多文件 %s（%d 字节）" % (rel, got[rel]["size"]))
        elif exp[rel] != got[rel]:
            problems.append("内容不同 %s（基线 %d 字节 / 候选 %d 字节）"
                            % (rel, exp[rel]["size"], got[rel]["size"]))
    return problems



CJK_RE = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")
TRANS_RE = re.compile(r'game\.translate_string\("([^"]+)"\)')


def _texts_of(path: Path) -> set:
    body = path.read_text(encoding="utf-8", errors="replace")
    return {m.group(1) for m in re.finditer(r"<text>(.*?)</text>", body, re.S)}


def _ids_of(path: Path) -> set:
    body = path.read_text(encoding="utf-8", errors="replace")
    return set(TRANS_RE.findall(body))


def superset_ok(ref_dir: Path, ours_dir: Path) -> tuple:
    """2026-10-28 新口径：**1.x 的产物必须是我们的子集**。

    事实：用户要求「符合文本的格式就全部提取出来」⇒ 中文正文也会被抽，
    与 1.x 的"只认西里尔"必然不同字节。这条替代"逐字节相等"：
      · 基线里每个文本都在我们这里（内容一致，id 可以不同——顺序变了但翻译表等价）；
      · 我们多出来的文本**必须含 CJK**（否则就是乱抽/乱码被当成正文）；
      · 改写后的文件里，基线引用的 id 必须都在我们这边能找到对应文本。
    返回 (ok, 说明)。
    """
    ref_files = {p.relative_to(ref_dir).as_posix(): p for p in ref_dir.rglob("*") if p.is_file()}
    ours_files = {p.relative_to(ours_dir).as_posix(): p for p in ours_dir.rglob("*") if p.is_file()}
    missing = [rel for rel in ref_files if rel not in ours_files]
    if missing:
        return False, "缺文件 %d 个：%s" % (len(missing), missing[:3])
    ref_texts, ours_texts = set(), set()
    for rel, path in ref_files.items():
        # 只比对**文本表**（`*texts.xml`）：被改写的源文件里 `<text>` 已经换成 id，
        # 混进来会把 id 当成"文本"，判据就自己骗自己了。
        if path.suffix.lower() == ".xml" and "texts" in path.name.lower():
            ref_texts |= _texts_of(path)
            ours_texts |= _texts_of(ours_files[rel])
    not_covered = ref_texts - ours_texts
    if not_covered:
        return False, "1.x 有 %d 条文本我们没抽到（例 %r）" % (
            len(not_covered), sorted(not_covered)[:2])
    extra = ours_texts - ref_texts
    bad_extra = [x for x in extra if not CJK_RE.search(x)]
    if bad_extra:
        return False, "多出 %d 条**非中文**文本（乱抽？例 %r）" % (len(bad_extra), bad_extra[:2])
    # 改写文件只比"有没有改写"（`translate_string` 出现），不比 id 编号：
    # 口径增量会让 id 整体平移（多一条中文，后面的编号全变），拿它当契约必假红。
    for rel, path in ref_files.items():
        if path.suffix.lower() not in (".script", ".ltx"):
            continue
        if _ids_of(path) and not _ids_of(ours_files[rel]):
            return False, "改写文件 %s 里没有 translate_string（改写链断了）" % rel
    return True, "1.x 基线是子集，多出 %d 条中文（口径增量）" % len(extra)

def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="文本提取层逐字节对拍")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    parser.add_argument("--id", default="", help="只跑 id 含该子串的夹具")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-TEXT-EXTRACT: SKIP (cli-missing)")
        return 2

    fixtures = [(i, k, p) for i, k, p in prepare() if not args.id or args.id in i]
    if not fixtures:
        print("!! 没有夹具（真语料不可达且合成夹具也没建成？）")
        print("GOLDEN-TEXT-EXTRACT: SKIP (no-fixtures)")
        return 2

    if args.write:
        # ★ --write 只更新**本次跑到的**夹具，其余条目保留：否则 `--write --id synth`
        #   会把真语料条目从基线里抹掉（实测踩过）。
        records: list[dict] = []
        if BASELINE.exists():
            old = json.loads(BASELINE.read_text(encoding="utf-8"))
            written = {f[0] for f in fixtures}
            records = [e for e in old.get("entries", []) if e["id"] not in written]
        for fixture_id, kind, src in fixtures:
            stats = run_1x(kind, src, WORK / fixture_id / "ref")
            records.append({"id": fixture_id, "kind": kind, "source": str(src),
                            "stats": stats, "tree": tree_hashes(WORK / fixture_id / "ref")})
            print("[%s] 基线：files=%d strings=%d chars=%d，产物 %d 个文件"
                  % (fixture_id, stats["files"], stats["strings"], stats["chars"],
                     len(records[-1]["tree"])))
        records.sort(key=lambda e: e["id"])
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_bytes((json.dumps(
            {"note": "基线由 1.x apps.text_extract_app 的 _extract_gameplay/_extract_scripts 生成",
             "id_prefix": ID_PREFIX, "entries": records}, indent=1, ensure_ascii=False) + "\n"
        ).encode("utf-8"))
        print("已写基线 %d 条 → %s" % (len(records), BASELINE.relative_to(ROOT)))
        print("GOLDEN-TEXT-EXTRACT: PASS (write)")
        return 0

    if not BASELINE.exists():
        print("!! 缺基线（先跑 --write）")
        print("GOLDEN-TEXT-EXTRACT: FAIL (no-baseline)")
        return 1

    base = {e["id"]: e for e in json.loads(BASELINE.read_text(encoding="utf-8"))["entries"]}
    failures = 0
    compared = 0
    for fixture_id, kind, src in fixtures:
        exp = base.get(fixture_id)
        if exp is None:
            print("  !! 基线里没有 %s（夹具变了？重跑 --write）" % fixture_id)
            failures += 1
            continue
        print("[%s]" % fixture_id)
        try:
            stats = run_2x(kind, src, WORK / fixture_id / "ours")
        except Exception as exc:  # noqa: BLE001
            print("  ✗ %s" % exc)
            failures += 1
            continue
        got = tree_hashes(WORK / fixture_id / "ours")
        problems = diff_trees(exp["tree"], got)
        if problems:
            # ★★ 2026-10-28（用户口径）：中文正文也算正文 ⇒ 必然与 1.x 不同字节。
            #   退回"1.x ⊂ ours"的判据（多出来的必须是中文，基线一条都不能丢）。
            ok_super, why = superset_ok(WORK / fixture_id / "ref", WORK / fixture_id / "ours")
            if ok_super:
                print("  ok 口径增量：%s" % why)
            else:
                failures += 1
                print("  ✗ 产物树不一致 %d 处（基线 %d 个文件 / 候选 %d 个）：%s"
                      % (len(problems), len(exp["tree"]), len(got), why))
                for p in problems[:10]:
                    print("      %s" % p)
        else:
            print("  ✓ 产物树逐字节一致（%d 个文件）" % len(got))
        # 统计口径：1.x 子集模式 ⇒ 候选必须 >= 基线（口径增量只许"多"，不许"少"）
        base_stats = (exp["stats"]["strings"], exp["stats"]["chars"])
        got_stats = (stats["strings"], stats["chars"])
        allowance = bool(problems) and ok_super
        if allowance:
            if got_stats[0] >= base_stats[0] and got_stats[1] >= base_stats[1]:
                print("  ok 统计：基线 strings=%d chars=%d ⊆ 候选 strings=%d chars=%d"
                      % (base_stats + got_stats))
            else:
                failures += 1
                print("  ✗ 统计缩水：基线 strings=%d chars=%d / 候选 strings=%d chars=%d"
                      % (base_stats + got_stats))
        elif got_stats != base_stats:
            failures += 1
            print("  ✗ 统计不一致：基线 strings=%d chars=%d / 候选 strings=%d chars=%d"
                  % (base_stats + got_stats))
        else:
            print("  ok strings=%d chars=%d %s" % (stats["strings"], stats["chars"], stats["tail"]))

        # ⑦7.2：**同一份源跑两次必须逐字节相同**。1.x 的坑是候选遍历没排序
        #（Python set 迭代随哈希随机化）⇒ 同一份源两次运行得到不同 ID。
        # 这条对拍只在候选数 >1 时有意义（一个候选的话排不排序都一样）。
        try:
            run_2x(kind, src, WORK / fixture_id / "again")
            second = tree_hashes(WORK / fixture_id / "again")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print("  ✗ 第二次运行失败：%s" % exc)
        else:
            repeat = diff_trees(got, second)
            if repeat:
                failures += 1
                print("  ✗ 7.2 两次运行产物不同 %d 处（候选遍历没排序 / 有随机性）"
                      % len(repeat))
                for p in repeat[:6]:
                    print("      %s" % p)
            else:
                print("  ✓ 7.2 两次运行逐字节相同（%d 个文件）" % len(second))
        compared += 1

    print("")
    print("GOLDEN-TEXT-EXTRACT: %s (compared=%d failed=%d)"
          % ("FAIL" if failures else "PASS", compared, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
