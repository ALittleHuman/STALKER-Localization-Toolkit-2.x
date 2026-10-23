# `tests/check_package_fixture_pages.py`（782 行，全文读完）—— ★★★ 20 次计数的**那个脚本**

> 这就是产出「19/20、20/20、8/20」这些数字的判据。它跑的是**包的临时副本**上的
> `xrkit_app.exe --smoke`，**两遍**：③ 默认形态、④ 把 `plugins\_fixtures\*` 抬到顶层再跑。

---

## 1. 四件事（文件头，第 12-27 行）
① 8 枚夹具带固定标记串 `NON-PRODUCT-FIXTURE`（"非产品不能只写在文档里"）；
② 发行口径包（RC/FINAL）里 `plugins\` 顶层与 `_fixtures\` 都必须为空；ALPHA/BETA 夹具落 `_fixtures\`
   （加载器**非递归**：`loader.cpp:108` / `py_host.cpp:107/3428` 三处都是 `directory_iterator`）；
③ **默认可见页＝六个内置页**：读 `PLUGIN-PAGES 可见页集合=…` 逐名比对；
④ **把 `_fixtures\` 抬上来再跑一次**：七个插件页 id 逐个点名、且**不再有内置页**。

## 2. ★★★ `FIXTURE_DEPENDENT`：哪些失败在 ③ 里算"未覆盖"（第 359-369 行）

```python
FIXTURE_DEPENDENT = ("P17-82", "P16-3b-2", "P16-3c", "P4 虚拟列表", "P4 拖入", "P4 任务壳", "P4 worker")
```
`fixture_only_failures(out)` ⇒ `(夹具依赖的失败行, 其余失败行)`；
③ 里若"其余为空且有夹具依赖失败" ⇒ 记 **未覆盖（NOTE）**，不是产品红（第 535-541 行）。

★ **注意 P16-3c 在这张名单里** ⇒ ③ 默认形态下它的失败被记成"未覆盖"；
   **④ 抬夹具形态下它照样要全绿** ⇒ 我们看到的 P16-3c 红，来自 **④** 那一遍。

## 3. ★★★ 文件头里那句**定性**（第 237-242 行）—— 正是我们三个红的现场

> ★★ 用户 2026-10-20 的取证要求：「查干扰 —— 杀软扫描」。实测线索：③ 的偶发红全是
> **插件页没建出来**（**P17-1 取消探针页 / P5 插件页视口为 0**），而插件是**刚写进 %TEMP% 的 .py**。
> 今天 360 把仓库加白之后，把私有 TEMP 也挪进**仓库内**（= 白名单路径）做对照实验：
> 若失败率从 8/20 掉到 0/20，就说明是"**杀软扫刚写的 .py 导致 import 偶发失败**"。

⇒ **P5 的红（"插件页视口为 0"）在这份代码里被明确列为"插件页没建出来"那一族**，
   而怀疑对象是**杀软扫刚写的 .py**。这条与我们这几轮的结论（占位页 / builder 被搅死）**互相印证**。

## 4. ★★ 私有 TEMP 的位置（第 234-249 行）
默认在 `ROOT/"build"`（仓库内 = 360 白名单内）；`XRKIT_PKTMP_DIR=system` 可切到 `%TEMP%` 做对照实验。
★ 并且"私有 TEMP 要**短**"：放进包副本里会把路径拉长，**插件夹具那条链在长路径上会出问题**
（实测：取消探针页的按钮找不到）。

## 5. ★★ 起跑前 `taskkill` 残留（第 168-176、231-233 行）
> P17-176 的教训：**残留进程会占用文件/目录**。

## 6. ★★ 两个"返 2（未覆盖）"的前置闸（第 198-216、439-464 行）

### （a）磁盘余量
> ★ 2026-10-20 现场教训：`XRKIT_KEEP_PKG_PAGES_TMP` 一开，失败的临时副本就不删；
> 连跑几十次后把 C 盘撑到 **0.1 GB** ⇒ 之后每次 `copytree` 都 `[WinError 112] 磁盘空间不足`，
> 表现为"**判据几乎全红**"。

⇒ `disk_space_ok(2.0)` 不够就打印"**未覆盖**（不是产品红；先清盘）"并 return 2。

### （b）★ 360 探针（`repo_write_allowed`）
> 本机 360「主动防御」会按"文件名＋仓库路径"拦 `vcruntime140.dll`
> （实测：改名片/换路径/先写 `.tmp` 再改名都被拒，`%TEMP%` 可写）⇒ 打包/自检会出现**一片红**，
> 而那是环境问题、不是产品问题。探针不过 ⇒ **未覆盖（return 2）**，绝不产出假红。

★ 这正是我们自己遇到的那条：**"仓库路径能不能写 DLL"是打包前必须探的**。

## 7. ★★ `package_looks_complete()`：输入不完整 ⇒ rc=2（第 279-305 行）
> P17-181 的取证：`pkg_pages_fail.txt` 显示抬夹具形态的 6~9 条失败全是"门面控件 0 个 /
> entry 关键字没生效" —— 那次判的是**上一次被中断的 `_pkg.py` 留在 `dist/` 里的半成品**
> ⇒ 判据本身没错，是**输入**不完整。

检查：`xrkit_app.exe` 存在且 ≥512 KB、`version.txt` 有 `version=`、`python\` 与 `bin\` 子目录都在。

## 8. `newest_package()`：输入钉死（第 308-342 行）
按 `version.json` 的 `tree` 里**最大序号**取（"只在成功发包时写、稳定"），没有才退回 `current.index`；
匹配到多个 ⇒ 报**歧义**并返回 None（"红绿就随目录内容漂"是禁止的）。

## 9. ★★ 失败现场取证（`capture_phase4_artifacts`，第 372-436 行）

拷出 11 个文件：`plugin_files.txt` / `page_trace_engine.txt` / `page_trace_shell.txt` /
`page_trace.txt` / `l2_trace.txt` / `build_error.txt` / `l2_marker.txt` / `cancel_marker.txt` /
`cancel_mark_error.txt` / `cancel_probe.py` / `l2_demo.py`；
**缺文件也要写一行"缺失"**（缺失本身就是证据）；日志类留**尾部 256 KB**。

★★ 还额外抽出一份 `diag_lines.txt`：
```python
diag_lines = [ln for ln in out.splitlines()
              if ("诊断" in ln or "plugin_errors" in ln or "插件问题" in ln or "已用占位页顶替" in ln)]
