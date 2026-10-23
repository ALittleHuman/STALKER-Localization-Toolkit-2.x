# `smoke.cpp` 批三（第 1351-1850 行）—— 主题/启动序列/滚动三闸门

---

## 1. ★ 样式表"警告数必须 0"（第 1352-1366 行）
`qInstallMessageHandler` 拦住 `parseHexColor` 与 `Argument missing` 两类警告，
**连套三次主题（暗→亮→暗）**，断言 0 条 —— 这是链式 `.arg()` 错位那次的回归锁。

## 2. ★★ 强调色"逐规则"白名单（第 1368-1391 行）
```cpp
const QStringList accent_allowed = {"QPushButton#primary", "…#primary:hover", "…#primary:pressed",
                                    "QTabBar#page_tabs::tab:selected", "QProgressBar::chunk"};
for (rule : sheet.split('}')) {
    if (!rule.contains(accent_hex)) continue;
    const QString selector = rule.section('{', 0, 0).trimmed();
    if (!accent_allowed.contains(selector)) accent_strays << selector;
}
```
⇒ **逐规则看**，不是"有没有"；多一处就红（"不许看着差不多"）。

## 3. ★★ ④-b/④-c：主题与界面大小的"记住上次选择"（第 1394-1606 行）

三条判据（**"先证红再改绿"**）：
1. 改主题 ⇒ `user.ltx` 的 `[theme] mode` 变新值 ⇒ **重新读回**（＝重启路径）拿到同一主题；
2. 界面大小档位同样；
3. **另预置一个别的键**（`[ffmpeg] path` 哨兵），断言那次改动**只动了该动的键**。

★ 三段还原判据都走 `user_ltx_restored()`（逐字节 + 备份缺失即红），并且**额外自证**
（第 1578-1606 行）：临时目录造三态 —— 一致（应真）/ **同长度写坏**（应假，旧判据在这里是绿的）/
**备份缺失**（应假，防 `0==0` 假绿）。

★★ ④-c 的关键：**判据钉在与 main 共用的 `apply_startup_ui` 上**，而不是 main 里那段内联代码
——"修前那条序列内联在 `main()` 里，而 `--smoke` 走另一条分支、压根不经过交互式启动
⇒ 这两条判据在门禁里**验不到**"。

★ 还有一条细节：**"字体与几何同源"那条不许写死字号**
（先在 `ui_scale=1.0` 量一次，之后必须**恰好**乘 1.2）——
"写成常量就等于把某个 Qt 默认字号钉进判据"。

★ H-2 的复现手法（第 1536-1550 行）：**如实重现启动态** ——
`qApp->setProperty("xrkit_theme", QVariant())` 把属性清空，再点「界面大小」，
断言 `core::theme_mode()` 仍是浅色、`palette().Window` 仍是浅色令牌。

## 4. ⑤ HiDPI 与 ⑥ 设置文件名（第 1608-1628 行）
- `px(24)@1.0==24`、`px(24)@1.5==36`；
- 设置文件名 = `xrkit_user.ltx`（**不与游戏同名**，H-02），且与 exe 同目录；
  迁移源仍是 exe 旁的 `user.ltx`（"不能两处各写一份路径"）。

## 5. ★★ ⑦ 滚动三闸门（第 1630-1850 行）—— 逐条"能红"

| 断言 | 修前事实 / 判红办法 |
|---|---|
| 装得下 ⇒ `can_scroll_vertical()==false`、`bar_shown()==false`、偏移恒 0 | `bar_shown` **四看**（isHidden / isVisibleTo / isVisible / 尺寸），"不 show 会假通过" |
| **控件级拦**：body 里的 `QPlainTextEdit` 条子不可见时**不许自己滚** | 1.x 原文"Tk 的 Text/Listbox 类绑定根本不看我们的条子"，Qt 同理 |
| ★ **条子可见且能滚时，必须它自己滚、事件被吃掉、外层不许动** | 2026-09-29 口径更新（旧口径"可见 ⇒ 不干预"正是用户报的现象）；判红办法：把 `route_wheel` 里那段去掉 ⇒ 恢复 `return false` |
| 装不下 ⇒ 能滚且条子四看全真 | — |
| ★★ **拖条子/点箭头也必须挪画布** | 2026-10-07 自检抓到：修前普通视口**没有 `valueChanged` 连接** ⇒ **只有滚轮能动画面**，拖滑块完全没反应 |
| 滚轮到底/到顶夹住 | 各 60 次 |
| ★★ **连续两个滚轮事件（不重置认领表）都必须真的动** | 2026-09-27 用户实测缺陷：旧实现拿 `QWheelEvent` **对象地址**当事件身份，而 Qt 会**复用槽位** ⇒ 第二个事件地址与上一个相同 ⇒ 被判成"已被认领"并撤销滚动 ⇒ 现象是"**滚下去之后滚不回来**" |

★★ 最后那条特别值得记：上面的 `send_wheel` **每次都 `reset_wheel_claims()`** ⇒
"**它天然测不出这个缺陷**" —— 所以另写了一个 `send_wheel_raw`（故意不重置）。
⇒ 这是"**判据的辅助函数本身会掩盖缺陷**"的典型，也是"同一个 helper 不能既方便又灵敏"的实证。

---

## 6. 待继续（1851 起）

⑦ 余下（全局路由/分栏）、⑧ 各页几何、⑭ 主题、P4/P5/P13/P16/P17 各组、⑬ 版本资源。
