#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""拉取 2.x 的第三方依赖源码（vendored，离线可复现）。

为什么不引 vcpkg 之类的包管理：本机 **GitHub 被 hosts 指向 127.0.0.1**，
且 schannel TLS 整体不可用（curl/git/PowerShell 走不通，只有 Python/Node 的
OpenSSL 栈正常）。所以就地用 Python 从**实测可达**的源拉 tarball。

纪律：
  · 每个 tarball 记录 sha256 到 third_party/sources.lock.json；已记录的一律先校验再解包；
  · 未记录的包**首次拉取后写锁**；锁里有而实际不符 → 报错退出（不许静默覆盖）；
  · 源码不入库（体积），只入库脚本 + 锁 + SOURCES.md。

用法：
    python tools/fetch_third_party.py              # 拉取缺失的，校验已有的
    python tools/fetch_third_party.py --check      # 只校验，不下载
    python tools/fetch_third_party.py --only zlib  # 只处理某个
    python tools/fetch_third_party.py --update-lock zlib   # 有意换源时重写某条锁
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TP = ROOT / "third_party"
CACHE = TP / ".cache"
LOCK = TP / "sources.lock.json"

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120 Safari/537.36")

# 源全部是本机实测可达的（见 README「第三方依赖」表）。
SOURCES = [
    {
        "name": "zlib",
        "version": "1.3.1",
        "url": "https://gh-proxy.com/https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz",
        "archive": "zlib-1.3.1.tar.gz",
        "extract_to": "zlib",
        "note": "SquashFS gzip 块 + 通用压缩",
    },
    {
        "name": "xz",
        "version": "5.6.4",
        "url": "https://tukaani.org/xz/xz-5.6.4.tar.xz",
        "archive": "xz-5.6.4.tar.xz",
        "extract_to": "xz",
        "note": "SquashFS xz 块（liblzma）",
    },
    {
        "name": "zstd",
        "version": "1.5.6",
        "url": "https://gh-proxy.com/https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz",
        "archive": "zstd-1.5.6.tar.gz",
        "extract_to": "zstd",
        "note": "SquashFS zstd 块",
    },
    {
        "name": "lz4",
        "version": "1.10.0",
        "url": "https://gh-proxy.com/https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz",
        "archive": "lz4-1.10.0.tar.gz",
        "extract_to": "lz4",
        "note": "SquashFS lz4 块（块格式，无帧头）",
    },
    {
        "name": "lzo",
        "version": "2.10",
        "url": "https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz",
        "archive": "lzo-2.10.tar.gz",
        "extract_to": "lzo",
        "note": "SquashFS lzo 块（1.x 的已知缺口，2.0 补齐）",
    },
    {
        "name": "freetype",
        "version": "2.14.3",
        "url": "https://gh-proxy.com/https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz",
        "archive": "freetype-2.14.3.tar.gz",
        "extract_to": "freetype",
        "note": "字形栅格 —— **对齐 Pillow 12.3.0 捆绑的 2.14.3**（字库产物保持原样的基准）",
    },
    {
        "name": "pcre2",
        "version": "10.48",
        "url": "https://mirrors.aliyun.com/debian/pool/main/p/pcre2/pcre2_10.48.orig.tar.gz",
        "fallbacks": [
            "https://mirrors.ustc.edu.cn/debian/pool/main/p/pcre2/pcre2_10.48.orig.tar.gz",
            "https://gh-proxy.com/https://github.com/PCRE2Project/pcre2/archive/refs/tags/pcre2-10.48.tar.gz",
        ],
        "archive": "pcre2-10.48.tar.gz",
        "extract_to": "pcre2",
        "note": "正则（文本提取用；std::regex 不支持 lookbehind）",
    },
    {
        "name": "pybind11",
        "version": "3.1.0",
        # 插件改装 P2（用户 2026-10-07 拍板：Python 模块接口 + C ABI 退役）：
        # 引擎要用 pybind11 把 C++ 能力暴露给随包自带的 CPython（插件＝`.py`）。
        # 规格要求 **vendored 进 third_party/**（原本写的是 `SOURCES.md` + `sources.lock.json`
        # 记来源与许可 —— 就是本脚本 + 本表）。
        "url": "https://gh-proxy.com/https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz",
        # 备用源：阿里云 PyPI 镜像的 **sdist**（仓库已在用的镜像，实测 200；
        # 官方 files.pythonhosted.org / pypi.org 本机全超时，见 SOURCES.md 的说明）。
        # ⚠ 两个源的 sha256 **不同**（打包方式不同）⇒ 锁记的是实际用上的那一个。
        "fallbacks": [
            "https://mirrors.aliyun.com/pypi/packages/76/f3/95b0f40b31df41dbfe6bb0857419c9442c15839cbac4796f1c26ae0b6081/pybind11-3.1.0.tar.gz",
        ],
        "archive": "pybind11-3.1.0.tar.gz",
        "extract_to": "pybind11",
        "note": "CPython 绑定（插件宿主；header-only，见 third_party/CMakeLists.txt 的 xrkit_pybind11）",
    },
    {
        "name": "python-embed",
        # ★★ P2c（2026-10-07，用户口径「**发布随包自带 embeddable Python**」）：
        #   版本必须与**构建期那个 Python 一致**（引擎链的是它的 import lib；当前 3.14.6）——
        #   门禁 `[python-embed]` 会拿 `sys.version_info` 与它对账。
        "version": "3.14.6",
        "url": "https://mirrors.aliyun.com/python-release/windows/python-3.14.6-embed-amd64.zip",
        # 备用源：华为云 python 镜像（python.org 官方布局的另一个国内镜像）
        "fallbacks": [
            "https://mirrors.huaweicloud.com/python/3.14.6/python-3.14.6-embed-amd64.zip",
        ],
        "archive": "python-3.14.6-embed-amd64.zip",
        "extract_to": "python-embed",
        # embeddable 包是**扁平**的（python314.dll / python314.zip / *.pyd 都在根）⇒ 不许剥顶层
        "strip_top": False,
        "note": "随包自带的 CPython 标准库与运行期（发行到没装 Python 的机器也能起插件宿主）",
    },
]

