# shell 批四：`split_pane.cpp`(325) ＋ `row_action_delegate.cpp`(73) ＋ `drop_zone.cpp`(49) ＋ `dialog_start.cpp`(71) ＋ `screen_scale.cpp`(89) ＋ `fake_ffmpeg.cpp`(97)

---

## A. `split_pane.cpp`（325 行）—— 分栏模型

### 1. 三条"实测踩到"的规则

**（a）新格子加入时先把已有格子收敛到各自的最小值**（第 35-40 行）
否则先加的那格独吞整幅空间，后加的只剩 min ⇒ **边界拖不动**
（实测："2 格分栏里 sash 拖不动，只有'后面的先挨饿'在起作用"）。

**（b）每格上限要在**每次** clamp 时先压一遍**（第 115-120 行）
`sizes_` 跨次保留：只在"富余分配"里按上限发不够 —— 上一次给多了之后下一次走**收窄分支**，
而收窄分支不会把超限的格子压回去（实测日志格 636 一直不收敛）。

**（c）弹性项不能取"最大的那个"**（第 142-152 行）
`add_pane` 逐个来 ⇒ 第一格先独吞，之后"第一格有余量、第二格只有 min" ⇒ 拖不动边界。
正确：**最后一个没收起的 pane**；而"展开一个刚收起的"场景里要从**余量**看，
不能简单取最后一个（第 104-105 行原话："展开后又变 0"）。

### 2. `clamp_now()` 的两条分支（第 107-177 行）
- `sum > total`：**从后往前**从有余量的 pane 收（后面的先挨饿）；还不够则从后往前压到 0；
- `sum < total`：从后往前**按每格上限**发放（`maxs_[i]==0` = 不限），吃到上限换下一格；
  全到顶还有剩 ⇒ 兜底给"弹性项"，保证用满。
- `++clamp_count_` —— **可断言的计数器**（判"多少次 clamp 后收敛"用它，不是量时间）。

### 3. 拖动：**拖动期不改几何**（2.1）
`begin_drag` 只 `update()`；`update_drag` 只改 `preview_` + `update()`；
`end_drag(commit)` 才**一次**重排，且差额只在**相邻两格**之间搬：
```cpp
const int pair = sizes_[index] + sizes_[index+1];
a = clamp(sizes_[index] + delta, floor_a, max(floor_a, pair - floor_b));
sizes_[index+1] = pair - a;      // 总和不变
```
注释解释了为什么必须"总和不变"：否则 clamp 会按"后面的先挨饿"把差额从别处再收一遍，
**位置就回不到目标**。

### 4. `set_collapsed`：**先记用户意图，再 clamp**（第 269-281 行）
反了会被立刻推回。收起时把 `mins_` 存进 `saved_mins_` 并置 0（"改窗口尺寸也不许弹回"）。

### 5. 去抖 120 ms（第 194-215 行）
`resizeEvent` 只 `request_clamp()`；`timerEvent` 到点才 `relayout()`。

---

## B. `row_action_delegate.cpp`（73 行）—— 行尾 ✕ 的唯一实现

两条硬要求（写死在注释里，来自 fs 页真事故）：
1. **画与命中同源**：`action_rect()` 只有一份，`paint`/`editorEvent`/`sizeHint` 都用它
   ⇒ 不会"看得见点不着"；
2. 命中后**返回 true 吃掉事件** ⇒ 不会退化成"点了一下整行"（选择语义 ≠ 删除语义）。

`action_rect`：贴行尾（右边缘留 `px(8)`），`box = px(20)`；`sizeHint` 给 `px(30)`。

⇒ 这是"**同源几何**"的样板：判据若自己算一遍位置，就会与实现分叉。

---

## C. `drop_zone.cpp`（49 行）—— `field_row` 的唯一来源

```cpp
FileDropFilter* DropZones::attach_row(QWidget* container, QWidget* filter_owner) {
    if (container && container->objectName().isEmpty())
        container->setObjectName("field_row");     // ★ 六页的所有字段行都在这里命名
    return attach_zone(container, ..., [container](paths){ 按顺序填该行的 QLineEdit });
}
```

