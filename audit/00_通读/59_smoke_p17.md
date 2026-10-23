# `smoke.cpp` 批十七（第 7851-8370 行）—— P13-2b 双击偏移 / P6 树 / P7 门面 / 空状态"按性质判"

---

## 1. ★★ P13-2b：带**滚动偏移**的双击（第 7856-7926 行）

```cpp
act_view->set_scroll_top(row_h * 5);                    // 先滚到第 5 行
const QPointF at(6.0, row_h * 2 + 1);                   // 再点视口第 2 行
QApplication::sendEvent(act_view, &double_click);
check(fired == {"7"}, "滚到第 5 行后点视口第 2 行应=7");
```
⇒ 这条判据的**语义**是"**行号 = 偏移行 + 视口行**"，不是"双击有反应"。

★★ 而**回车那条明确不判**（第 7912-7913 行）：
> ★ 回车那条**不判**（见文件头/批报告：离屏自检里键盘事件送不到目标控件；
>   `QWidget::event` 是 protected，QtTest 没链）

⇒ "**有的路径在离屏环境里判不了，就如实不判**"，而不是写一条恒真的假判据。

★ 并且**先把这一页切成当前页**（第 7896-7903 行）："键盘事件只发给**焦点控件**，
而不可见控件拿不到焦点 ⇒ 先把这一页切到当前（自检里的一次'真导航'）"。

## 2. P6 树六条（第 7951-8010 行）
① 结构 `rows:2 / keys:A,B`；
② **懒建走用户路径**（`setExpanded(true)` ⇒ `on_expand` ⇒ 插件补子）⇒ `rows:5 / keys:A,a1,a2,a3,B`；
③ 三态：勾父 ⇒ `checked:A,a1,a2,a3`；取消一个子 ⇒ `partial:A / checked:a2,a3`；
④ 选中 `selected:B`（**放在过滤之前** —— "藏着的行不该被选中"）；
⑤ 过滤 `hits:1 / keys:A,a2`；
⑥ 激活：`invokeMethod(itemDoubleClicked)` ⇒ `activate:a3`。

## 3. P7a 引擎门面（第 8016-8057 行）
插件在 `build()` 里调 `api.engine.font_size_slots()` / `font_state_marks()`
⇒ 判据 `slots:10` 与 `ok_has_text:1` —— "值必须来自引擎（10 档槽位 + 引擎给的中文文案），
**不是插件自己编的**"。

## 4. P7c `replaces` 顶掉内置页（第 8059-8092 行）
四条：插件页进页表 / **内置 `fontpack` 不再可见**（`page_index("fontpack") < 0`）/
**可见页数不变（==6）** / **标签仍是原名**（`windowTitle()=="汉化包生成"`，而 id 换新 `fontpack_py`）。

## 5. 视频端到端：**宁可 note 跳过，也不造假绿**（第 8113-8176 行）
```cpp
if (!paths.found || !paths.has_probe) {
    result.notes.append("视频端到端：本机未按口径配置 ffmpeg/ffprobe（只认 user.ltx 手动指定与程序目录）"
                        "⇒ 跳过，属人工验收项");
} else { 造 1 秒 lavfi 输入 → start_transcode → 等结束 → 产物存在且 >0 → ffprobe 能解析出 duration= }
```
⇒ 与本项目"360 探针不过就记 NOT_COVERED、不产假红"是同一条纪律。

## 6. ★★ 5.x 页面骨架 + B1-6（第 8200-8235 行）
- ① 标题比正文大 ≥ 2pt；② 有副标题控件；③ 内容卡片 ≥ 页面高 **55%**；
- ④ **先判前提再判几何**：
  ```cpp
  const bool bar_ok = action_bar() && action_bar()->parentWidget() == &skel;
  check(bar_ok, "骨架④ 前提：操作栏存在且挂在页上");
  if (bar_ok) check(bottom_gap >= 0 && bottom_gap <= px(10), "操作栏贴底");
  ```
  注释："同 ①-a1 —— **条件不成立时断言静默消失**。把'操作栏存在且挂在页上'也写成判据（缺件即红）"。

## 7. ★★★ 空状态：**按性质判，不按数字**（第 8237-8308 行）

### （a）删掉的三样不许回来
```cpp
for (child : findChildren<QWidget*>())
    if (name == "fs_hint" || "fs_entry_hint" || "fs_empty_block") leftover << name;
check(leftover.isEmpty(), "删掉的空状态块/重复提示行又回来了：…");
check(!empty_page.entry_hint_visible(), "空状态提示访问器应为「恒 false」（该文字块已删）");
```

### （b）★★ "说明段落"按**性质**判（第 8260-8271 行）
```cpp
int paragraph_labels = 0;
for (QLabel* label : findChildren<QLabel*>())
    if (label->wordWrap() && label->text().size() > 16) ++paragraph_labels;
check(paragraph_labels == 0, "卡片里不该有自动换行的说明段落（实测 %1 个）");
```
★ 注释给了**口径沿革**（很值得抄的写法）：
> 原来这条写的是"卡片内直接 QLabel 恰好 1 个"：那是删完提示行那一刻的**快照**，
> 2026-09-28 按 1.x 补回按钮行与计数标签后它必然红 ⇒ **改成按性质判，不是把数字改大**。

### （c）★★ 断言作废时**报账**（第 8276-8293 行）
> 原 ⑥「空状态块 ≥ 卡片高 60%」的度量对象（那个占位块）已按用户要求删除 ⇒ 断言作废，
> 换成等价物：**没条目时条目列表收起、各列表不被挤成细线**。
> **口径沿革（报账，别当成"悄悄改数字"）**：
> ① 上一轮：60%（我按"卡片里只有两个列表"估的）；
> ② 用户要求补回 1.x 的按钮行后 → 改成"过半"；
> ③ 本轮再补 1.x 的封包面板 ⇒ 任何"某个列表占卡片几分之几"都不再说明问题。
> **保留的不变量**（也是用户真正抱怨过的："空列表只有一条细线，看着像坏了"）：
> **每个列表不得小于它自己的最小高度**。

```cpp
check(list_h >= px(120) && pack_h >= px(90), "空状态：各列表都不许被挤成细线");
check(entry_pane()->isHidden(), "空状态：条目面板整块收起，包列表填满工作区（与封包页同形）");
```

### （d）加载后也不许冒出那句提示（第 8301-8306 行）
`add_loose_dir(...)` ⇒ `!entry_hint_visible()` **且** `!entry_tree()->isHidden()`。

## 8. ★ 解密器消费端三条 + 钩子不许悬空（第 8323-8365 行）
① 没注册表 ⇒ 行为不变；② 能解开 ⇒ 源换成 `.decrypted` 产物；
③ 解不开 ⇒ **拒绝加载**且 `last_gate_error()` 非空；
★★ ④ **`set_registry` 同时把注册表交给引擎（活动钩子）**，而用例里的注册表是**局部对象**、
出块就死 ⇒ 必须 `plug::set_active_registry(nullptr)` 收回，**且这条本身是判据**：
"钩子不许悬空"。

## 9. 待继续
`smoke.cpp` 8371-9370。
