# 第三方依赖 provenance

> 本文件由 `tools/fetch_third_party.py` 生成；改动请改脚本后重跑，不要手改。
> 源码不入库（体积），锁文件 `sources.lock.json` 是复现依据：
> `python tools/fetch_third_party.py --check` 会逐个校验 sha256。

| 依赖 | 版本 | sha256 | 源 | 用途 |
|---|---|---|---|---|
| zlib | 1.3.1 | `17e88863f3600672…` | https://gh-proxy.com/https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz | SquashFS gzip 块 + 通用压缩 |
| xz | 5.6.4 | `829ccfe79d769748…` | https://tukaani.org/xz/xz-5.6.4.tar.xz | SquashFS xz 块（liblzma） |
| zstd | 1.5.6 | `30f35f71c1203369…` | https://gh-proxy.com/https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz | SquashFS zstd 块 |
| lz4 | 1.10.0 | `537512904744b35e…` | https://gh-proxy.com/https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz | SquashFS lz4 块（块格式，无帧头） |
| lzo | 2.10 | `c0f892943208266f…` | https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz | SquashFS lzo 块（1.x 的已知缺口，2.0 补齐） |
| freetype | 2.14.3 | `dc49de6b01a266ee…` | https://gh-proxy.com/https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz | 字形栅格 —— **对齐 Pillow 12.3.0 捆绑的 2.14.3**（字库产物保持原样的基准） |
| pcre2 | 10.48 | `ebcc25aadf2a51fa…` | https://mirrors.aliyun.com/debian/pool/main/p/pcre2/pcre2_10.48.orig.tar.gz | 正则（文本提取用；std::regex 不支持 lookbehind） |

## 另一类"来源"：编码表（不入 third_party，随仓库走）

`src/engine/text/*_tables.inc`（cp1251 / cp1252 / gb18030 / gbk / gb2312 / **big5 /
shift_jis / euc_kr**）**不是**第三方归档，而是由 `tools/gen_encoding_tables.py`
从**本机 CPython 的同名 codec** 导出（`--check` 是门禁 [12]，表被手改或换 Python 版本即 FAIL）。
它们的来源与许可是：

- **来源**：CPython 标准库的 `encodings` / `_codecs_*` 各编码实现（`Modules/cjkcodecs/`、
  `Modules/_codecsmodule` 等），表数据本身派生自 Unicode Consortium / 各标准（KS X 1001、
  JIS X 0208、Big5）的公开映射。
- **许可**：CPython 采用 **PSF License Agreement**（Python Software Foundation License，
  GPL 兼容、允许再分发与派生）。派生成的数据表（`.inc`）按同一许可随本仓库分发。
- **可复现**：`python tools/gen_encoding_tables.py`（生成）/ `--check`（校验）；
  生成物一律 **bytes + LF** 写盘（`.gitattributes` 规定源码 LF，Windows 文本模式会翻成 CRLF
  ⇒ 混着来会让 `--check` 在"刚生成"与"刚检出"两种状态下结论不同）。

## 为什么不用 vcpkg 之类的包管理

本机 **GitHub 被 hosts 指向 127.0.0.1**，且 **schannel TLS 整体不可用**
（curl / Invoke-WebRequest / git 全挂，只有 Python/Node 的 OpenSSL 栈正常）。
包管理器要联网解析与拉取，在这台机器上不可靠；因此改为「脚本 + sha256 锁」：
首次联网拉一次，之后同机离线可重建。

## 注意事项

- 国内镜像对高频请求会返回 HTTP 200 的 HTML 拦截页（如 USTC 的 "Verifying" 页），
  因此脚本按**归档魔数**判真伪，且下载失败会删掉半截文件（否则残留会被当成依赖）。
- 锁只在「下载正确 + 解包成功」之后写入。
