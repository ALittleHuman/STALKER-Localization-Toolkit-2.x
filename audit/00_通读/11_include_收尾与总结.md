# include 收尾批：video/transcode(146) ＋ video/ffmpeg(72) ＋ process/run(61) ＋ patch/patch(161) ＋ engine.h(44) ＋ third_party_probe(26)

> **`include/` 至此全部读完：46 个文件。** 本批 6 个，合计 ~510 行。

---

## A. `video/transcode.h`（146 行）

### 1. 判据 = 与 `tests/golden/video_argv.json` **逐项相等**
那份 golden 是"**exec 1.x `convert()` 本体实调**"录下的 argv（第 5-6 行）。
⇒ 又是"对着真值来源对拍"，不是对着另一套近似实现。

### 2. ★ 面板值与 argv **必须同源**（第 64-66 行）
`TranscodePlan` 里的 `scale_applied / fps_applied / video_bitrate / audio_bitrate /
audio_sample_rate / audio_channels / audio_disabled` **全由 `plan_transcode` 决定**
⇒ 面板直接读，**不许在页面里把阈值/默认值再判一遍**
（否则 23.98 vs 24.00 那类边界会出现"**面板说 OK、argv 里却插了 fps 滤镜**"）。

⇒ 这就是"**同一事实两处各算一次**"的又一次（第 5 例）。我们的判据也常常自己复算
  "页应该是什么样"，而产品另有一套 —— **同族**。

### 3. ★★ F1-2/F1-3：`-y` 让覆盖变成**静默的**（第 81-98 行）
`plan_transcode` 照抄 1.x，argv 里**恒有 `-y`** ⇒ ffmpeg 不问、页面也不问。
所以"输出已存在"与"输出就是输入"必须在**起跑前**由引擎判定：
`OutputConflict{None, SameAsSource, SameAsReference, Exists}`，
判定顺序 `SameAsSource → SameAsReference → Exists`（**前两条最危险，优先报**）。
只做**规范化路径比较**（`core::path_equals`）与**存在性**，不读内容、不做别的推断。

⇒ **"工具链默认带 `-y`"是一个静默危险**；这正是"零静默"要补的地方。
  我们的打包/解包路径也有"默认覆盖"（`overwritten` 台账已补）。

### 4. ★ 进度口径（第 119-134 行）：上限 **99.5**、没有下限
`percent = min(cur/duration*100, 99.5)`；`CancelToken` 只有 `reset()` 能清零
（1.x `convert()` **不清零** `_cancelled`）—— **照抄**，否则"取消后重跑"行为不同。

⇒ 这条与 P17-1② 直接相关：**取消位何时清零**是行为契约的一部分。
  我们的 `TaskShell` 每次 `run()` 都 `cancel_requested_.store(false)`（清零），
  与 1.x 视频页的"不清零"**不同** —— 但那是 TaskShell 的设计，需要确认是有意的。

### 5. 取消**不留半成品**（第 122-123 行）
1.x：取消就 `terminate()` + `wait()` + **删掉输出文件** + 返回 `(False, "已取消")`。

⇒ 与 `text/extract.h` 的"停在完整产物上"一致：**取消的验收标准是产物状态**。

---

## B. `video/ffmpeg.h`（72 行）—— 定位与版本守卫

- **只认两处**：`user.ltx [ffmpeg] path/probe`（必须真实存在）与**程序目录**下的
  `ffmpeg.exe`/`ffprobe.exe`；**不搜 PATH、不 import imageio**
  —— 理由：到处找正是启动变慢（冻结包 ~373 ms）与"到底用了哪个 ffmpeg 说不清"的来源；
- 两侧**独立解析**；8.2 探测**惰性**且**失败也置位**（否则每点一次按钮重弹一次窗）；
- 8.5 **三态** `NotProbed / Found / Missing`：**探测之前不许先画 ✗**；
- 8.6 版本守卫：显式拒绝 ffmpeg **8.x**（8.x 的 theora 编码器对 STALKER OGM 有 bug）；
- 8.7 **只有手动指定的**才写进 `user.ltx`，自动查找到的**不写盘**。

