# `src/shell/theme.cpp`（646 行，全文读完）—— ★ ⑭ 的根在这里

---

## 1. ⑭「标签背后不许有深灰底板」的**根因与修法**（第 292-316 行）

### 根因（注释里写得很清楚，是 `--shot` + 控件级 `render()` 采样实测出来的）
`QWidget { background: @{bg}; }` 这条**泛化规则命中所有"没写 QSS 的裸容器"**
（`path_row()` 的行容器、`TaskShell::panel()`、页视口…）
⇒ Qt 给它们置 `WA_StyledBackground` 后**每个容器都自己涂一层 `bg`**（比卡片 `surface2` 更深）；
行容器又正好被标签/输入框盖住，**只露出标签背后那一块** ⇒ 就是用户说的"缺了一块"的深灰圆角底板
（实测采样：容器渲染出 `#1e2124`；标签自己 `render()` 是透明的 —— **画底的是容器**）。

### 修法
```
QWidget#field_row { background: transparent; }     // 字段行容器
QWidget#task_panel { background: transparent; }     // 任务面板
QLabel            { background: transparent; }
```
★ 并且**保留** `QWidget { background: @{bg}; }`，理由：
> 它是 **`QPalette::Window` 的口径来源**（自检 ④ 钉着"暗/亮 QPalette::Window == 令牌 bg"）。
> 删掉它会连调色板一起变（实测 probe 的 Window 掉成 #000000，亮色下 app palette 退化）。

⇒ **判据 ⑭ 扫的是 `field_row`**（`--shot` 里也 dump `field_row`）。
  ⇒ **若页里一个 `field_row` 都没有，候选集为 0** ⇒ 判据报"扫到 0 个可见字段行，0 处涂底"
  ⇒ **这正是我们观测到的 ⑭ 红**。
  ⇒ 结论：**⑭ 是"空页/占位页"的次生红**，与用户的判断一致。

---

## 2. QSS 从"链式 `.arg()`"改成"命名令牌 + replace"（第 202-214 行）

### 真事故（本机 `--smoke` 实测 88 条警告）
模板编号**不连续**时 Qt 的 `.arg()` 会把"下一个值"喂给"下一个**还存在**的号" ⇒ 从缺号处整体错位：
`padding: #3a3f45px 4px`、`border-radius: #ffffffpx`、`color: 2`、`background: 10`
⇒ **尺寸槽拿到颜色、颜色槽拿到数字** ⇒ padding/圆角/勾选框尺寸/选中文字色**全部静默失效**
（观感因此"又挤又空"），链尾还多出 3 条 `Argument missing`。

### 现在的三道判据（都在 SHELL-SMOKE 里）
1. 生成样式表里**不许出现「颜色值 + px」**；
2. 不许有未替换的令牌、不许有残留 `%N`；
3. 应用样式表期间 Qt **一条 `parseHexColor` / `Argument missing` 警告都不许发**。

★ 缺字体角色时**故意留下** `@{missing-role:…}`（"必然判红"），而不是静默用默认值（第 245-248 行）。

---

## 3. 强调色只有三处（第 285-289 行）
> **强调色（蓝）只出现在三处** —— 主按钮（`#primary`）/ 当前标签项 / 进度条。
> hover 边框、勾选框选中态、滚动条 hover 一律用**中性色**。

★ 而且这条**被锁住了**：`drop_hover` 高亮**本来想用 accent**，被锁判红 ⇒ 改成 `selected`
（第 488-490 行原话："本轮真被拦住过一次 —— 锁是对的，改的是我"）。

---

## 4. `CheckMarkStyle`：两个**实测出来的机制**（第 52-113 行）

### （a）勾**不能**用 QSS 画
- `image: url(data:…)` —— Qt 的样式表引擎**根本不加载**（三条写法全试过，其它属性生效、只有 image 不生效）；
- 字体里的 ✓ —— 离屏机器上退化方块；
- ★★ **只要 QSS 给 `::indicator` 配了尺寸/边框/底色，Qt 就完全自己画指示器、
  根本不再走 `PE_IndicatorCheckBox`**（用"往指示器填品红"验证过：真机截图 0 个品红像素）。
⇒ 定案：指示器的 QSS 规则**整条撤掉**，方框与勾都自绘；颜色全从 palette 读。

