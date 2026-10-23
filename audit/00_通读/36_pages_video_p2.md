# `pages/video_page.cpp` 后半（第 841-1228 行）—— 全文读完

---

## 1. `textChanged` 的"同一件事不重复做"（第 905-927 行）

```cpp
if (trimmed == last_source_request_) return;      // ★ 同一个文本只处理一次
last_source_request_ = trimmed;
if (info.exists() && info.isFile() && trimmed != source_path_) apply_file(trimmed, false);
```

两条注释都很有价值：
- **只对"真存在的文件"起探测** —— "打字打到一半的路径不存在 ⇒ 不探，免得把半截路径解析失败当成结论"；
- **`textChanged` 对一次改动可能来两趟**（实测）：不挡住的话第二趟会在第一趟刚结束时又起探测，
  把随后那份文件的探测挤成"跳过" —— **面板上就多出一条莫名其妙的跳过记录**。

⇒ 这是"**用户可见的噪声来源于重复事件**"的实例。判据若统计"跳过条数"，会被这个坑影响。

## 2. 面板字体：`setStyleHint(Monospace)` 而不是"找系统等宽字体"（第 935-942 行）
> 离屏自检机器上 `QFontDatabase::systemFont(FixedFont)` 很容易落到**没有字形的字体**上
>（实测：面板整块空白方块，甚至 `QTextDocument` 布局里**直接崩**）。

⇒ 与 `theme.cpp` 的"不用 `QFontDatabase` 找等宽"是同一条教训。

## 3. ★ 三态状态栏（第 990-1008 行，8.5）
| 状态 | 文案 |
|---|---|
| `NotProbed` | `未检测 ffmpeg` —— **探测之前不显示 ✗**（"那就是在断言'没有'，而我们还没查"） |
| `Found` | `✓ ffmpeg: <path>`（缺 ffprobe 时追加 `（缺 ffprobe —— 视频信息读不出来）`；手动指定时追加 `（user.ltx 手动指定）`） |
| `Missing` | `✗ 没找到 ffmpeg（只认 user.ltx 与程序目录）—— <试过哪些路径>` |

## 4. `set_running_ui` 还禁了「设置 ffmpeg」（第 1023-1031 行）
> 否则**转换途中换掉 ffmpeg，正在跑的进程就成孤儿**。

⇒ 与 `discard_process` 同一条纪律：**不许留下没人管的子进程**。

## 5. 设置对话框是**非阻塞**三个按钮（第 1050-1092 行，8.3）
`setModal(false)` + `show()`（不是 `exec()`）；三个按钮：**自动安装 / 手动选择 / 稍后**。
★ 手动选择时：`ffprobe.exe` 若不存在 ⇒ `probe.clear()`（"缺 ffprobe 如实报，不假装有"）。

## 6. ★★ 自动安装的三条路都**真做事、结论可见**（第 1094-1209 行，8.4）
1. **随包自带** `ffmpeg.exe` 在程序目录 ⇒ `locator_.invalidate()` + 重新 `detect()`，
   并如实说"已重新探测：✓ / 还是没认它"；
2. **脚本**（`download_ffmpeg.py`，**根优先**、其次 `bin/`）⇒ 真跑 `python <script>`，
   完成后 `invalidate + detect`，把退出码 + 重新探测结论 + 输出末尾 300 字**都写出来**；
3. **都没有** ⇒ `自动安装不可用：…（不联网、不静默自救）` + 试过的路径。

★ 三个"放弃对象"点和 `run_auto_install` 的并发保护（`install_process_ != nullptr` ⇒
"自动安装已经在跑"）都走 `discard_process`。
★ 脚本子进程是**成员**（`install_process_`），由 `~VideoPage` 的同一个 `stop()` 收
——修前它是局部对象：析构不停、回调还会碰已析构的 `status_`。

## 7. 三个对话框起点（第 1211-1226 行）
- 源 / 参考：`dialog_start_for(edit.text(), {}, false)`（继承 → 工具目录）；
- **输出**：`dialog_start_for(QString(), {source_edit_->text()}, true)` —— 源是**文件** ⇒ 用其所在目录；
  且**输出框已有值 ⇒ 继承优先**（`spec.inherited = QFileInfo(current).absolutePath()`）。

---

## 8. video_page 全文给出的检查项（合并 35 号笔记）

| 检查 | 为什么 |
|---|---|
| 子进程"放弃"是否只有一处实现 | F1-1：4 处各有各的错法（含删错新对象） |
| 主线程是否还有 `waitFor*` 长阻塞 | P17-7：冻结 5~10 秒 + Windows"无响应" |
| 缩短等待后是否补 `errorOccurred` | 否则失败变静默 |
| 覆盖前是否问过（argv 有 `-y`） | 否则静默截断输入 |
| 面板值与 argv 是否同源 | 否则"面板 OK、argv 加了滤镜" |
| 转换中是否禁用了"设置 ffmpeg" | 否则进程成孤儿 |
| 探测前是否显示 ✗ | 三态缺失 = 断言未发生的事实 |
| 是否用了 `QFontDatabase::systemFont(FixedFont)` | 离屏机器上可能崩 |
| `textChanged` 是否做去重 | 否则面板多一条莫名"跳过" |

## 9. 待继续

`pages/fs_page.cpp`（1952，将分 4~5 轮）、`font_picker.cpp`(348)、`smoke.cpp`(10390)。
