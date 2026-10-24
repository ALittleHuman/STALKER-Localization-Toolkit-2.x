# 通读台账 01：`src/engine/process/run.cpp`（290 行）

> 2026-10-24。**只记录：问题 / 调用关系 / 风险 / 证据。不做顺手重构。** BETA.2 保持可比。

## 一、它是干什么的（一句话）

**跑外部进程的唯一实现**：`CreateProcessW` + `CREATE_NO_WINDOW`，双管道用 `PeekNamedPipe`
轮询读（不起读线程），取消用**作业对象**杀整棵树，退出码/stderr 尾巴如实报。

## 二、调用关系

```
src/engine/plugin/host_facade_impl.cpp:38-47   把门面参数搬进 RunOptions → eprocess::run()
src/engine/plugin/engine_facade_impl.cpp       只传 should_stop（钩子透传）
src/engine/plugin/py_host.cpp:593/658/845       Python 侧构造 should_stop / cancel_wait_ms
src/engine/text/*（视频/编码路径）             经门面间接用（引擎内不直接 include run.h 的只有 facade）
include/xrkit/engine/process/run.h             公开契约（RunOptions / RunResult / build_command_line）
tests/check_no_console.py                      按文件判"必须有 CREATE_NO_WINDOW"
smoke（引号表）                                判 build_command_line 与 Python list2cmdline 逐字节一致
```
★ 关键设计：`build_command_line` 被**特意暴露**给自检（P17-11），所以"参数拼装"有判据盯着；`run()` 内部只走这一份。

## 三、语义要点（读出来的）

| 项 | 实现 | 证据 |
|---|---|---|
| 不弹控制台 | `flags = CREATE_NO_WINDOW` | :194 |
| 整棵树可杀 | Job Object + `KILL_ON_JOB_CLOSE`；取消时 `TerminateJobObject` | :176-181、:221 |
| 不卡在读线程 | 两条管道同循环 `PeekNamedPipe` 轮询 | :114-131、:231-233 |
| 收尾排空 | 退出后 ≥300ms 且两管道空 ⇒ 收；最多 2s 兜底 | :238-250 |
| stderr 尾巴 | `append_tail` 只留最后 N 字节（默认 8 KiB） | :30-35、:232 |
| 取消如实 | 5s 没死透 ⇒ `cancel_timeout=true` | :225-229 |
| 起不来 vs 退出码 | `started=false` + `error`，不混进 `exit_code` | :201-207 |
| 非 Windows | 明确报"未实现"，不糊一条没测过的路径 | :281-287 |

## 四、问题与风险（按严重度；**本批不改**）

### P1（中）：`normal_exit` 用 `exit_code != STILL_ACTIVE` 判"正常退出"——`259` 是合法退出码

```cpp
result.normal_exit = !result.cancel_timeout && exit_code != STILL_ACTIVE;   // :268
```
`STILL_ACTIVE == 259`。**子进程真返回 259** 时会被判成"没正常退出"。
- 何时会踩：插件脚本 `sys.exit(259)`、某些工具用 259 表示自定义状态（非零但合法）。
- 现状判据覆盖不到（`smoke` 只判引号表与起停，没判"退出码 259"这一格）。
- 建议（下批）：改用"**已经 `WaitForSingleObject` 确认退出**"这一事实，而不是拿退出码反推；
  或加一条自检：跑一个 `exit 259` 的子进程，断言 `normal_exit == true`。

### P2（中）：**无换行的输出会让 pending 无界增长**（stdout 与 stderr 都中）

`feed_lines()` 只在遇到 `'\n'` 时才 `erase` 已消费部分（:102-110）。`stderr_tail` 有 8 KiB 上限，
但那是**结果字段**的上限，**pending 缓冲没有上限**。
- 真实现场：`ffmpeg -stats` 的进度是 `\r` 刷行、**不带 `\n`** ⇒ `err_pending` 会一直涨。
- 长跑（几小时编码）+ 高频进度 ⇒ 内存线性增长（不是泄漏，但同样会 OOM）。
- 建议（下批）：给 pending 加硬上限（例如 64 KiB），超限时**强制切一行**交出去（并计数）。

### P3（低）：子进程 `stdin` 是 `nullptr`，与 1.x 的 Python `subprocess` 行为不同

`startup.hStdInput = nullptr` + `STARTF_USESTDHANDLES`（:185-187）。Python 的 `subprocess`
默认**继承父 stdin**。⇒ 若将来跑会读 stdin 的工具（交互式提示、`y/n` 确认），我们的子进程会立刻 EOF。
- 现状无影响（ffmpeg/python 都不吃 stdin），但这是**接口语义差异**，写在这里免得以后踩。
- 建议：要么显式继承 `GetStdHandle(STD_INPUT_HANDLE)`，要么在 `run.h` 写明"stdin 关闭"。

### P4（低）：`CreateJobObject` 失败是**静默降级**

:176-181：job 为 `nullptr` 时就不设 `KILL_ON_JOB_CLOSE`，取消退化成 `TerminateProcess`（只杀子进程，
孙进程还攥着管道）⇒ 靠 :247 的 2 秒兜底 break，**可能丢尾部输出**，且调用方看不出"这次没有作业对象"。
- 建议：`job == nullptr` 时在 `result.error` 或新字段里**留痕**（当前是零痕迹）。

### P5（文档漂移）：`run.h` 的注释与实现不同源

`run.h:14` 写"stderr 走一个线程专门读"、`:32` 写"每 20 ms 问一次"；
实现是 **PeekNamedPipe 轮询、无读线程**（run.cpp:6-10 的 P12b 注释已说明），循环间隔是 **10 ms**（:251）。
- 这正是 `comment-parity` 那条判据管的形态，但判据只覆盖了它登记过的那几处。
- 建议：改 `run.h` 的这两句（**注释改动，零行为变化**；下批一起做，单独提交）。

### 已核对、不算问题

- `quote_arg` 的反斜杠翻倍规则与 Python `list2cmdline` 一致（有 smoke 引号表 + 4000 随机串对拍）；
- 双管道都读 ⇒ 不会死锁；
- 读端 `SetHandleInformation(..., 0)` 去继承、父侧写端立刻关 ⇒ 不会因"父进程攥着写端"而无 EOF。

## 五、与 1.x 的差异（需要知道的）

| 维度 | 1.x（Python `subprocess`） | 我们 |
|---|---|---|
| 取消 | `terminate()` 只杀直接子进程（孙进程可能残留） | **作业对象杀整棵树** |
| stdin | 继承父进程 | **nullptr**（见 P3） |
| stderr | 整段捕获或线程读 | **尾巴 8 KiB + 逐行回调** |
| 命令行拼装 | `list2cmdline` | **同一规则**（逐字节对拍过） |

## 六、下一步（本文件之后）

按用户排的顺序：`src/engine/fs/db.cpp`（966 行）→ 其余 `src/engine` → `src/shell` 六页与 `hub.cpp`/`smoke.cpp` → `tests/` 112 → `scripts/`+`tools/`。
**本页只留证据与建议，不动代码。**