⇒ 又是**三态**（`NotProbed`）——"还没测"不许显示成"缺失"。这与 `LinkProbe::StatFailed`
  是同一条纪律的第 3 次出现。

---

## C. `process/run.h`（61 行）—— 跑外部进程的唯一实现

四条产品语义（第 4-8 行）：**取消要真停** / **退出码如实** / **stderr 尾巴看得见** /
**不许弹控制台**（`CREATE_NO_WINDOW`，门禁 `tests/check_no_console.py` 按文件判）。

- ② **双管道不读就会死锁** ⇒ stderr 走一个线程专门读；
- ③ 取消：先 `TerminateProcess`，再等 `cancel_wait_ms`（默认 5000）；**没死透要如实报**
  `cancel_timeout=true`（不假装停了）；
- ④ `exit_code` / `normal_exit` 一律如实（**不把"起不来"混进"退出码非 0"**）；
- ⑤ `stderr_tail` 只留尾巴（默认 8 KiB）。

### ★★ P17-11（第 54-59 行）：把"参数表 → 命令行"暴露出来
修前这一手只藏在匿名命名空间里，于是"**参数以反斜杠结尾时收尾引号被吃掉**"这条缺陷
**没有任何判据盯得住**。现在 `build_command_line()` 公开，规则与 Python
`subprocess.list2cmdline` 逐字节一致；`run()` 内部走的就是它（唯一一份）。

⇒ **这条与 `case_fold.h` 的 `path_lower` 是同一个教训**：
  **判据够不到的东西 = 没有人保护的东西。** 所以"把关键步骤暴露成可独立调用的函数"
  不是洁癖，是判据可写的前提。

---

## D. `patch/patch.h`（161 行）—— 二进制补丁

### 1. 通用机器 + 表由调用方给（第 4-9 行）
表是"**单一构建的特化数据**"（1.x 原话："换一个模组、换一次引擎编译，这些数字就全部作废"）；
本仓红线：**不许拿代理指标/特化数据当语义判据** ⇒ 构建相关数据不焊进内核。

### 2. ★★ 判定纪律：**绝不看"差了多少字节"**（第 11-14 行）
实测 v3 与 v4 的差异字节数**完全相同（都是 88）** ⇒ 字节数既分不出正确/缺陷形态，
也证明不了打对了位置。`reference_diff_bytes` **只作报告参考值**。

⇒ **"代理指标"（差了几个字节 / 跑了多少毫秒 / 转了几圈）永远不能当判据** ——
  这是本仓反复强调的红线，也正是我们"用 2 秒等待当完成判据"的同一个错误。

### 3. 二进制字段一律 hex 文本（第 16-17 行）
pybind11 的 `std::string` 走 UTF-8，直接传裸字节会被改坏；解析大小写不敏感、允许空格。

### 4. 守卫链 ①~⑥ + 后置自检 ⑦（第 123-135 行）
① PE 布局（目标偏移都落在 `.text` 原始数据内 + 目录项显式 layout 声明）；
② preserve 项在输入里本来就完好；③ 每项 `expect` 逐字节命中；
④ 每项**周边锚**命中（防单字节碰撞/防打错位置）；⑤ helper 空闲区**全零**且起点不在前一条指令跨度内
（v1/v2/v3 的翻车点）；⑥ call 项自洽（`E8 rel32` 逐字节等于目录记录的 patched）；
⑦ 后置自检：逐项终态 == patched **且实际改动集合 == 目录声明的写入集合**。

★ 任何一条守卫不满足 ⇒ `ok=false` + 逐条 reason/notes，**不产生任何副作用**。

⇒ **这是"守卫链 + 后置自检"最完整的样板**：
  - 前 6 条是**前置**（防打错），第 7 条是**后置**（独立于定位方式，兜底）；
  - "实际改动集合 == 声明集合"这个断言，等于"**没有任何意外写入**"。
  ⇒ 我们的解包/打包路径应当有同形的断言（写入集合 == 计划集合）。

