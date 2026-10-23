# `src/shell/main.cpp`（479 行，全文读完）—— 入口与全部诊断开关

---

## 1. 五条互斥的入口分支（顺序即优先级）

```
① --python-host / --py-plugins / --py-plugin-call / --ui-facade
       ⇒ 不建 QApplication，直接 run_python_host_probe(args, sink) 然后 return rc
② --scale-report
       ⇒ 建 QApplication，打一行 SCALE-APPLY 后退出（验"真实启动路径"的缩放）
③ --smoke
       ⇒ 离屏 + 跑 run_smoke()，逐行打 SHELL-SMOKE note/fail，收尾
          `SHELL-SMOKE ok checks=N failed=M mode=dark|light`，返回 failed==0?0:1
④ --shot <png> [--size WxH] [--page N] [--load P] [--filter T] [--log-open]
       [--font-picker] [--video-in/--video-ref/--video-out]
       ⇒ 真实窗口抓图 + 一大批 `SHOT/GEOM/HDR/ROW/PROW/LBL/PICKER` 诊断行
⑤ 无开关 ⇒ 正常 GUI（apply_startup_ui → Hub → resize → show → exec）
```

★ ① 在 **QApplication 之前**：理由写在注释里 ——"发行包里只有 `xrkit_app.exe`（没有 CLI），
而'包内自带的 Python 到底能不能起来'必须在**包内**判（`scripts/package.ps1` 的包内自检）"。

## 2. 离屏平台自动设置（第 122-125 行）
```cpp
if ((smoke || !shot_path.isEmpty()) && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
```
⇒ **只在用户没显式指定时**才设（可被外部覆盖，便于真屏调试）。

## 3. 高 DPI：`PassThrough`（第 130-131 行）
必须在 `QApplication` **构造之前**设。
理由：Qt6 默认 `Round` 会先把 125%/133% 取整一次，而我们的 `snap_ui_scale()` 还会再吸一档
⇒ 两处都在取整、结果不可预期。

## 4. ★ `--scale-report` 的三样取证（第 144-147 行）
> Windows 上 Qt6 的 **`logicalDotsPerInch` 恒为 96**（150% 缩放的机器上 logical 可用区
> 1707x1019、logicalDotsPerInch 仍报 96.0）⇒ 只看逻辑 DPI 的自适应**在真机上等于没做**。

⇒ 所以那行同时打 `dpi / pdpi / dpr / scale / screen / window / min` 七项。
**"看起来在做、实际没做"的典型**：又一条"必须用可观测值而不是意图"的实例。

## 5. `--shot` 分支的价值（第 75-78 行的原话）
> 我在离屏自检里量的几何"都对"，但他眼睛看到的是另一回事 ——
> **几何断言只能证明"我断言的那几条"，证明不了"看起来对不对"**。

⇒ 这是本项目"判据 + 截图双轨"的由来，也解释了为什么后面有一大堆 `GEOM/HDR/ROW/PROW/LBL`
  诊断行：**把'看起来'翻译成可读数字**。

### 逐类诊断行的用途
| 前缀 | 用途 |
|---|---|
| `SHOT load=… sources=… entries=… errors=…` | 与 CLI `--dump` 的 `entries=` 同口径对数字 |
| `SHOT convert source_enc=[…] target_default=…` | 离屏无中文字体 ⇒ 截图看不出下拉内容，只能打原文 |
| `GEOM <tag> <name> x y w h vis` | 抓哪页量哪页（前缀不写死 `fs_`） |
| `LBL <name> w h text="…"` | 截图判几何、这行判文案 |
| `HDR pkg cols=… widths=[…] total=… viewport=… fill=0/1` | "三列总宽 < 面板宽 ⇒ 右半边死区"用数字判 |
| `HDR tree_font_px=… hdr_fm_h=… hdr_height=…` | "表头裁字"专项 |
| `ROW/PROW d=… text=… chk=… kids=…` | 树的层级/勾选态（前 40 行） |
| `PICKER ok=… items=… preview=WxH` | 字体选择器是独立顶层窗口，`hub.grab()` 抓不到 |

★ 两点细节：
1. `--shot` 结束前跑 **3 轮** `layout()->activate()` + `processEvents()`（离屏下 `LayoutRequest`
   不会自己派发）；
2. `--font-picker` 抓完 `picker->close(); delete picker;`（独立窗口自己负责）。

## 6. 一个已知的注释重复（第 316-318 行）
`★ 2026-10-06（D1）：前缀不再写死 fs_ …` 连写了两遍（同一句）。无害，属整理项。

## 7. `--page` 越界处理（第 205 行）
`if (shot_page > 0 && shot_page < hub.page_count()) hub.set_current_page(shot_page);`
⇒ 第 0 页是默认，不必显式切。

## 8. 结论

1. **`--smoke` 与 `--shot` 是两条独立通道**：前者是**判据**（`SHELL-SMOKE` 收尾行），
   后者是**人眼/数字取证**（`SHOT/GEOM/...`）。两者不要混用。
2. ① 分支在 QApplication 之前 ⇒ 那四个开关**不能依赖任何 Qt 状态**（这也解释了它们为什么不建 app）。
3. `logicalDotsPerInch` 在 Windows 恒 96 ⇒ 任何"按 DPI 自适应"的判据都必须用
   `devicePixelRatio` 或物理 DPI，不能只看逻辑 DPI。

## 9. 待继续

shell 剩：`split_pane.cpp`(266)、`font_picker.cpp`(348)、`theme.cpp`(438)、
`row_action_delegate.cpp`(58)、`drop_zone.cpp`(39)、`dialog_start.cpp`(55)、`screen_scale.cpp`(69)、
`fake_ffmpeg.cpp`(79)、`pages/*` 6 页、`smoke.cpp`(10390)。