MAX_TRIES = 4


def say(msg: str) -> None:
    print(msg, flush=True)


MAGIC = (
    (b"\x1f\x8b", "gzip"),
    (b"\xfd7zXZ\x00", "xz"),
    (b"BZh", "bzip2"),
    (b"PK\x03\x04", "zip"),
)


def sniff_archive(path: Path) -> str | None:
    """按魔数判断这是个归档，还是镜像塞给我们的 HTML 拦截页。

    国内镜像对高频请求会回 "Verifying - USTC Mirrors" 之类**HTTP 200 的 HTML**
    （几百字节），只看状态码与大小是不够的。
    """
    try:
        head = path.read_bytes()[:512]
    except OSError:
        return None
    if len(head) < 512:
        return None
    for magic, name in MAGIC:
        if head.startswith(magic):
            return name
    return None


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_lock() -> dict:
    if LOCK.exists():
        return json.loads(LOCK.read_text(encoding="utf-8"))
    return {}


def save_lock(lock: dict) -> None:
    TP.mkdir(parents=True, exist_ok=True)
    # ★ 逐字节写 **LF**（`.gitattributes` 规定源码 LF）：`Path.write_text()` 在 Windows 上
    #   会把 `\n` 翻成 CRLF ⇒ 同一份锁在"刚生成"与"刚检出"两种状态下字节不同
    #   （本批实测：git status 里 SOURCES.md/sources.lock.json 立刻显示 CRLF 警告）。
    #   纪律同 `gen_encoding_tables.py`：生成物一律 bytes + LF。
    text = json.dumps(lock, indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    LOCK.write_bytes(text.encode("utf-8"))


def download(url: str, dest: Path) -> None:
    last = None
    for attempt in range(1, MAX_TRIES + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=60) as resp, dest.open("wb") as out:
                shutil.copyfileobj(resp, out, length=1 << 20)
            if dest.stat().st_size < 4096 or sniff_archive(dest) is None:
                raise OSError("内容不是归档（%d 字节，像是镜像的拦截页）" % dest.stat().st_size)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            last = exc
            # ★ 失败必须删掉半截文件：否则下一轮会看到"文件已存在"而当成功，
            #   把残留物当成依赖（本轮真踩过：873 字节的 USTC 拦截页进了锁）。
            dest.unlink(missing_ok=True)
            say("      第 %d/%d 次失败：%s" % (attempt, MAX_TRIES, str(exc)[:70]))
            time.sleep(2 * attempt)
    raise RuntimeError("下载失败：%s（最后一次：%s）" % (url, last))


def fetch_archive(src: dict, archive: Path) -> bool:
    """按 url → fallbacks 顺序试；任何一个成功即返回 True。

    国内镜像会对高频请求返回 "Verifying - USTC Mirrors" 这类**反爬拦截页**
    （HTTP 200、只有几百字节），所以 download() 里有"内容过小即失败"的判据，
    并在这里换成下一个源重试。
    """
    if archive.exists():
        if sniff_archive(archive) is not None:
            return True
        say("      已有文件不是归档（上一轮的失败残留），删除重下")
        archive.unlink()
    for idx, url in enumerate([src["url"]] + list(src.get("fallbacks", []))):
        if idx:
            say("      换备用源 #%d" % idx)
        try:
            download(url, archive)
            return True
        except RuntimeError as exc:
            say("      源失败：%s" % str(exc)[:110])
    return False


def extract(archive: Path, dest: Path, strip_top: bool = True) -> None:
    """解包并按需剥掉顶层目录（源码包都是单顶层目录）。

    ★ 2026-10-07（P2c）：新增 **zip** 支持与 `strip_top=False` —— 随包的 embeddable Python
      就是**扁平** zip（`python314.dll` / `python314.zip` / `*.pyd` 全在根），没有顶层目录，
      剥一层就什么都没了。判据靠**归档魔数**（`sniff_archive`），不靠扩展名猜。
    """
    if dest.exists():
        shutil.rmtree(dest)
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    if archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(archive) as zf:
            if not zf.namelist():
                raise RuntimeError("空包：%s" % archive)
            zf.extractall(tmp)
    else:
        mode = "r:xz" if archive.suffix == ".xz" else "r:gz"
        with tarfile.open(archive, mode) as tar:
            members = tar.getmembers()
            if not members:
                raise RuntimeError("空包：%s" % archive)
            tar.extractall(tmp)
    if not strip_top:
        tmp.rename(dest)
        return
    entries = list(tmp.iterdir())
    if len(entries) == 1 and entries[0].is_dir():
        entries[0].rename(dest)
        tmp.rmdir()
    else:
        tmp.rename(dest)


def write_sources_md(lock: dict) -> None:
    """把 provenance 落成文件：版本、源 URL、sha256、用途。"""
    lines = [
        "# 第三方依赖 provenance",
        "",
        "> 本文件由 `tools/fetch_third_party.py` 生成；改动请改脚本后重跑，不要手改。",
        "> 源码不入库（体积），锁文件 `sources.lock.json` 是复现依据：",
        "> `python tools/fetch_third_party.py --check` 会逐个校验 sha256。",
        "",
        "| 依赖 | 版本 | sha256 | 源 | 用途 |",
        "|---|---|---|---|---|",
    ]
    for src in SOURCES:
        entry = lock.get(src["name"], {})
        lines.append("| %s | %s | `%s` | %s | %s |" % (
            src["name"], src["version"], (entry.get("sha256") or "未拉取")[:16] + "…",
            entry.get("url", src["url"]), src["note"]))
    # ★★ 2026-10-07（P2 批踩到）：这一段**必须**由生成器写出来 —— 它原先是手工加在
    #   SOURCES.md 里的，而本脚本每次都会**整文件重写** ⇒ 我跑一次 `--only pybind11`
    #   就把它整段抹掉了（git diff 里 17 行消失）。生成物里凡是"人写的"部分，
    #   都得搬进生成器，否则"重跑生成器 = 静默删内容"。
    lines += [
        "",
        "## 另一类\"来源\"：编码表（不入 third_party，随仓库走）",
        "",
        "`src/engine/text/*_tables.inc`（cp1251 / cp1252 / gb18030 / gbk / gb2312 / **big5 /",
        "shift_jis / euc_kr**）**不是**第三方归档，而是由 `tools/gen_encoding_tables.py`",
        "从**本机 CPython 的同名 codec** 导出（`--check` 是门禁 [12]，表被手改或换 Python 版本即 FAIL）。",
        "它们的来源与许可是：",
        "",
        "- **来源**：CPython 标准库的 `encodings` / `_codecs_*` 各编码实现（`Modules/cjkcodecs/`、",
        "  `Modules/_codecsmodule` 等），表数据本身派生自 Unicode Consortium / 各标准（KS X 1001、",
        "  JIS X 0208、Big5）的公开映射。",
        "- **许可**：CPython 采用 **PSF License Agreement**（Python Software Foundation License，",
        "  GPL 兼容、允许再分发与派生）。派生成的数据表（`.inc`）按同一许可随本仓库分发。",
        "- **可复现**：`python tools/gen_encoding_tables.py`（生成）/ `--check`（校验）；",
        "  生成物一律 **bytes + LF** 写盘（`.gitattributes` 规定源码 LF，Windows 文本模式会翻成 CRLF",
        "  ⇒ 混着来会让 `--check` 在\"刚生成\"与\"刚检出\"两种状态下结论不同）。",
    ]
    lines += [
        "",
        "## 为什么不用 vcpkg 之类的包管理",
        "",
        "本机 **GitHub 被 hosts 指向 127.0.0.1**，且 **schannel TLS 整体不可用**",
        "（curl / Invoke-WebRequest / git 全挂，只有 Python/Node 的 OpenSSL 栈正常）。",
        "包管理器要联网解析与拉取，在这台机器上不可靠；因此改为「脚本 + sha256 锁」：",
        "首次联网拉一次，之后同机离线可重建。",
        "",
        "## 注意事项",
        "",
        "- 国内镜像对高频请求会返回 HTTP 200 的 HTML 拦截页（如 USTC 的 \"Verifying\" 页），",
        "  因此脚本按**归档魔数**判真伪，且下载失败会删掉半截文件（否则残留会被当成依赖）。",
        "- 锁只在「下载正确 + 解包成功」之后写入。",
    ]
    # 同样逐字节写 LF（理由见 `save_lock`）
    (TP / "SOURCES.md").write_bytes(("\n".join(lines) + "\n").encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="拉取 2.x 第三方依赖源码")
    parser.add_argument("--check", action="store_true", help="只校验已有产物，不下载")
    parser.add_argument("--only", default="", help="只处理名字含该子串的依赖")
    parser.add_argument("--update-lock", action="append", default=[],
                        help="有意换源时重写指定依赖的锁（可多次）")
    parser.add_argument("--force", action="store_true", help="重新解包（即使目录已存在）")
    args = parser.parse_args()

    TP.mkdir(parents=True, exist_ok=True)
    CACHE.mkdir(parents=True, exist_ok=True)
    lock = load_lock()
    failures = 0

    for src in SOURCES:
        name = src["name"]
        if args.only and args.only not in name:
            continue
        archive = CACHE / src["archive"]
        dest = TP / src["extract_to"]
        recorded = lock.get(name, {}).get("sha256")

        say("[%s %s] %s" % (name, src["version"], src["url"]))

        if args.check:
            if not archive.exists():
                say("      -- 本地无 tarball（需先跑一次拉取）")
                failures += 1
                continue
            actual = sha256_of(archive)
            if recorded and actual != recorded:
                say("      !! sha256 不符：锁 %s / 实际 %s" % (recorded[:16], actual[:16]))
                failures += 1
                continue
            say("      ok tarball（%s…） 已解包=%s" % (actual[:16], dest.exists()))
            continue

        if not fetch_archive(src, archive):
            say("      !! 所有源都失败")
            failures += 1
            continue

        actual = sha256_of(archive)
        if recorded and name not in args.update_lock and actual != recorded:
            say("      !! sha256 与锁不符（若是无意换源，请勿用 --update-lock 掩盖）：")
            say("         锁  = %s" % recorded)
            say("         实际 = %s" % actual)
            failures += 1
            continue

        need_extract = args.force or not dest.exists()
        if need_extract:
            say("      解包中…")
            try:
                extract(archive, dest, strip_top=src.get("strip_top", True))
            except Exception as exc:  # noqa: BLE001 —— 解包失败必须显式失败，不许当成功
                say("      !! 解包失败（不记锁、不保留坏包）：%s" % exc)
                archive.unlink(missing_ok=True)
                failures += 1
                continue

        # ★ 只有**下载正确 + 解包成功**之后才写锁：锁是从"可用产物"里长出来的，
        #   不是从"下载动作发生过"里长出来的。
        if not recorded or name in args.update_lock:
            lock[name] = {"version": src["version"], "url": src["url"],
                          "archive": src["archive"], "sha256": actual, "note": src["note"]}
            save_lock(lock)
            say("      已记录 sha256 %s" % actual[:16])

        size_mb = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file()) / 1048576
        say("      ok  %s（%.1f MB）" % (dest.relative_to(ROOT), size_mb))

    say("")
    write_sources_md(load_lock())
    if failures:
        say("FETCH: FAIL（%d 处）" % failures)
        return 1
    say("FETCH: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