```
⇒ **"已用占位页顶替"就在这一行里** —— 这就是"我们的空页是不是占位页"最直接的证据文件。

## 10. ★★ ④ 抬夹具的两个自记坑（第 565 行）
> ★ 数量必须在**搬之前**数（我第一版放在搬之后 ⇒ 目录已空 ⇒ 恒 0 ⇒ **假红**）。

以及"lift 必须搬完：少一个 ⇒ 现场就不对，别把它当'smoke 飘'"。

---

## 11. 对我们三个红的**直接意义**（可执行）

| 红 | 该去看什么 |
|---|---|
| **P5（插件页视口为 0）** | `build/logs/pkg_pages_phase4_artifacts_*/diag_lines.txt`（找"已用占位页顶替"/"插件问题"）＋ `plugin_files.txt`（夹具是否落盘/size/sha256）＋ `page_trace_shell.txt` |
| **⑭（扫到 0 个可见字段行）** | 同一份 `stdout_tail.txt` 里找 `⑭` 行的数字；再看 `page_trace_shell.txt` 里 `p5_page|… current_eq_page=…` |
| **P16-3c（菜单两项未就绪）** | `l2_marker.txt`（`menu_ok` 有没有写出）＋ `diag_lines.txt`（"P16-3c 诊断：等两项菜单 2000ms 未就绪（actions=…）"） |

⇒ 三个红**各有专门的证据文件**，不必再靠猜。
