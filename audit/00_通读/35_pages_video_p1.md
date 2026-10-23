# `pages/video_page.cpp`（1228 行；本轮读 1-840）

---

## 1. ★★★ F1-1：`discard_process()` —— 放弃子进程对象的**唯一**实现（第 93-110 行）

```cpp
void VideoPage::discard_process(QProcess*& slot) {
    QProcess* process = slot;
    slot = nullptr;              // ★ ① 先置空（回调里的守卫立刻生效）
    if (process == nullptr) return;
    process->disconnect();       // ★ ② 再断开（kill/wait/deleteLater 期间绝不把 finished 派回来）
    process->deleteLater();      // ★ ③ 最后删
}
```

修前事实（注释原文）：
> `cancel_transcode` 的超时支只 `deleteLater(); process_ = nullptr;` 而**没有断开**，
> 而 `finished` 回调读的是**成员** `process_`（此刻已是 nullptr）⇒
> ① 对 nullptr 调 `deleteLater()`；② **取消超时后立刻再点转换 ⇒ 旧对象的回调把新的进程对象删掉**。

⇒ **顺序是"先置空、再断开、最后删"** —— 三步缺一不可。
  并且"析构、取消超时、起不来、回调自身收尾**全部**走这里 ⇒ 静态判据只需要盯住这一个函数"。

★ 这是本仓"**同一动作只留一个实现**"最典型的收益：4 个调用点原本各有各的错法。

## 2. ★★ P17-7：主线程不再长阻塞（`wait_for_start_probed`，第 297-313 行）

```cpp
static constexpr int kStartProbeMs = 500;   // 修前 10 000
if (process->waitForStarted(kStartProbeMs)) return true;
return process->state() != QProcess::NotRunning;
```

语义（保住同步契约、又不再长阻塞）：
- **true** ⇔ "这一刻没有确凿的启动失败"（可能仍在 Starting）；
- **false** ⇔ 进程**已经确定没起来**（`NotRunning`）。

★★ **两半缺一不可**：只缩短等待会把"晚一拍的失败"变成**静默** ⇒ 四个调用点都必须接
`&QProcess::errorOccurred`（`FailedToStart`）兜底，并把理由写到状态标签 + 宿主日志。

★ 取消路径同样改成**定时器兜底**（第 265-294 行）：
`kill()` 之后不再 `waitForFinished(5000)`（那会冻结界面 5 秒），而是
`QTimer::singleShot(5000, …)`；正常情况下 `finished` 已把 `process_` 置空 ⇒ 定时器空转。
判据：`run_ci` 的 shell-smoke **静态锁要求 `waitForFinished` 不再出现在取消路径上**
（只允许析构里那个 2 秒的收尾）。

## 3. 取消/失败/成功的三种结局都要说清（第 171-222 行）
- **取消** ⇒ 删半成品，且"删没删掉都要说出来"（`已删除半成品文件 X` / `删不掉 X（请手动删除）` / `没有半成品文件可删`）；
- **失败** ⇒ `ffmpeg 退出码 N（正常退出/异常终止）` + **stderr 末尾原文**；输出文件**保留**供排障（只有取消才删）；
- **成功** ⇒ 100% + `完成: <文件名>` + **再探一次产物**（`start_probe(output, Result)`）。

## 4. ★ F1-2/F1-3：起跑前的输出目标守卫（第 821-840 行）
```cpp
const OutputConflict conflict = check_output_conflict(request);
if (conflict != None) {
    if (conflict == Exists && confirm_overwrite(reason)) { log…; }
    else { last_error_ = (Exists ? "已取消：不覆盖已存在的输出文件 X" : reason); … return; }
}
```
理由：argv 里恒有 `-y` ⇒ 覆盖是**静默的**；修前"防自毁"只作用于**默认名**、且只跟**参考**比
⇒ 源本身是 `.ogm` 时默认输出＝源文件，叠上 `-y` 就是**把输入截断**。

## 5. ★ 默认输出名的"自毁"规避（第 575-594 行）
参考存在时输出落到**参考所在目录**（同主名）；若算出来正好等于**参考文件本身**
⇒ 改成 `<stem>.new.ogm`（"避免默认值指向输入"）。

## 6. 探测的并发保护与"跳过要留痕"（第 376-383、503-510 行）
- 同一时刻只允许一次探测；已有探测在跑 ⇒ `note_skip("上一份文件的参数还在解析中 ⇒ 暂不解析 X")`
  （用户红线：**不能静默**）；
- `apply_file` 里对"同一文件同一目标仍在解析"**直接 return 不记跳过**
  —— 注释区分得很清楚："那是同一件事，不是被丢掉的信息"。

## 7. 参数面板：**全部读引擎的结论**（第 674-722 行）
`plan_transcode(request)` 一次算出所有要显示的值：`video_encoder` / `scale_applied` /
`fps_applied` / `video_bitrate` / `audio_sample_rate` / `audio_channels` / `audio_bitrate` /
`audio_disabled` / `encoder_warning`。
页面**不再判一遍阈值/默认值** —— 否则会出现"面板说 OK、argv 里却插了 fps 滤镜"（23.98 vs 24.00 那类边界）。

★ `encoder_warning` 有专门一行：`⚠ 没有可用编码器表 ⇒ 回退内置 mpeg4（输出编码可能与参考不一致）`。

## 8. 空状态与"跳过清单"（第 737-751 行）
`skip_reasons_` 逐条列出（"══ 跳过 N 项（逐条原因） ══"）；
空面板返回 `拖入视频文件查看参数`（1.x 的空状态文案）。

## 9. `can_transcode()` 的三条件（第 763-766 行）
`source_probed_ && reference_probed_ && process_ == nullptr`
—— 注释："参考没落地也 ⇒ 灰（参考必需，且'灰着且不解释'是有意为之）"。

## 10. 编码器表未就绪时**不阻塞**（第 802-810 行）
`pending_transcode_ = true; start_encoder_scan();` ⇒ 扫描完成（或确定扫不到）之后
`complete_encoder_scan` 再触发 `request_transcode()`。

---

## 11. 这一页给我们的检查项

| 检查 | 为什么 |
|---|---|
| 子进程对象是否只有一处"放弃"实现 | F1-1：4 个调用点各有各的错法 |
| 主线程上是否还有 `waitFor*` 长阻塞 | P17-7：界面冻结 5~10 秒 |
| 缩短等待后是否补了 `errorOccurred` | 否则失败变静默 |
| 覆盖前是否问过（argv 有 `-y`） | 否则静默截断输入 |
| 面板值与 argv 是否同源 | 否则"面板 OK、argv 加了滤镜" |
