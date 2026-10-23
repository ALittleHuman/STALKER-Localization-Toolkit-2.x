# `src/shell/font_picker.cpp`（414 行，全文读完）—— ★★★ 一条**真崩溃**的完整血账

---

## 1. 文件来由（第 1-6 行）
2026-10-08 P8a 从 `fontpack_page.cpp` 抽出，作为**唯一实现**（页面与 `api.ui.pick_font` 共用）。
三处改动：页面成员 → 文件内静态/对话框局部；落点改成注入回调；起点走共用 `dialog_start`。

## 2. ★★★ `detect_batch` 的自续期链：`--smoke` 里 0xC0000005 的**真因**（第 264-295 行）

修前：
```cpp
QTimer::singleShot(25, [keep, end] { (*keep)(end); });   // ★ 没有 context 对象
```
- 它既不属于 `dialog`、也不属于页面 ⇒ **谁析构都拦不住它**；
- 而 `detect_batch` 捕获的是裸 `this`（当时是页面）⇒ 下一跳里 `cached_mark(items[i])`
  直接摸页面的 `font_mark_cache_`（`std::map<QString,QString>` 成员）。

崩溃栈（门禁 [23] 实崩）：
```
run_smoke ← make_font_picker 的 lambda ← cached_mark ← std::_Tree<...>::_Find_lower_bound
```
即：**页夹具析构后链还在 25 ms 一跳地跑** ⇒ 下一个 `processEvents()` 里解引用已释放的树节点。

修法：把 **`dialog` 交给 `singleShot` 当 context**
```cpp
QTimer::singleShot(25, dialog, [keep, end] { (*keep)(end); });
```
⇒ "对话框一析构，Qt 就把这一跳取消，链自然终止"。

★★ 这条是本仓**最有教育意义的一条**：
1. **定时器必须绑生命周期**（context 对象）；
2. 崩溃**不在**崩的地方，而在"上一轮的定时器还在跑"；
3. 它的触发条件是"页析构 + 还有事件循环"，**只有自检能稳定复现** —— 这就是为什么本仓坚持
   "每条修复都要有判据"。

## 3. ★ "代次"用 **items 快照 + 匹配校验**代替（第 366-373 行）
```cpp
for (int i = 0; i < list->count() && i < items.size(); ++i) {
    QListWidgetItem* item = list->item(i);
    if (item->data(Qt::UserRole).toString() != items[i]) continue;   // 对不上就跳过
    …
}
```
过滤词一变，旧检测链"作废"—— 但没有显式取消机制，而是**用"列表项与快照是否对得上"**判过期。
⇒ 这是"**乐观并发 + 版本校验**"的手法（不引入代次计数器也能防止过期结果写进新列表）。

## 4. 四态标记只有一个来源（第 110-115 行）
`font_mark_of` → `efont::cjk_state_mark(efont::classify_cjk_font(path).state)`
—— **列表标记与预览标题共用它** ⇒ "两处结论不可能矛盾"。
并且带 `std::map` 缓存（跨对话框复用，第二次打开几乎立刻）。

## 5. 预览的"两行样本 + 日文回退"（第 117-134 行）
`汉化测试你好ABC` / `Привет 世界 123`；
若状态是 `Japanese`，**按 `fallbacks` 把缺的简体字换成繁体字形**再画
（用户 2026-09-14 口径："日语的，标明是日语字体，然后那些缺简体的就上繁体"）——
否则那几格是空白，用户看到的还是"缺字"。

## 6. 预览是"图随内容增长 + 可滚"（第 208-217 行）
1.x 那次修正（用户实测）：`QScrollArea(setWidgetResizable(true))` + `QLabel`，
放大后整张图变大、**滚动条随之可用**。

## 7. 字体候选的三段（第 68-103 行）
① 系统字体目录（**从引擎拿**，与 `api.host.system_font_dirs()` 共用）；
② **当前已选字体所在目录**（"用户自带的字体就在那儿"）；
③ 存在目录排前、完整路径去重、后缀白名单（`.ttf/.ttc/.otf` 小写比较）、子串过滤。

## 8. `default_font_path` 的三级回落（第 136-152 行）
① 上次用过的（user.ltx `[font] path`，**文件仍存在**才算）；
② 系统 `msyh.ttc/msyh.ttf/msyhl.ttc`（按名字优先级逐个目录找）；
③ exe 目录的 `msyh.ttf`；都没有 ⇒ 空串。

## 9. 双击＝选择（第 376-378 行）
`itemDoubleClicked → pick->click()`（1.x `:641`）。

## 10. 起点不写死 `C:\Windows\Fonts`（第 392-400 行）
`start_judge`（页面传自己那份）或 `dialog_start_for_input_file(current, {})`。

---

## 11. 待继续

`smoke.cpp`（10390 行，判据本体）—— 下一轮起分批读。
