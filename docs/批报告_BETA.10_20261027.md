# 批报告：BETA.10（2026-10-27）—— 文本工具改名 + 夹具离包 + IX-Ray 选项落地

> 结论：`--fast` PASS、`--release` PASS（130 步）；包 `dist/2.0.0/BETA/10`（765 文件）；
> 包内自检 `SHELL-SMOKE ok checks=917 failed=0`；`commit=042d2a6`；
> `exe_sha256=456a18fc3f09eaeb4ff51ba4635a8b3a6ede75a909bc3e94e1a0c6141e40fcec`。
> BETA.1/2/258/3/4/5/6/7/8/9 未动。

## 一、本批三条（用户 2026-10-27）

| # | 要求 | 落地 |
|---|---|---|
| ① | 「XML校对」改名「文本工具」 | 页标题/任务名/插件替换表/smoke 期望/导出报告标题/golden 全同步（`6be16d9`） |
| ② | 包里的 `plugins/_fixtures` 删掉 | 开发/发行包都**不带夹具**；门禁④改为从**仓库** `plugins/` 抬；打包自检从仓库临时拷到包外跑一次（`90ec1ca`） |
| ③ | 汉化包生成加 **IX-Ray** 选项 | 下拉第四项 `IX-Ray` + 「参照包」行；引擎 `ixr_build` 起外部 fonttools 助手；工具链随包（`042d2a6` + 本报告） |

## 二、IX-Ray 这条链（产品）

```
汉化包生成页（游戏=IX-Ray）
  → engine::font::ixr_build
      找 python：XRKIT_IXR_PYTHON → 包内 python/python.exe → PATH
      找启动器：XRKIT_IXR_RUN → 包内 tools/ixr_run.py
      process::run（双管道、真停、如实退码）
  → tools/ixr_run.py（把 tools/fontlib/fontTools 插进 sys.path）
  → tools/ixrgen.py（fonttools 合并：glyf/cmap/hmtx；「指定」模式只换字体）
```

包内布局：`tools/ixr_run.py`、`tools/ixrgen.py`、`tools/fontlib/fontTools/`（698 文件，16.6 MB）。

## 三、本批抓到的真 bug

1. **包内 `python\python.exe` 起不来（0xC0000135）** —— `python3XX.dll`/`vcruntime140*.dll` 按布局留在包根，子进程的 DLL 搜索只从自己目录开始 ⇒ 缺 DLL。
   修法：打包时**再拷一份到 `python\`**（+~8 MB），包内自检 `IXR-RUN selftest ok` 由红转绿。
2. **`Start-Process` 拿不到输出** —— 自检改成 `& python.exe script --selftest` 直接取 `$LASTEXITCODE` 与文本，失败时能把输出写进 throw。
3. **packaging 语法两次被自己改坏** —— 删 `_fixtures` 注释时整行删掉了 `throw (...)` 的第一行；改用单行 throw 并加 `Parser::ParseFile` 预检（之后每次改 package.ps1 都先 parse）。

## 四、判据增量

| 步 | 状态 |
|---|---|
| `ixr-product`（新，130 步） | 缺参数退 3 / 真跑出合并字体（cmap 有「你」）/ 坏参照包退 1 且带出助手原话 / 启动器缺失点名 |
| `package-pages` | ④抬夹具改从仓库；新增"开发版包内也不许带夹具" |
| `package-plugin-list` | 静态锁改新口径（清单仍显式、不许 `_fixtures`/拷进 stage）；反例自证换成"又往包里带夹具""夹具拷进 stage" |
| `cli-exit-codes` | 冻结 USAGE 53 → 55（`--ixr-build` 两处用法错误） |

## 五、仍挂着

1. IX-Ray 页面**还没有"逐槽位选字体"的整条交互**（引擎侧 `--plan` 已支持；页面目前是"整包一个字体"）；
2. 分区（`--sizes`）在页面里还没暴露；
3. PKG-PAGES 间歇红（15~25%）仍未定性；
4. 包体积从 ~230 MB 涨到 ~250 MB（fontTools 16.6 MB + DLL 副本 8 MB）。
