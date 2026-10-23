# shell 批三：`page_host.cpp`(198) ＋ `virtual_list.cpp`(≈200)

---

## A. `page_host.cpp`（198 行）—— 页注册表（内核）

### 1. 排序键：**带 → order → 注册序**（第 9-20 行）
```cpp
struct Slot { int band; int order; size_t seq; };
```
`band` 是"带"（内置页 / 插件页分带），`order` 是声明顺序，`seq` 是注册顺序
⇒ 三键排序，**同 order 保注册顺序**（与 registry.cpp 的双键口径一致）。

### 2. ★★ P17-88：**注册入口对"跨用"当场拒绝**（第 35-50 行）
```cpp
if (kind == Builtin && owner != kBuiltinPageOwner)  ⇒ 拒绝（不许冒充内置页）
if (kind == Plugin  && owner == kBuiltinPageOwner)  ⇒ 拒绝（不许把插件页挂到内置身份上）
```
用户口径：「`owner`/`kind` 是**权限边界**……注册入口对跨用**当场拒绝**」。
并且 `reg.kind` **随注册一起记下**（第 86 行）⇒ 之后 `page_kind(id)` 读这个字段，
**不再靠哨兵字符串推断**（`hub.cpp:416-422` 注释点名"修前是三处字符串比较 —— 靠哨兵分叉的暗号"）。

⇒ 这是"**身份必须是数据，不是推断**"的样板。

### 3. 六条注册校验（第 51-82 行）
id 非空 / title 非空 / builder 非空 / id 不重复 / **不能替换自己** /
**同一目标只允许一个替换者**（"谁后注册谁赢"＝静默顶掉的形状，必须当场拒）。

### 4. `all_sorted` / `visible_sorted` / `replaced`
- `slot_of(reg)`：声明了 `replaces` 且目标已注册 ⇒ **继承目标的槽位**（替换＝占位，不是另起一页）；
  目标未注册 ⇒ 用自己声明的槽位；
  ★ 注释明确"刻意**只查一层**"（链式替换没有唯一合理答案，写死一层比猜可控）。
- `visible_sorted` = `all_sorted` 去掉"被替换的"；`replaced` = 只要被替换的（可一键还原）。

### 5. `take_diagnostics()`
`diags_` 用 `swap` 取走 ��� 每个消费者只看自己那一轮（`hub.cpp` 的 `drain_page_diagnostics`）。

---

## B. `virtual_list.cpp`（≈200 行）

### 1. 内容是**虚拟**的：`visible_range(top, view_h, row_h, total)`
```
first = top / row_h                                   // 10.2
last  = min(total, (top + view_h) / row_h + 2)        // ← +2 是"多画两行"的余量
```
`range_` 每次 `refresh()` 重算；`row_text(row)` 走 `provider_`，没有 provider 就 `QString::number(row)`。

### 2. ★ 内容高**通过 Qt 动态属性说出来**（第 31-37 行）
```cpp
void publish_content_height() { setProperty("xrkit_content_height", content_height()); }
```
注释写明理由：外层 `PageBase::sync_inner_scroll()` 读这个属性；
**不让基类 `qobject_cast<VirtualList*>`**（"基类只该认识'内容件'这个角色"），
也**不用 signals/slots**（"改行数的地方很少，显式调一次更清楚"）。

⇒ 这是"**用属性解耦**"的样板：跨层传值但互不依赖具体类型。
  （与 `drop_filter` 用 `drop_hover` 属性做视觉反馈是同一条思路。）

### 3. 选中模型（第 63-95 行，P16-3a）
- `add_sorted()`：按升序**就地插入**（集合很小，不引 `<algorithm>`）；
- `set_selected_row()`：**无修饰键＝重选一行**（多选模式下也一样，QListWidget 语义）；
- `set_multi_selection(false)`：退回纯单选时**只留锚点**（"别把选中一堆偷偷留着"）；
- `selected_rows()` 返回**升序**集合。

### 4. 键盘激活（P13-2b）
`setFocusPolicy(Qt::StrongFocus)` —— 收键盘才能回车激活。

---

## C. 这两份文件给 P13/P16 的结论

1. **列表/树的"可见行"是虚拟的**（`visible_range` 带 +2 余量）⇒ 判"40 行"要读 `set_row_count`
   或 `content_height()/row_height`，**不要数可见子控件**。
2. **选中集合的升序/锚点语义**决定"滚到第 5 行后点视口第 2 行应=7"那类断言怎么算
   （P13-2b 的 `[]` 失败很可能是 `range_` 没刷新或 provider 为空）。
3. **页身份是数据（`reg.kind`），不是字符串推断** —— 判据读 `page_kind(i)` 而不是比 owner 字符串。

## D. 待继续

shell 剩：`split_pane.cpp`(266)、`font_picker.cpp`(348)、`theme.cpp`(438)、`main.cpp`(410)、
`row_action_delegate.cpp`(58)、`drop_zone.cpp`(39)、`dialog_start.cpp`(55)、`screen_scale.cpp`(69)、
`fake_ffmpeg.cpp`(79)、`pages/*` 6 页、`smoke.cpp`(10390)。
