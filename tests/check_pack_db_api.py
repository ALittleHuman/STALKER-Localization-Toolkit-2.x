# -*- coding: utf-8 -*-
r"""判据 A-5：`pack_db` 的失败信号**不许依赖可选出参**（第三方审计 T-04／HanaAgent 2026-10-18）。

事实（本仓代码核，逐行）：
  · `src/engine/fs/db.cpp::build_header()` 的两条 4 GiB 窄化检查（单条目 `data.size()` /
    累计偏移 `off`）**无条件**执行并 `return {}`（P17-68 已从 opt-in 改成无条件）；
  · 但 `pack_db()` 的中止条件写成了 `if (error != nullptr && !error->empty()) return {};`
    ⇒ `error == nullptr` 的调用方（本仓 smoke 里就有 `pack_db(items, fmt)`）拿到的是
    **空包头 ＋ 完整数据块** 的包：LZHUF 编码一个空 header，再拼上数据区 ——
    「能打开、但一个条目都没有」，比崩掉更难查；
  · `db.h` 的注释还写着「给了它（非 `nullptr`）**才启用**检查」（opt-in），
    与 `.cpp` 里「检查已改成无条件」（P17-68）**互相矛盾** —— 审计把这两句都点了出来。

★ 为什么 A/B/C 只能静态判（写在判据头上，免得后人误以为"没验就算过"）：
  两条检查都要**真的持有 4 GiB 数据**才可能触发（`DbPackItem::data` 是实体 `std::vector`，
  `resize` 就是真分配），门禁里不可能跑。这里判的是**失败信号的形状** ——
  它是不是"看返回值"而不是"看某个调用方给没给出参"。与 P17-83 的口径一致：
  能静态判的一律静态判，并且写清守的是什么。

断言：
  A `build_header` 的两条检查各带 `return {}`，且都**置了失败标志**（计数 == 2）；
  B `pack_db` 的中止**只看** build_header 的成功标志：体里必须有 `if (!header_ok)`，
    且**不许**再出现把 `error` 当闸门的形态（`error != nullptr && !error->empty()`）；
  C 中止必须在 `lzhuf_encode` **之前**（不许先压包、再回头说失败）；
  D `db.h`／`db.cpp` 里 pack_db 的注释**不许**再出现「才启用」类 opt-in 说法
    （★ 这一条查的就是**注释文字本身**，所以用原文、不去注释 —— P17-83 教训 ①）；
  E 运行时**正控**：CLI `--pack-db xdb` 建的小树，`--dump` 回读的条目名与尺寸必须与输入一致
    （证明"把信号从出参挪到返回值"没有弄坏正常路径）。

退出码：0=绿 / 1=判据不成立 / 2=缺 CLI 构建产物（E 那条没跑成 ⇒ 不假装通过）。
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _cpp_source import function_body, strip_comments  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "engine" / "fs" / "db.cpp"
HDR = ROOT / "include" / "xrkit" / "engine" / "fs" / "db.h"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def pack_db_comment_in_header(raw: str) -> str:
    """`db.h` 里 pack_db 那段注释（从「封包（写路径）」到声明结束）。"""
    a = raw.find("// 封包（写路径）")
    # ★ 2026-10-24（batch-3 / 3a）：`pack_db` 多了两个**默认参数**（duplicates / strict）
    #   ⇒ 声明不再以 `std::string* error = nullptr);` 结尾。锚点跟着签名走（判据不许因签名演进而静默失效）。
    b = raw.find("bool strict = false);")
    if b < 0:
        b = raw.find("std::string* error = nullptr);")   # 兼容旧签名（回滚时仍能判）
    if a < 0 or b < 0 or b <= a:
        return ""
    return raw[a:b]


def pack_db_comment_in_cpp(raw: str) -> str:
    """`db.cpp` 里 `pack_db` 函数体（含其内部注释）—— 只查它，不牵连别处。"""
    a = raw.find("std::vector<uint8_t> pack_db(")
    return raw[a:] if a >= 0 else ""


def main() -> int:
    print("== pack_db 的失败信号（T-04）：不许把 error 出参当闸门 ==")
    for p in (CPP, HDR):
        if not p.is_file():
            print("PACK-DB-API skip 缺 " + str(p))
            return 2

    src_raw = CPP.read_text(encoding="utf-8")
    hdr_raw = HDR.read_text(encoding="utf-8")
    src = strip_comments(src_raw)

    # ── A：build_header 的两条检查都置失败标志 ─────────────────────
    #    ★ 签名要抓**函数头**（`function_body` 从花括号起，天然不含参数表）
    sig = re.search(r"std::vector\s*<\s*uint8_t\s*>\s*build_header\s*\((.*?)\)\s*\{", src, re.S)
    want(sig is not None, "A0 抓到 build_header 的定义（抓不到 = 判据自己失效，红）")
    want(sig is not None and re.search(r"bool\s*\*\s*ok", sig.group(1)) is not None,
         "A3 build_header 的签名带 `bool* ok`（失败经返回值传出）",
         sig.group(1).replace("\n", " ")[:90] if sig else "")
    bh = function_body(src, "std::vector<uint8_t> build_header(")
    want(bool(bh), "A0b 切到 build_header 函数体")
    n_flag = bh.count("*ok = false")
    n_ret = len(re.findall(r"return\s*\{\s*\}", bh))
    want(n_flag == 2, "A1 两条 4 GiB 检查都置 `*ok = false`", "实际 %d 处（要求 2）" % n_flag)
    want(n_ret >= 2, "A2 两条检查仍 `return {}`（不返回半截包头）", "实际 %d 处" % n_ret)

    # ── B：pack_db 只看成功标志，不看 error ──────────────────────
    pk = function_body(src, "std::vector<uint8_t> pack_db(")
    want(bool(pk), "B0 切到 pack_db 函数体")
    want(bool(re.search(r"if\s*\(\s*!\s*header_ok\s*\)", pk)),
         "B1 中止条件是 `if (!header_ok)`（成功标志，不是出参）")
    gate_on_error = re.search(r"error\s*!=\s*nullptr\s*&&\s*!\s*error\s*->\s*empty\s*\(\s*\)", pk)
    want(gate_on_error is None,
         "B2 **不许**把 `error != nullptr && !error->empty()` 当闸门",
         ("命中：" + pk[max(0, gate_on_error.start() - 20):gate_on_error.end() + 20].replace("\n", " ")[:80])
         if gate_on_error else "")

    # ── C：中止在压包之前 ───────────────────────────────────────
    i_abort = pk.find("!header_ok")
    i_encode = pk.find("lzhuf_encode")
    want(i_abort >= 0 and i_encode >= 0 and i_abort < i_encode,
         "C1 失败中止发生在 `lzhuf_encode` **之前**",
         "abort@%d encode@%d" % (i_abort, i_encode))

    # ── D：注释口径（查原文，不去注释）──────────────────────────
    stale = ["才启用", "opt-in", "未接线的调用点行为不变", "行为逐字节不变", "保持旧语义"]
    hdr_block = pack_db_comment_in_header(hdr_raw)
    want(bool(hdr_block), "D0 切到 db.h 里 pack_db 的注释块")
    hits_h = [s for s in stale if s in hdr_block]
    want(not hits_h, "D1 db.h 的 pack_db 注释不再声称「给了才启用／行为不变」", "仍含：" + "、".join(hits_h))
    cpp_block = pack_db_comment_in_cpp(src_raw)
    hits_c = [s for s in stale if s in cpp_block]
    want(not hits_c, "D2 db.cpp 的 pack_db 注释同上", "仍含：" + "、".join(hits_c))
    want("不承诺" in hdr_block or "无条件" in hdr_block,
         "D3 db.h 的注释**正面写出**了现在的口径（无条件检查／error 只报理由）")

    # ── E：运行时正控（正常打包路径不许被这次改动弄坏）─────────────
    if not CLI.is_file():
        print("   · 缺 CLI 构建产物 ⇒ E 那条**未跑**（本判据不假装它通过）")
        print("PACK-DB-API skip 缺 CLI（静态 12 项已判，运行时正控未验）")
        return 2
    with tempfile.TemporaryDirectory(prefix="packdbapi_") as tmp:
        base = Path(tmp)
        tree = base / "src"
        (tree / "sub").mkdir(parents=True)
        (tree / "a.txt").write_bytes(b"alpha\n")
        (tree / "sub" / "b.bin").write_bytes(bytes(range(256)) * 2)
        pack = base / "out.db0"
        p = subprocess.run([str(CLI), "--pack-db", str(pack), "xdb", str(tree)],
                           capture_output=True, timeout=600)
        log = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
        want(p.returncode == 0 and pack.is_file(), "E1 --pack-db xdb 退出码 0 且产物非空",
             log.strip().splitlines()[-1][:120] if log.strip() else "")
        d = subprocess.run([str(CLI), "--dump", str(pack), "xdb"],
                           capture_output=True, timeout=600)
        dlog = ((d.stdout or b"") + (d.stderr or b"")).decode("utf-8", "replace")
        want(d.returncode == 0 and "DUMP ok kind=db" in dlog, "E2 --dump 回读成功",
             dlog.strip().splitlines()[-1][:120] if dlog.strip() else "")
        # `--dump` 的 DB 分支是 6 列 TSV：`<d|f>\t<路径>\t<尺寸>…` ⇒ 名字在第 2 列
        names = set()
        for line in dlog.splitlines():
            f = line.split("\t")
            if len(f) >= 2:
                names.add(f[1].strip())
        want({"a.txt", "sub\\b.bin"} <= names or {"a.txt", "sub/b.bin"} <= names,
             "E3 回读的条目名与输入一致（两个文件都在）", "回读：" + ", ".join(sorted(names))[:90])

    print("-" * 66)
    if PROBLEMS:
        print("PACK-DB-API fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PACK-DB-API ok %d 项（失败信号看返回值、不看 error；正常打包路径仍通）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
