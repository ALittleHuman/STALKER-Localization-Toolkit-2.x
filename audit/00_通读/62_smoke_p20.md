# `smoke.cpp` 批二十（第 9451-9990 行）—— ★★★ ⑭ 判据本体：为什么"扫到 0 个可见字段行"

---

## 1. ★★ B1-8：夹具在仓库外 ⇒ 锁**静默消失**（第 9482-9504 行）

修前：三条锁（简体/日语/拒绝生成）都包在 `if (exists(…))` 里，而 `msyh` 指向
**仓库之外的开发机私有路径**（`…\otherTools\CN_Pack_Generator\msyh.ttf`）
⇒ 换台机器**三条锁静默消失**，连 notes 都查不到。

现在：
```cpp
if (!have_msyh) notes.append("⑪11.8 简体字体夹具缺件（…）⇒ 该条锁**未覆盖**");
…
check(have_msyh || have_jpan || have_latin,
      "⑪11.8/11.9 字体夹具**一个都没找到** ⇒ 这一组锁全部没跑（见 notes）");
```
⇒ "**判据一条都没跑 ≠ 通过**" —— 与 360 探针的 NOT_COVERED 同一纪律。

## 2. ★★ P17-1/F-01：worker 不许碰控件的**四可判形态**（第 9520-9589 行）

修前：worker 直接调 `build()` ⇒ 读下拉/写标签/裸 `++refused_count_` ⇒ 跨线程碰 QWidget +
数据竞争；页析构时 `result_label_`（子控件，**先于** `task_` 被删）撞上 worker 的写 ⇒ 访问违例。

判据：worker 里调 `build()` 与 `options_for()` ⇒
① UI 线程守卫**逐条记账**（`violations == before+2`，最近一条是 `options_for`）；
② 控件文本 / 拒绝计数 / 落盘目录**一个字都不许变**；
③ ★ **读控件那条路在 worker 里读不到值**（`worker_game != "CoP"`），而**主线程读得到同一个值**
   —— "一正一反"对拍；
④ 拒绝理由可见（含「线程纪律」）。

★ ③ 这条是本仓"**对拍**"手法的最佳实例：不是"worker 里报错就行"，而是**同一函数在两个线程上给出不同结论**。

## 3. ⑫12.2：16 个槽位扫空洞（第 9592-9638 行）
```cpp
const void* const* slot_table = reinterpret_cast<const void* const*>(ui);
for (i < slot_count) if (slot_table[i] == nullptr) ++holes;
check(holes == 0 && slot_count == 16, "api.ui 应有 16 个非空槽位");
```
⇒ `stk_ui` 是"纯函数指针的 POD"⇒ 按指针数组扫一遍就知道有没有空洞。
再做九种控件真造 + `set_text`/`set_state`/`alive`/`pack` 四条行为断言。

## 4. ⑫12.4：坏插件不阻塞启动（第 9641-9684 行）
空目录 ⇒ 无错 + 提示含"无"；坏 `.py`（`raise RuntimeError`）⇒ `registration_errors` 非空 +
提示含"问题" + 日志栏含"插件问题" + **`plugin_ui_installed()`** + **内置六页仍在**。
★ 注释还记了一条**假线索**：
> 包里那条 `not_a_plugin.dll: LoadLibrary 失败（错误码 193）` 是**别的夹具**留下的日志，不是这条判据。

## 5. ⑬ 版本资源：**读本进程 exe 的真实资源字节**（第 9686-9719 行）
三字段非空 + **`FileVersion == core::version_string()` 的版本号部分**（同一进程里现取现比）
+ `OriginalFilename == 本 exe 名`（"挂错目标的资源 ⇒ 当场红"）。

## 6. ★★★ ⑭ 判据本体（第 9721-9974 行）—— 为什么我们会看到"扫到 0 个可见字段行"

### （a）机制说明（与 `28_theme.md` 一致）
泛化规则 `QWidget { background: bg; }` 命中裸容器 ⇒ 容器自己涂了一层比卡片更深的灰，
又只露出**标签背后那一块** ⇒ 用户看到"缺了一块"的深灰圆角底板。

### （b）① 字段行（第 9804-9847 行）—— **这就是那条红**
```cpp
for (container : style_hub.findChildren<QWidget*>()) {
    if (container->objectName() != "field_row") continue;
    if (!container->isVisible()) continue;                       // ★ 只认"真的显示出来"的行
    if (container->findChild<QLineEdit*>() == nullptr || findChild<QLabel*>() == nullptr) continue;
    if (size.width() < 8 || size.height() < 8) continue;
    ++plate_rows_scanned;
    … render(container) ⇒ dominant(标签带) …
}
check(plate_rows_scanned > 0 && plate_hits == 0,
      "⑭ 标签背后不许有深灰底板（扫到 %1 个可见字段行，%2 处涂底；%3）");
```

★★ **`plate_rows_scanned > 0` 是 B1-5 新加的**（注释原文）：
> 候选样本数（B1-5：**没有自证样本 ⇒ 候选集空就恒绿**）

⇒ 所以我们看到的"扫到 0 个可见字段行，0 处涂底"这条红，**不是"涂底"**，
  而是 **候选集为空**。可能导致候选集为空的四个条件（按嫌疑排序）：
1. **`isVisible()` 为假** —— 只有**当前页**上的控件算可见；`style_hub.set_current_page(5)` 若没真切到
   汉化包生成页（或该页在 `QStackedWidget` 里不是 current），所有 `field_row` 全部不可见；
2. `objectName() != "field_row"` —— 只有当行容器**名字为空**时 `DropZones::attach_row` 才会命名；
   若某处先给了别的名字就永远不会是 `field_row`；
3. **没有同时含 `QLineEdit` 与 `QLabel`** 的子件（例如行容器被整体替换成别的布局）；
4. 尺寸 < 8×8（布局没跑）。

★ 判定建议（下次跑红时先取证这三样）：`style_hub.stack()->currentWidget()->objectName()`、
`findChildren<W>("field_row").size()`、以及它们的 `isVisible()`。

### （c）② 任务面板 / ③ 下拉 / ④ 勾选框（第 9849-9966 行）
- ② `task_panel` 渲染后底色必须**透明**（不许 == `bg`），同样带 `panel_scanned > 0`；
- ③ 可见下拉的"文字下方横带"必须**恰好 == `entry_bg`**，带 `combos_scanned > 0`；
- ④ 勾选框：用**哨兵底**（品红）渲染 `PE_IndicatorCheckBox`，数**亮像素**（`qGray>=150`）：
  ```cpp
  check(on_hits > 0 && on_hits > off_hits, "探针**真的画出了东西**（两者同为 0 ⇒ primitive 没被接管，下面那条判据就是空转）");
  check(on_hits >= 12, "选中态要真的画出勾");
  check(off_hits <= 2, "未选中态不许有勾");
  ```
  ★★ 这段注释是"**判据空转**"的教科书：
  > 修前**没有**"这一笔真的画出来了"的自证 —— 一旦 `PE_IndicatorCheckBox` 不再被本项目样式接管，
  > `on_hits` 与 `off_hits` 会**一起**变成 0，而 `off_hits <= 2` 照样绿 ⇒
  > "**没画勾**"被读成"**未选中态正确**"。

### （d）⑤ 拖放高亮规则还在（第 9968-9973 行）
`sheet.contains("*[drop_hover=\"true\"]")` —— "改样式别把它弄丢"。

## 7. 待继续
`smoke.cpp` 9991-11989（约 2000 行，含 D2 批 + 收尾）。