### （b）下拉箭头**不能**用 QSS 画
- ★ **只要存在一条 `QComboBox::drop-down` 规则（哪怕只有 `border: 0`）**，
  Qt 的样式表样式就接管整个 combo 的绘制、**连箭头都不画**（真 combo `grab()` 后数
  `text_dim` 像素 = **0**）；
- 去掉那条规则 ⇒ 箭头出来，但只有 **6 个像素**（远不是 1.x 的硬朗三角）；
- 给 `::down-arrow` 配 `background` 也不行 —— Qt **涂完那一格就完事**、不再调 primitive。
⇒ 定案：`QSS 里没有 ::drop-down 规则`、`::down-arrow 只留几何`；
  槽底（surface2）+ 三角（text_dim）整块在 `PE_IndicatorArrowDown` 里画，
  实测 primitive 矩形 `[367,4 10x26]` ⇒ **尺寸按槽宽算**（用 `min(w,h)*0.5` 只有 5px 宽）。
判据：真 combo 槽里必须量到 **≥6 个 `text_dim` 像素**（实测 19；加回规则 ⇒ 0）。

---

## 5. ★ 样式对象身份的一个静默 bug（第 193-198、600-618 行）

```cpp
if (dynamic_cast<CheckMarkStyle*>(app.style()) == nullptr) { …重造… }
```
> 设过样式表之后 `QApplication::style()` 返回的是 **QStyleSheetStyle 包装**
> ⇒ 那个 cast **恒为 nullptr** ⇒ **每次 apply_theme 都重造一个样式对象**
> （只影响样式对象身份、不影响观感，所以一直没被注意到）。

修法：用进程级 `g_check_mark_style` 指针；每次 `apply_theme` 用它刷箭头颜色
（否则切主题后箭头还是旧色）。

⇒ **"判据恒为真 ⇒ 每次都重做"** 这类静默缺陷 —— 与 `host_facade_has` 恒 false 同族。

---

## 6. `apply_startup_ui` 的四步顺序（第 531-554 行，H-1）

```
① apply_theme(mode)              // 控件构造之前界面就得有主题
② apply_screen_ui_scale()        // 屏幕自适应给"没有偏好时"的基准
③ 有保存档时 set_ui_scale(saved)  // 用户上次选的压过屏幕自适应
④ apply_theme(mode)              // ★ 必须再套一次：字号是 (基准+2.0)*ui_scale()
```
修前这段**内联在 main.cpp**，于是两条判据（"字体与几何同源"、"界面大小不许翻主题"）
**在离屏自检里没法复现**（`--smoke` 走另一条分支）⇒ 搬到函数里让 main 与 smoke 调同一个。

★ 两次调用幂等：字号基准取自一次性 `static const kBasePt` ⇒ 不会连乘
（修前每次切主题 +2，切一次涨一次）。

---

## 7. H-2：`apply_ui_size` 的模式**从 core 取**（第 521-528 行）
修前读 `app.property("xrkit_theme")`，而那个属性**只在用户手动切主题时写**，启动路径从不写
⇒ 浅色用户第一次点「界面大小」读到空串 ⇒ 走 Dark ⇒ **整界面被拍回暗色**。
现在 `core::theme_mode()` 是唯一真值（`apply_theme` 内部会写它，启动路径一定写过）。

---

## 8. 其它
- `px(int)` 直接转发 `core::px`（第 48 行）；
- `apply_dark_titlebar`：DWM 属性 20/19（深色标题栏）+ 35（Win11 标题栏颜色）；
- `ui_size_choices()` 四档：小 0.9 / 默认 1.0 / 大 1.1 / 特大 1.2（用户 2026-09-30 收紧）。

## 9. 结论

1. **⑭ 是空页的次生红**（`field_row` 候选集 0）—— 与 P5 同源，修好空页即自愈。
2. **QSS 的"两条规则同名 ⇒ 后写的赢"**在本文件里被清理过四处（第 460-463 行），
   而 `QComboBox, QLineEdit, QSpinBox` 那一对**曾经**就是靠"后写覆盖"实现的内距 —— 现已逐字一致。
3. **`dynamic_cast<app.style()>` 恒空的静默 bug** 值得作为"判据恒真"的又一例记档。
