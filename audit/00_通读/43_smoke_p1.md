# `smoke.cpp` 批一（第 1-850 行 / 共 11989 行）

> `run_smoke()` 是**一个巨型函数**：从第 365 行开始一直到文件末尾，中间用 ①②③… 编号分块，
> 每块都写着"修前事实 / 判据 / **判红办法**"。这本身就是一份可执行的验收规格。

---

## 1. 结构（第 365-408 行）
```cpp
SmokeResult result;
① 删 trace 文件（若 XRKIT_PAGE_TRACE=1）
② 注册 menu_ready 钩子（键 = page|menu_id，裸键仅诊断）
③ 打一行 UI-FACADE on_ui_thread / app_thread / cur_thread
④ const auto check = [&](bool good, const QString& what) { ++checks; if(!good){++failed; failures<<what;} };
```
★ `check` 只数"跑了多少条 / 红了几条"；**notes 是另一条通道**（诊断，不影响判定）。

## 2. ★★ 三个共享 helper（第 279-363 行）

### （a）`wait_until(pred, ms)`（第 293-302 行）
`QElapsedTimer` + `processEvents(AllEvents,10)` + `msleep(5)`；返回 bool。
契约写明："调用方**超时照旧 `check(false)`**（不重试、不加固定 sleep），并把当时读到的值写进 notes"。

### （b）`wait_page_ready(page, ms, why)`（第 304-342 行）—— 四条就绪条件
1. `page != nullptr`；
2. `layout() != nullptr && layout()->count() > 0`；
3. **连续两次泵事件之间子树控件数不变**（不再长东西）；
4. 页或主视口 `width() > 0` —— ★ **offscreen 下不许用 `isVisible()`**（父窗没 show 时它恒假）。

超时把 `objectName / childCount / layoutCount / pageSize` 写进 `why`，**由调用方 check 判红**。

### （c）`write_fixture_safely(path, source)`（第 349-363 行）
`QSaveFile` + `commit()` + **读回比 size 与 SHA-256**。
理由（第 346-348 行）：裸 `QFile::open(WriteOnly|Truncate)` 的 `write()` 返回值**不证明**
Hub 扫目录时文件完整（缓存/杀软/中断）。

★ 这三件合起来是本仓"**判据不许用固定 sleep、不许假设写完就是完整**"的落地。

## 3. ★★ `user_ltx_restored()`：两个假绿漏洞（第 209-239 行）
修前判据是 `QFile(ltx).size() == ltx_before.size()`：
1. **读备份失败时** `ltx_before` 是空 ⇒ 判据退化成 `0 == 0` ⇒ **数据被破坏却判"还原成功"**；
2. 只比长度不比内容 ⇒ 同长度写坏照样绿。
现在：存在状态 + **逐字节内容相等**，并把"备份到底读到没有"作为**前置**
（"没读到就明说无从校验"）。

## 4. ★★ `qss_rule_body(sheet, selector, last)`（第 250-277 行）
- 修前 QSS 锁用 `indexOf`（取**第一条**），而 **Qt 里后写的规则赢** ⇒ 改回缺陷写法/整条删掉，锁仍然绿；
- 表头锁用 `qss.mid(at)` 取**到文末整段** ⇒ 同样抓不住；
- 选择器匹配必须**以规则头为准**（选择器后只能空白然后 `{`）：
  `QComboBox QAbstractItemView` 是 `…::item` 的**真前缀**，纯子串查找会取到错的规则（实测下拉③误判红）。

## 5. 各块的"判红办法"写法（这是本仓的纪律）
每块注释都写明"把 X 删掉 ⇒ 立刻红"，例如：
- P17-11：把反斜杠翻倍那两处删掉 ⇒ 立刻红；
- P17-14：把 `write_file_atomic` 里那道拦删掉 ⇒ ②立刻红；
- P17-17：超上限入参用 `nullptr` + 超大 size 调 ⇒ **真去碰了就崩**。

## 6. ★★ P17-11：命令行引号（第 420-482 行）
12 条用例逐字节对齐 Python `subprocess.list2cmdline`；期望值**不是手写的**，由 Python 生成 +
4000 条随机串对拍（0 条不一致）。端到端那一半用 `cmd /c echo` 证明"拼出来能被系统接受"。

## 7. ★★ P17-14：保留设备名（第 484-542 行）
两半：① 判定表 12 条（`console.txt`/`COM0`/`CON_X` 合法；`CON`/`con.txt`/`Com1.log` 非法）；
② 真调 `write_file_atomic("CON.txt")` ⇒ 必须 false 且理由含"保留设备名"，**且不许留产物**；
外加**对照**：普通名字仍要能写。

## 8. ★★ P17-15：JSON 深度（第 544-593 行）
8000 层嵌套 ⇒ 必须 `false` + 理由（不是崩）；**对照**：60 层不许被判成"超深"
（"它可能因为形状不对而以别的理由失败，那是另一条判据的事"——**只要求不是因为深度**）。

## 9. ★★ P17-17：lzhuf 上限（第 595-621 行）
`lzhuf_encode(nullptr, MAX+1)` ⇒ 当场空；对照：正常输入往返。

## 10. ★★ "被删掉的文字不许回来"锁（第 654-717 行）
六页每个 `QLabel` / 占位符 / 下拉项 / 副标题都扫一遍禁用串。
★★ 注释里有一条**我被自己的锁拦下**的教训：
> ⚠ 这里**不能**用 `行数/ID 统计`：那是 XML 页比较模式下拉里的**合法模式名**，
> 我第一版写成它，锁当场把我自己拦下来了 ⇒ 这条经验：**锁的判据要挑"只有被删对象才有"的字串**。

## 11. ★★ P16-2a/2b：每个页槽位**两种合法实现**（第 719-781 行）
开发态＝内置页；随包＝插件版（`replaces` 顶替内置页）。
逐槽位**显式**列出 `{builtin_id, builtin_title, plugin_id, plugin_title}`，
"第三种一律红 —— 不许用通配/前缀/'有个 id 就算'的放宽写法"。
★ 并且如实登记 `xml` 两版标题不同（`XML校对` vs `XML 校对`），"不许谁去顺手统一"。

## 12. ★★ P1：页注册表（第 784-850 行）
修前事实：内置六页硬编码 + 插件页另走 `plugin_pages_` ⇒ **同一件事两份实现**；
Hub 为注入注册表要 `dynamic_cast<FsPage*>` ⇒ "Hub 不含业务"不成立。
判据：注册→出现、注销→消失、重复→被拒且留痕、`replaces`→只隐藏且能一键还原、
**追加插件页不许重建已有页**（`stack()->widget(0) == first_page_before`）。

---

## 13. 待继续（851-11989）

Hub 机制各块、⑤⑥⑦ 页级判据、P4/P5/P13/P16/P17 各组、⑭ 主题、⑬ 版本资源。
