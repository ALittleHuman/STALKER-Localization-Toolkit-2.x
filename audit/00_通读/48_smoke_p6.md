# `smoke.cpp` 批六（第 2851-3350 行）—— 两树同源 / 结构重排几何 / DB seek / P17-85

---

## 1. ★★ 两棵树"同一套小组件配置"的可判形式（第 2851-2922 行）

一条巨型 `check(...)`，把"两树同形"拆成**逐项相等**：
```
columnCount==3 / selectionMode 相等且==ExtendedSelection / uniformRowHeights 相等且为真 /
alternatingRowColors 相等 / header_stretch_columns()==1（两棵都要）/ minimumSectionSize 相等 /
expandsOnDoubleClick==false（两棵）/ rootIsDecorated（两棵）
```
★ 其中 `rootIsDecorated` 那条有故事：包列表在平表时代留了 `setRootIsDecorated(false)`
⇒ 树化 + 默认收起之后**顶层节点的孩子再也点不出来**（"把这条去掉立刻红"）。

★ 而 `header_fills_viewport()` 是**独立的硬判据**：
`sum(sectionSize) >= viewport()->width() - px(2)` —— "三列总宽 < 视口宽 ⇒ 右边是死区"。
★ 并且明确 **`acceptDrops()` 不算对齐项**（实例级特性，不是"同一套配置"的一部分）。

## 2. 过滤器"含家族通配、不枚举后缀"（第 2925-2941 行）
`*.db* / *.sq* / *.xdb*` 都在、且**不许**出现 `*.db0` / `*.db_base`。
⇒ "删掉 `.xdb*` 这条断言立刻红"（用户问过"`.xdb` 能识别吗"）。

## 3. ★★ 结构重排的几何判据（第 2943-3117 行）

### （a）空状态：**条目面板整块收起**（第 2948-2994 行）
判据：`entry_pane()->isHidden() && package_list()->height() > px(300)`。
修前：空状态把条目树 `setVisible(false)`，而 **Qt 布局不给隐藏控件分配空间**
⇒ 那 340 px 全落进 `entry_bar`（普通 QWidget、默认 Preferred）⇒ 里面的按钮被 `QHBoxLayout`
**垂直居中** ⇒ "搜索行孤零零飘在两块空白中间"。
★ 判红办法写明："把这两处改回去 ⇒ 实测会从 42 变成 386"。

### （b）★ 操作栏贴右：**先判前提，再判几何**（第 2972-2993 行）
修前：`if (action_bar() && unpack_all_button()) { check(贴右) }`
—— 条件不成立时**断言静默消失**（"操作栏丢了"这种回归反而一条失败都不记）。
现在：**把条件本身也写成判据**（`right_ok` 一条 check），几何检查才在块内做。

⇒ 这是"**前提不满足时断言静默消失**"的样本，与本仓"零静默"同族。

### （c）筛选三连（第 2996-3036 行）
① 树里**没有空名字行**（占位子项被摘掉）；② 搜 `aim` **不许出现 `other.bin`**（按名字匹配）；
③ 命中项**加粗**；④ 条目树 `ExtendedSelection`。

### （d）几何必须在**有内容**的夹具上量（第 3038-3106 行）
"空夹具的树是隐藏的（历史现象），不适合量几何" ⇒ 另造 `geo_page`。
两档：`900×700`（工具行在树上、两面板够高）与 `720×page_min_h()`（两面板都没被挤成一条 >40px）。

## 4. ★★ ⑦' DB seek 三判据（第 3119-3312 行）

**夹具构造很讲究**：先 `pack_db` 造一个真 xdb，然后**只把 DATA 块撑到 32 MB**、
尾部 HEADER 原样搬过去 ⇒ 得到一个"真档案尺寸"的包（真档案 DATA 就是几百 MB～几 GB）。

| 判据 | 内容 |
|---|---|
| ① | `used_seek && header_bytes < file_size/100`（32 MB 的包只读几十 KB）|
| ② | seek 版条目表与内存版 `unpack_db` **逐字段一致**（拼成 `is_dir|path|offset|size_real|size_comp|use_lzhuf` 串比）|
| ③ | 单条目 `extract_entry_seek` 与 `extract_entry` **逐字节一致** |

判红办法：把 `list_db_seek` 的 seek 分支短路掉 ⇒ ①立刻红。

## 5. ★★ P17-85：出参那条链真的接通了（第 3182-3273 行）

- **正常包**（xdb 能 seek）⇒ `used_seek==true` 且 **理由为空**（"没退回就不许报理由"）；
- **显式格式包**（11xx + 显式）⇒ **P17-127 之后**变成 `used_seek==true` +
  **非空理由且含"显式选定"**（"显式选定格式先在 seek 路径上试"）；
- ★ **逆向保护**：**不给格式**时 seek 认不出 ⇒ 必须**退回整包读并写理由**（含 "seek"）
  —— "01 F-09 要锁的就是这条：不许'认不出就静默当空包'"。

★★ 一处极实用的注释（第 3198 行）：
> ★ 名字别叫 `QString reason`：那个会**遮蔽**（并改变类型）传给 `pack_db` 的出参

## 6. ★ 包名规则：三家族 + 四条反例（第 3340-3350 行）
正例：`x.db0 / x.db_base / x.sqfs / x.sq_textures / x.xdb / x.xdb_base / patch_00.xdb`；
反例：`x.txt / xdbc / x.db.bak2 / x.xdoc`。
★ 原因（注释）：旧规则只取"点后两位"＝`xd` ⇒ 漏掉 `.xdb` 家族。

---

## 7. 待继续（3351 起）

1.x 便利批、五页判据、P4/P5/P13/P16/P17、⑬ 版本资源。