⇒ **⑭ 判据扫的 `field_row` 就是这里命名的**（`theme.cpp` 里那条 `QWidget#field_row{transparent}`
也是为它写的）。**插件页没有 `field_row`**（插件用门面的 `row`/`pack` 自己排）⇒
⑭ 在插件页上必然"候选集 0"。

---

## D. `dialog_start.cpp`（71 行）—— 对话框起点的**三级回落**

① `inherited`（上次的选择）→ ② **唯一**输入目录（0 个或 ≥2 个都不认）→ ③ **exe 所在目录**
（注释强调"**不是 CWD**"）。
`canonical_dialog_dir` 统一成 `/` 并 `cleanPath`（"统一之后自检能逐字比"）。

---

## E. `screen_scale.cpp`（89 行）

- `snap_ui_scale`：五档 `{1.0, 1.25, 1.5, 1.75, 2.0}` 就近吸附；`0/负/NaN ⇒ 1.0`；
- `adaptive_window_size`：默认 70% 可用区、上限 90% 且不超过 1600×1000 逻辑像素；
  **最小尺寸本身超上限时以最小尺寸为准**（"宁可溢出，也不能把窗口压到内容放不下"）；
- ★ `ui_scale_for(dpr, pdpi)`：
  ```cpp
  if (device_pixel_ratio > 1.01) return 1.0;   // Qt 已在按 OS 缩放渲染 ⇒ 我们**不叠加**
  return snap_ui_scale(physical_dpi / 96.0);   // dpr≈1（4K 屏 + 100% 缩放）⇒ 按物理 DPI 自己放大
  ```
  ⇒ 与 `main.cpp` 的 `logicalDotsPerInch 恒 96` 那条取证合起来看：**只能靠 dpr + 物理 DPI**。

---

## F. `fake_ffmpeg.cpp`（97 行）—— 离屏自检的假 ffmpeg

一个 exe 演三种角色（页面只通过"ffmpeg 路径"这一个入口拿到它）：
| 参数 | 行为 |
|---|---|
| `-encoders` | 打编码器表（**连图例行垃圾一起**，1.x 会收下） |
| `-print_format` | 打 ffprobe JSON（转换后重解析产物那一屏） |
| 其余 | 造输出文件 + stderr 打 `time=` 进度行；默认 ~480ms 退出 |
| `XRKIT_FAKE_FFMPEG_MODE=loop` | 一直打，**等着被取消杀掉**（判"取消要真停"） |
| `XRKIT_FAKE_FFMPEG_MODE=fail` | 打 `boom-invalid-input` 并以 3 退出 |

★ **不进发行包**（`package.ps1` 只拷 `xrkit_app.exe` 与依赖），只是构建产物。

⇒ ★ 这正好覆盖 P17-1② 那种"取消要真停"的判据需求 —— **`loop` 模式就是一个永不自己结束的 worker**。
  如果 P17-1② 的取消判据也用这种"等被杀"的夹具，就不会出现"点取消时 worker 已结束"的竞态。

---

## G. 结论

1. **分栏的三条收敛规则**都是实测踩出来的；判据该断 `clamp_count_` 与**几何**，不要断时间。
2. **`field_row` 只在 `drop_zone.cpp` 命名** ⇒ ⑭ 在插件页/占位页上必然候选集 0（**次生红确认**）。
3. **`ui_scale_for` 的 dpr 优先**解释了为什么"看逻辑 DPI 等于没做"。
4. **`fake_ffmpeg` 的 `loop` 模式**是"等被杀"的夹具范式 —— P17-1② 应当照它改。

## H. 待继续

shell 剩：`font_picker.cpp`(348)、`pages/*` 6 页（fs 1952 / video 1055 / xml 761 / convert 616 /
fontpack 421 / text 225）、`smoke.cpp`(10390)。
