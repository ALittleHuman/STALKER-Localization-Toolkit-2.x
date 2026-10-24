# -*- coding: utf-8 -*-
"""**名字合法性**判据（hotfix-1：R10 / R20 / R15）。

锁三件事
--------
A 静态：`src/engine/fs/name_check.h` 是**唯一一份**判定，parser 与落盘层**都调它**
   （修前：NUL/控制字符三层都不查；保留名只在 `write_file_atomic` 的**叶子**上查）。
B 行为①（控制字符，走落盘层）：造一个真名叫 `a\\x01b.txt` 的文件 → 打成 sqfs → 解包**必须被拒**，
   拒因点名"控制字符"；同一张镜像里**合法**的那一条仍要能解出来（防"一刀切全拒"）。
C 行为②（保留设备名，走 parser）：把镜像里的 `abc.txt` **逐字节改成 `CON.txt`** → 解包**必须被拒**，
   拒因点名"保留设备名"；**未打补丁的同一张镜像必须成功**（基线，防"恒红"）。
D 静态：slink 必须报"不支持的条目类型"（`kTypeSlink` 分支存在），不再当普通文件读。

退出码：0=绿 / 1=红 / 2=缺 CLI 产物。
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
NAME_CHECK = ROOT / "src" / "engine" / "fs" / "name_check.h"
PARSER = ROOT / "src" / "engine" / "fs" / "squashfs.cpp"
EXTRACT = ROOT / "src" / "engine" / "fs" / "extract_tree.cpp"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

problems: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            for line in detail.splitlines():
                print("      " + line)
        problems.append(label)


def run(args: list[str], timeout: int = 300) -> tuple:
    proc = subprocess.run([str(CLI)] + args, cwd=str(ROOT), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 名字合法性（R10 控制字符 / R20 保留名 / R15 slink 拒绝）==")
    if not CLI.is_file():
        print("FS-NAME skip 缺 %s" % CLI.relative_to(ROOT).as_posix())
        return 2

    # ── A 静态：唯一一份判定，两处都调 ─────────────────────────────────
    a_ok = NAME_CHECK.is_file()
    check(a_ok, "`fs/name_check.h` 存在（判定只许一份）")
    if a_ok:
        nc = NAME_CHECK.read_text(encoding="utf-8")
        check("has_control_bytes" in nc and "is_reserved_device_name" in nc,
              "name_check 里同时有控制字符与保留名两条规则")
        check("segment_reject_reason" in nc, "name_check 暴露 `segment_reject_reason`（唯一入口）")
        for name, path in (("parser", PARSER), ("落盘层", EXTRACT)):
            text = path.read_text(encoding="utf-8")
            check("name_check.h" in text and "name_check::segment_reject_reason" in text,
                  "`%s` 调同一把尺（include + 调用）" % name)
        parser_text = PARSER.read_text(encoding="utf-8")
        check("kTypeSlink" in parser_text and "不支持的条目类型" in parser_text,
              "parser 对 slink 明确报「不支持的条目类型」（R15）")

    # ★ 造夹具的难点：**Windows 不允许创建**含控制字符/保留名的真实文件（OSError 22 / 设备名），
    #   所以行为面走"**逐字节改镜像里的名字**"——同名同长度覆盖，镜像其余字节一个不动。
    #   这同时把 parser 与落盘层都跑到（两层共用 `name_check`，正是本批的目的）。
    tmp = Path(tempfile.mkdtemp(prefix="fsname_", dir=str(ROOT / "build")))
    try:
        stage = tmp / "stage"
        stage.mkdir()
        (stage / "abc.txt").write_text("x", encoding="utf-8")
        (stage / "долго.txt").write_text("ru", encoding="utf-8")   # 合法但非常规：防"一刀切"
        image = tmp / "base.sqfs"
        rc, out = run(["--pack-sqfs", str(image), str(stage)])
        check(rc == 0 and image.is_file(), "造夹具：三条目打成 sqfs", out[-400:])
        if rc != 0 or not image.is_file():
            return 1
        raw = image.read_bytes()
        pos = raw.find(b"abc.txt")
        check(pos >= 0, "镜像里能找到 `abc.txt` 的目录项名字（同长度改写的前提）")

        # 基线：未改名的镜像必须能完整解包（防"恒红"）
        out_base = tmp / "out_base"
        rc_b, out_b = run(["--unpack-package", str(image), str(out_base)])
        # ★ 落点可能多一层"包名壳"（本夹具不自带壳 ⇒ 解到 `<out>/base/…`）⇒ 递归找，不假设层级。
        def find_out(root: Path, name: str):
            for cand in root.rglob(name):
                return cand
            return None

        ab = find_out(out_base, "abc.txt")
        ru = find_out(out_base, "долго.txt")
        base_ok = (rc_b == 0 and ab is not None and ab.read_text(encoding="utf-8") == "x"
                   and ru is not None and ru.read_text(encoding="utf-8") == "ru")
        check(base_ok, "基线：未改名的镜像能解包，且非 ASCII 名字正常（不是一刀切）", out_b[-500:])

        if pos >= 0:
            # ② 控制字符（R10）：`abc.txt` → `a\x01c.txt`（同为 7 字节）
            ctrl = tmp / "ctrl.sqfs"
            ctrl.write_bytes(raw[:pos] + b"a\x01c.txt" + raw[pos + 7:])
            check(ctrl.read_bytes() != raw, "自证：控制字符补丁确实改动了镜像字节")
            rc_c, out_c = run(["--unpack-package", str(ctrl), str(tmp / "out_ctrl")])
            print("    控制字符镜像解包退出码=%d" % rc_c)
            check(rc_c != 0 and "控制字符" in out_c,
                  "控制字符名字被拒、理由点名「控制字符」", out_c[-600:])

            # ③ 保留设备名（R20）：`abc.txt` → `CON.txt`（同为 7 字节）
            res = tmp / "res.sqfs"
            res.write_bytes(raw[:pos] + b"CON.txt" + raw[pos + 7:])
            check(res.read_bytes() != raw, "自证：保留名补丁确实改动了镜像字节")
            rc_r, out_r = run(["--unpack-package", str(res), str(tmp / "out_res")])
            print("    保留名镜像解包退出码=%d" % rc_r)
            check(rc_r != 0 and "保留设备名" in out_r,
                  "保留设备名被拒、理由点名「保留设备名」", out_r[-600:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("FS-NAME fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("FS-NAME ok 名字合法性：控制字符/保留名/ slink 三处都明确拒绝（parser 与落盘层同一把尺）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