### 5. `refused_input_is_output` / `refused_output_exists`（第 150-151 行）
具名的拒绝原因，前端据此问"要覆盖吗" —— 又是"**拒绝是一等状态**"。

---

## E. `engine.h`（44 行）：**模块数不许自报常量**

审计原文：「`engine::status_line()` 的**常量自报**」。
修前 `status_line()` 直接返回字面量 `" ready=1 modules=5/5"` —— 与"到底编进来几个模块"毫无关系
（实测 `src/engine` 下有 6 个模块，报告里还是 5/5）。

现在：
- 模块**清单**是字面量列表，**必须与 `src/engine/` 下模块目录一一对应**（自检逐名核对）；
- 模块**计数**来自"真链上的探针"（每个模块定义 `probe_<模块>()`）—— 摘掉一个就少一个；
- `--module-subset` 能真的收窄 ⇒ `status_line()` 的数字**必须跟着变**，否则就是自报常量
  （判红锁见 `tests/run_ci.py` 的 `engine-status` 步）。

⇒ **"自报常量"是本仓最典型的假绿形态**：报告说自己 5/5，实际 6 个模块。
  ⇒ 我们的 `SHELL-SMOKE ok checks=935` 里的 **935 也是自报**（数出来的 check 数）——
  这个数**不能证明覆盖了什么**，只能证明"跑了 935 次检查"。**必须与判据清单对账**。

---

## F. `third_party_probe.h`（26 行）

"源码拉下来了""能编过"都不等于"能用" ⇒ 每个依赖跑一次**真实往返**，把"可用"变成可量判据。
`ProbeResult{ok, skipped, detail}`：`skipped=true` = 环境缺件（如系统无字体），
**不算失败，但必须显式说出原因**。

⇒ 与 `run_python_host_probe` 的 rc=2 同族：**"跳过"必须是显式状态**，不许混进"通过"。

---

## G. `include/` 全部读完后的总结（46 个文件）

十条反复出现的**结构性教训**（每一条都至少两处实例）：

1. **信号必须结构化**（`truncated` / `user_cancelled` / `needs_confirm` / `ProbeState` /
   `LinkProbe::StatFailed` / `OutputConflict` / `refused_*`）—— 二态必丢"不知道"。
2. **两份实现必分叉**（`selection_matches` / `host_run_process` / `read_bytes` / 落点规则 /
   `system_font_dirs` / `path_lower`）—— 已数到 6 例。
3. **判据要考"在用的那个函数"**（`case_fold.h:138-141`、`process/run.h:54-59`）。
4. **不量代理指标**（`reference_diff_bytes` 只看不用；`full_detail_calls` 计数替代耗时；
   `atomic_write` 的 `record` 回调替代"耗时上界"）。
5. **判据样本本身是判据的一部分**（`cjk_sample` 的"永你好汉化测"漏简体特有字）。
6. **量词必须与用户语义对齐**（`paths_carry_shell` 从"全部"改"过半"）。
7. **量纲/取整跨语言差异**（Python `//` vs C++ `/` 在负数上差 1；`splitlines` 的结尾边界）。
8. **默认路径不许动**（`PackOptions.compressor` 默认 Xz；黄金对拍基线）。
9. **行为不随诊断参数变化**（`error` 出参不是开关；trace 只影响写不写）。
10. **"自报常量"是假绿**（`status_line` 的 5/5；我们的 `checks=935`）。

## H. 下一步

进 `src/`（105 个文件 / 43,785 行）。顺序：
`src/core/**`（最小，先熟）→ `src/engine/plugin/**`（py_host/widget_facade 实现，最相关）→
`src/engine/fs|text|font|video|patch|process/**` → `src/shell/**`（Hub/pages/facade 实现）→
`src/cli/**` → `tests/**` → `scripts/**`。

★ 优先读 `src/engine/plugin/widget_facade.cpp`（我们加了大量探针的那个文件）与
`src/shell/hub.cpp` 的 `sync_pages()`（P5 的根）。
