#requires -Version 5.1
<#
  2.0.0 打包（M3 第一步）：把 msvc-shell 的产物做成一个**可回退**的发行包。

  口径（见 docs/2.0.0-打包与发版规格.md）：
   ① 包内**不带** ffmpeg、**不带** NLC 解密插件、**不带**实验性引擎补丁；`plugins\` 目录**存在但为空**
      （插件由用户自己放；发行包为空是既有决策）；
   ② Qt 运行时用官方 `windeployqt` 铺（不手抄 DLL 名单，避免漏平台插件）；
   ③ 包内放 `README.txt`（安装/回退）与 `version.txt`（版本 + 源提交 + 构建时间）；
   ④ 打完后**从包目录跑一次离屏自检**（`xrkit_app.exe --smoke`）——这是"包是否完整可运行"的判据，
      不是人眼看文件齐不齐。

  用法：pwsh -File scripts/package.ps1 [-OutDir dist]
#>
param(
    [string]$OutDir = 'dist',
    # 构建序号与完整版本串由 CLI（--build-package）传进来 —— 版本口径只有一处：version.json
    [int]$BuildIndex = 0,
    [string]$VersionString = '',
    # ★★ 2026-10-05（本批 #8）：**开发版默认带全部插件**。默认值按下文 `current.prerelease`
    #   推导（ALPHA/BETA ⇒ 开发版）；显式 `-Dev:$false` ⇒ 强制发行口径（plugins\ 为空）。
    #   `DevWasSet` 用来区分"用户显式关了"与"默认值"两种情况。
    [bool]$Dev = $false
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root 'build\package_stage'
$shell = Join-Path $root 'build\msvc-shell'
$qt    = 'E:\Software\Tools\Qt\6.8.3\msvc2022_64'

$exe = Get-ChildItem -Path $shell -Recurse -Filter 'xrkit_app.exe' -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $exe) { throw "找不到 msvc-shell 的 xrkit_app.exe（先跑 scripts\build.ps1 -Preset msvc-shell）" }
if (-not (Test-Path (Join-Path $qt 'bin\windeployqt.exe'))) { throw "找不到 windeployqt：$qt" }

Write-Host "== 清理舞台 =="
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'plugins') | Out-Null

Write-Host "== 复制主程序 =="
Copy-Item -LiteralPath $exe.FullName -Destination $stage

# ★★ 插件改装 P2（2026-10-07）：**CPython 运行期必须随包**。
#   引擎现在链接 `python3XX.lib`（插件＝`.py`，宿主在引擎层）⇒ 包里缺 `python3XX.dll`
#   的结果不是"插件不可用"，而是**主程序根本起不来**。开发态那份 DLL 由 CMake 的
#   `xrkit_attach_python_runtime()` 拷在 exe 旁边；这里把它搬进包。
#   ★ 判据化：**拷不到就 throw** —— 宁可打包失败，也不许打出一个双击起不来的包
#     （包内那趟 `--smoke` 是第二道闸，但它只能证明"跑起来了"，证明不了"为什么没有"）。
#   ⚠ 尚未随包的：Python **标准库**（`Lib/`）。当前解释器用的是构建期烘焙的
#     `XRKIT_PYTHON_HOME_DEFAULT`（本机 Python 安装）⇒ 发行到**没有 Python 的机器**上会
#     `Failed to import encodings module`。随包自带 embeddable Python（`python\`）是下一片，
#     见批报告「只报告未改」。
$pyTargets = @()
$pyNames = @()
foreach ($d in @((Join-Path $shell 'src\shell'), (Join-Path $shell 'tools\cli'))) {
    if (-not (Test-Path $d)) { continue }
    Get-ChildItem -LiteralPath $d -Filter 'python3*.dll' -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            if ($pyNames -notcontains $_.Name) { $pyNames += $_.Name; $pyTargets += $_.FullName }
        }
}
if ($pyTargets.Count -eq 0) {
    throw ("找不到 python3*.dll（引擎已嵌入 CPython）—— 打出来的包会起不来。" +
           " 先跑 scripts\build.ps1 -Preset msvc-shell，确认 exe 旁边有 python314.dll；" +
           " 搜过：" + ((Join-Path $shell 'src\shell') + ' ; ' + (Join-Path $shell 'tools\cli')))
}
foreach ($f in $pyTargets) { Copy-Item -LiteralPath $f -Destination $stage -Force }
Write-Host ("  CPython 运行期 {0} 个：{1}" -f $pyTargets.Count, ($pyNames -join ', '))

# ★★ 2026-10-05（本批 #8）：开发版 = 标记 ∈ {ALPHA, BETA}（版本口径仍是 version.json 一处）。
#   在复制插件**之前**先把这个开关定下来 —— 它决定 `plugins\` 是"带全部插件"还是"空目录"。
$Prerelease = ''
try {
    $vjDev = Join-Path $root 'version.json'
    if (Test-Path $vjDev) {
        $curDev = (Get-Content -LiteralPath $vjDev -Raw -Encoding UTF8 | ConvertFrom-Json).current
        if ($curDev -and $curDev.prerelease) { $Prerelease = [string]$curDev.prerelease }
    }
} catch {
    Write-Warning "读 version.json 的 prerelease 失败（按发行版口径处理）：$_"
}

# ★★ 2026-10-05（本批 #8，用户约定）：**开发版默认带上所有插件**（2.0.0 有测试插件）。
#   背景：`plugins\` 一直**存在但为空**（① 里那条"发行包为空"是**发行**口径，本身不变）——
#   但开发/自测时每次都要手动把插件拷进去，而"包内自检"因此**从来验不到插件链路**。
#   口径（本批定）：
#     · **开发版**（标记 ∈ {ALPHA, BETA}）⇒ 把已构建插件**复制进包** `plugins\`（含测试插件）；
#     · **发行版**（标记 ∈ {RC, FINAL}，或显式 `-Dev:$false`）⇒ **仍按既定策略**：
#       `plugins\` 存在但为空、**NLC 解密插件不随包发布**。
#   ★ 判据（可判红）：开发版包里 `plugins\*.dll` **非空** + 包内自检**打印插件数与名字**；
#     改回"空目录"（或把这一段删掉）⇒ 立刻红。
$isDev = ($Prerelease -match '^(?i)ALPHA|BETA')   # 标记口径见 docs/2.0.0-打包与发版规格.md
if ($PSBoundParameters.ContainsKey('Dev')) { $isDev = $Dev }   # 显式 -Dev 覆盖标记推导
$copiedPlugins = @()
if ($isDev) {
    Write-Host "== 开发版：把已构建插件复制进包（用户约定：开发版带全部插件）=="
    # 只认**我们自己构建出来的插件 DLL**（`xrkit_*_plugin.dll`）：
    #   插件目录的加载器会吃**任意** .dll（loader.cpp 只按后缀过滤），照搬整目录会把
    #   Qt 运行时那堆 DLL 也当成插件、并把它们从包根"搬走" ⇒ 包直接坏掉。
    #   命名前缀既是构建约定，也是"这是我的插件"的判据。
    $pluginDirs = @(
        (Join-Path $root 'build\msvc-release\tools\cli'),   # xrkit_test_plugin.dll 落点
        (Join-Path $root 'build\msvc-shell\tools\cli'),     # 外壳预设下的同一目标
        (Join-Path $root 'build\plugins')
    )
    foreach ($d in $pluginDirs) {
        if (-not (Test-Path $d)) { continue }
        Get-ChildItem -LiteralPath $d -Filter 'xrkit_*_plugin.dll' -File -ErrorAction SilentlyContinue |
            ForEach-Object {
                # ★★ 2026-10-05（本批）：**同名只算一次** —— 同一枚插件会在**多个**构建目录里
                #   各有一份（`msvc-release` 与 `msvc-shell` 是同一目标的两种预设），而且
                #   `Copy-Item -Force` 到同一个落点本来就是"后写覆盖前写"。
                #   修前不去重 ⇒ `插件 2 个：xrkit_test_plugin.dll, xrkit_test_plugin.dll`，
                #   而包里其实只有 **1** 个文件 —— 那句数会一路写进 README.txt（用户可见面），
                #   与"零静默/说明书必须与实际打的包一致"直接冲突。
                if ($copiedPlugins -contains $_.Name) { return }
                Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'plugins') -Force
                $copiedPlugins += $_.Name
            }
    }
    if ($copiedPlugins.Count -gt 0) {
        Write-Host ("  插件 {0} 个：{1}" -f $copiedPlugins.Count, ($copiedPlugins -join ', '))
    } else {
        Write-Host "  插件 0 个（没有已构建的 xrkit_*_plugin.dll）"
    }
} else {
    Write-Host "== 发行版：plugins\ 保持为空（既定策略：NLC 解密插件不随包发布）=="
}

# ★★ 用户口径（2026-10-05）：「ffmpeg 等依赖应当自带」。
#   视频页按 ⑧8.1 只认**程序目录**与 user.ltx 两处（不搜 PATH）⇒ 自带件必须落在包根，
#   页面才可能"不填路径也能用"。找不到就**报错**（不静默跳过：静默跳过的结果就是
#   用户拿到一个视频页不能用的包，而没人知道为什么）。
#   版本守卫照 ⑧8.6：ffmpeg **8.x 拒收**（8.x 的 theora 编码器对 STALKER OGM 有 bug）。
$depDirs = @()
if ($env:XRKIT_FFMPEG_DIR) { $depDirs += $env:XRKIT_FFMPEG_DIR }
$depDirs += (Join-Path $root 'third_party\ffmpeg')
$depDirs += (Join-Path $root 'deps\ffmpeg')
$depDirs += 'E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit'
$depSrc = $null
foreach ($dir in $depDirs) {
    if ((Test-Path (Join-Path $dir 'ffmpeg.exe')) -and (Test-Path (Join-Path $dir 'ffprobe.exe'))) {
        $depSrc = $dir
        break
    }
}
if (-not $depSrc) {
    throw ("ffmpeg.exe / ffprobe.exe not found. Looked in: " + ($depDirs -join '; ') +
           ". Put both in the same directory and set XRKIT_FFMPEG_DIR, or pass a directory there.")
}
Write-Host "  自带依赖来源: $depSrc"
Copy-Item -LiteralPath (Join-Path $depSrc 'ffmpeg.exe') -Destination $stage
Copy-Item -LiteralPath (Join-Path $depSrc 'ffprobe.exe') -Destination $stage
$ffVersion = (& (Join-Path $stage 'ffmpeg.exe') -version 2>&1 | Select-Object -First 1)
$ffExit = $LASTEXITCODE
Write-Host "  $ffVersion"
# ★★ 2026-10-07（审计补充 H1-5）：这条守卫原来**可以整段绕过** ——
#   `-match` 不命中（版本行形状变了 / ffmpeg 起不来）就什么都不做；命中了又只拦 `-eq 8`
#   ⇒ 9.x / 10.x 照样放行，而 `scripts\download_ffmpeg.py` 那边的口径是"**必须恰好 7.x**"
#   （8.x 的 libtheora 编码器对 STALKER OGM 有缺陷）。现在：起不来 / 版本行认不出 /
#   主版本 ≠ 7 ⇒ **明确报错**（零静默；"认不出就放行"与"只拦一个数"都是假判据）。
if ($ffExit -ne 0) {
    throw "自带的 ffmpeg 起不来：`ffmpeg -version` 退出码 $ffExit"
}
if ($ffVersion -notmatch 'ffmpeg version n?(\d+)\.') {
    throw "认不出自带 ffmpeg 的版本行（原始行：$ffVersion）—— 不许「认不出就放行」"
}
$ffMajor = [int]$Matches[1]
if ($ffMajor -ne 7) {
    throw ("自带的 ffmpeg 是 {0}.x —— 只接受 7.x（8.x 的 libtheora 编码器对 STALKER OGM 有缺陷，" -f $ffMajor) +
          "download_ffmpeg.py 的口径也是恰好 7.x）"
}
Write-Host "  版本守卫通过：自带 ffmpeg 是 7.x"
# 自动安装按钮的真身（程序目录里没有自带件时，页面用得上它）
Copy-Item -LiteralPath (Join-Path $root 'scripts\download_ffmpeg.py') -Destination $stage

Write-Host "== windeployqt（铺 Qt 运行时；不铺翻译/示例）=="
& (Join-Path $qt 'bin\windeployqt.exe') --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw (Join-Path $stage 'xrkit_app.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败（退出码 $LASTEXITCODE）" }

Write-Host "== README / version =="
# ★★ 2026-10-07（审计补充 H1-5）：`git rev-parse` 原来**不查 `$LASTEXITCODE`** ——
#   git 不可用 / 不在仓库里时，`version.txt` 的 `commit=` 会静默写成一个错的值
#   （证据：`version.json` 里 13/14/15/16/19~23 等条目的 commit 被记成了版本串
#   `2.0.0 ALPHA.0`）。commit 是"这份包从哪个源码提交出来的"唯一凭据 ⇒ 取不到就**报错**。
$commitRaw = & git -C $root rev-parse --short HEAD
$commitExit = $LASTEXITCODE
$commit = ("$commitRaw").Trim()
if ($commitExit -ne 0 -or $commit -eq '') {
    throw "取不到源提交（git -C `"$root`" rev-parse --short HEAD 退出码 $commitExit）—— version.txt 的 commit= 不许写错的/空的值"
}
$stamp  = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
# ★ 2026-10-05（#8）：README 里那句"plugins\ 为空"必须与**实际打出来的包**一致 ——
#   开发版带插件时还写"为空"就是谎报（零静默红线同源：说明书也是"可见面"的一部分）。
if ($isDev) {
    $pluginLine = if ($copiedPlugins.Count -gt 0) {
        "插件：本包是**开发版**，plugins\ 已带 {0} 个插件：{1}。" -f $copiedPlugins.Count, ($copiedPlugins -join ', ')
    } else {
        "插件：本包是**开发版**，但构建产物里没有 xrkit_*_plugin.dll，plugins\ 为空。"
    }
} else {
    $pluginLine = "插件：plugins\ 为空目录（发行版策略：NLC 解密插件闭源、不随包发布）。需要时把 <name>.dll 放进去，重启程序。"
}
@"
STALKER 汉化工具集 2.0.0（xrkit）
--------------------------------
安装：整个目录解压到任意位置，双击 xrkit_app.exe。
回退：删除该目录即可（本工具**不改动**游戏目录里的任何文件；
      唯一的写盘位置是用户设置 user.ltx，删除它即恢复默认）。
$pluginLine
      插件加载失败不会阻塞启动，问题会显示在启动后的日志栏里。
自带：ffmpeg.exe / ffprobe.exe（视频页不填路径也能直接用；ffprobe 沿用 1.x 自带那份，
      实测可用，不为它做版本守卫）+ download_ffmpeg.py（缺件时重下）。
不含：NLC 解密插件（发行版）、实验性引擎补丁。
自检：命令行跑 `xrkit_app.exe --smoke`（离屏，ASCII 输出）可核对包是否完整。
      本包 `platforms\` **同时**带 `qwindows.dll` 与 `qoffscreen.dll`：`--smoke`/`--layout-report`
      走离屏，而 `windeployqt` 按 app 的实际调用只铺 qwindows ⇒ 少了后者会直接
      崩 `0xC0000409`（BEX64 / 故障模块 Qt6Core.dll / P7=0x267a8，ALPHA.57~59 都踩过）。
      跑法：`set QT_QPA_PLATFORM=offscreen` 再执行；结论行是 `SHELL-SMOKE ok checks=N failed=0`
      （`failed=` 必须为 0，退出码必须为 0）。
"@ | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8
$verLine = if ($VersionString -ne '') { $VersionString } else { '2.0.0' }
# ── ★★ 2026-10-07（H1-2，用户候选 A）：**包内 exe 的版本身份**必须等于本次发版串 ──────
#   事实（修前）：exe 的资源与 `core::version_string()` 都编自 `version.h` 的常量
#   （`kPrerelease="ALPHA.0"` / `kBuildCount=0`，从未被写回），而包目录名与 version.txt 来自
#   version.json ⇒ 实测**同一个交付物里"包说 ALPHA.60、包内 exe 说 ALPHA.0"**，
#   而门禁那两条 `WINRES` 判据全绿（只在各自源内自比 —— 这正是审计 H1-2 说的"两个唯一
#   事实来源、无任何检查比对"）。
#   现在：从**产物**（`$stage\xrkit_app.exe`）读真实 VERSIONINFO，与本次发版串**逐字比**；
#   不一致就 throw（包根本不会落地），一致才把结论写进 version.txt 的 `exe_identity=`。
#   ★ 门禁据此对**声明过的序号**执行独立的"包内 exe ↔ version.txt"判据（自己再读一遍 exe，
#     不信任这行自报）；修前打的旧序号没有这一行 ⇒ 门禁记台账。
$shellCli = Join-Path $shell 'tools\cli\xrkit_cli.exe'
if (-not (Test-Path $shellCli)) {
    throw "找不到 $shellCli —— 读包内 exe 的版本资源要用它（本次构建没产出 CLI？）"
}
$exeVerLine = (& $shellCli --exe-version (Join-Path $stage 'xrkit_app.exe')) | Select-Object -First 1
if ($LASTEXITCODE -ne 0) {
    throw "读包内 xrkit_app.exe 的版本资源失败（退出码 $LASTEXITCODE）：$exeVerLine"
}
if ($exeVerLine -notmatch 'product=(.+?)\s+file=') {
    throw "认不出 `--exe-version` 的输出：$exeVerLine"
}
$exeIdentity = $Matches[1]
if ($exeIdentity -ne $verLine) {
    throw ("包内 exe 的 ProductVersion 与本次发版串不一致：exe=「{0}」/ 期望「{1}」" -f $exeIdentity, $verLine) +
          "（version.h 没被写回？`--build-package` 的写回段失败？.rc 没重生成？）"
}
Write-Host ("  版本身份判据通过：包内 exe ProductVersion=「{0}」" -f $exeIdentity)
@"
version=$verLine
index=$BuildIndex
commit=$commit
built=$stamp
platform_plugins=qwindows,qoffscreen
exe_identity=$exeIdentity
"@ | Set-Content -LiteralPath (Join-Path $stage 'version.txt') -Encoding UTF8
# ★★ 2026-10-07（用户裁决）：`platform_plugins=` 这一行是**包自己的契约声明** ——
#   门禁（`tests\check_package_platform.py`）看到它才会对**这个序号**执行"platforms\ 里
#   两份都在 + 从产物目录真跑 `--smoke` 要求 failed=0"。修前打的包（ALPHA.57/58/59）
#   没有这一行 ⇒ 门禁只能记台账（它们确实缺 qoffscreen）；**一旦声明了就必须成立**。
#   为什么不让门禁无条件跑：`dist\` 是运行期产物、干净检出时没有包，而无条件跑会让
#   修前留下的旧序号把 release 档判红（与本批无关的红）—— 声明制把这点分清楚了。

Write-Host "== 打包判据：从包目录跑一次离屏自检 =="
# ★★ 2026-10-07（用户裁决）：**offscreen 平台插件随包携带**（不再"跑完删掉"）。
#
# 事实（修前）：`windeployqt` 只会铺 `qwindows.dll`（它按 app 的实际调用挑），而
# `--smoke` / `--layout-report` 走**离屏**。原来的处置是"跑判据前临时拷进去、跑完删掉
# （保持包精简）" ⇒ 后果是**产物本身不可自检**：拿包内 exe 直接跑 `--smoke` 会 fail-fast
# `0xC0000409`（WER: BEX64 / 故障模块 Qt6Core.dll / P7=0x267a8 —— ALPHA.57/58/59 是同一个签名）。
#
# 裁决（用户 2026-10-07）：**部署舞台必须同时带 qwindows.dll 与 qoffscreen.dll**。
# 体积账：qoffscreen.dll 109 KB · qwindows.dll 908 KB ⇒ 用 109 KB 换"判据＝从产物里真跑一次"，划算。
# 判据（都能判红）：
#   ① 源侧：Qt 的 `plugins\platforms\qoffscreen.dll` 必须存在（缺 ⇒ 明确报错，不静默降级）；
#   ② 产物侧：包内 `platforms\` **同时**有 qwindows.dll + qoffscreen.dll；
#   ③ 真跑：从**包目录**跑 `xrkit_app.exe --smoke`（QT_QPA_PLATFORM=offscreen）⇒
#      `SHELL-SMOKE ok checks=N failed=0` 且退出码 0。
#   ★ 判红办法：把下面 copy 那两行删掉（⇒ ①②红）；把 `failed=0` 那段放宽回只 grep "ok"
#     （⇒ 有失败也绿）。
$off = Join-Path $qt 'plugins\platforms\qoffscreen.dll'
$stagePlat = Join-Path $stage 'platforms'
if (-not (Test-Path $off)) {
    throw "找不到 $off —— 包内 `--smoke`/`--layout-report` 走离屏，少了它就是 0xC0000409（用户 2026-10-07 裁决：随包携带）"
}
New-Item -ItemType Directory -Force -Path $stagePlat | Out-Null
Copy-Item -LiteralPath $off -Destination $stagePlat -Force
$prevQpa = $env:QT_QPA_PLATFORM
$env:QT_QPA_PLATFORM = 'offscreen'
$smokeOk = $false
try {
    # ★ 原生程序往 stderr 打警告（Qt 的 QFontDatabase 之类）在 `$ErrorActionPreference='Stop'`
    #   下会被 PowerShell 当成终止错误 ⇒ 这里临时改成 Continue，并把两边输出一起收进日志再挑结论行。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $log = Join-Path $stage 'smoke.log'
    $errLog = Join-Path $stage 'smoke.err.log'
    try {
        # ★ 2026-09-28：主程序改成 **GUI 子系统**（修用户报的"打开弹 cmd"）之后，
        #   PowerShell 的 `& exe args *> log` **不会等待 GUI 程序结束**、也不完整捕获输出
        #   ⇒ 抓不到 `SHELL-SMOKE ok`，打包被误判成失败（实测）。
        #   `Start-Process -Wait -PassThru` 对 GUI 程序是等进程结束的，还能拿 ExitCode
        #   （比只 grep 一行更硬：日志里有 ok 但退出码非 0 也要拦住）。
        #   ★ 2026-10-07：结论行判据收窄成 `... failed=0` —— 修前只 grep `SHELL-SMOKE ok`，
        #   而 smoke 的收尾行**恒**以 `ok` 开头（`SHELL-SMOKE ok checks=741 failed=1` 也是 "ok"）
        #   ⇒ 一旦只看那一行，"有 1 条判据不过"就会被读成通过（本次实测见过 failed=1 的那行）。
        $proc = Start-Process -FilePath (Join-Path $stage 'xrkit_app.exe') -ArgumentList '--smoke' `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError $errLog
        $line = Select-String -Path $log -Pattern 'SHELL-SMOKE ok checks=(\d+) failed=0' | Select-Object -First 1
        # ★★ 2026-10-05（本批 #8）：**包内自检要把插件数与名字打出来** —— 那一行由 smoke 自己发
        #   （`SHELL-SMOKE note #8 包内插件：N 个 dll（<名字>）· 加载器 loaded=M · dir=…`）。
        #   为什么不用"包内 CLI 跑 `--plugins`"：包里**只有 xrkit_app.exe**（发版规格 §1），
        #   没有 CLI 可跑 ⇒ 上一版那条判据是**自相矛盾**的（开发版必被 throw，实测打包直接失败）。
        #   改成读**包内 exe 自己**的那一行：它既数了目录，又给了**加载器**的真结论。
        $pluginNote = Select-String -Path $log -Pattern '#8 包内插件：' | Select-Object -First 1
        if ($line -and $proc.ExitCode -eq 0) {
            Write-Host "  $($line.Line.Trim())  (exit=$($proc.ExitCode))"
            $smokeOk = $true
        } else {
            Write-Host "  包内自检退出码=$($proc.ExitCode)"
            Get-Content -LiteralPath $log -Tail 10 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
            Get-Content -LiteralPath $errLog -Tail 10 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        }
    } finally {
        $ErrorActionPreference = $prevEap
        Remove-Item -Force $log, $errLog -ErrorAction SilentlyContinue
    }
} finally {
    # ★ 2026-10-07（H1-5 同族）：原来是 `$env:QT_QPA_PLATFORM = ''`（**覆写后不还原**，
    #   把"未设置"变成一个空字符串的环境变量）⇒ 现在按原值还原（原本没有就删掉）。
    if ($null -eq $prevQpa) {
        Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    } else {
        $env:QT_QPA_PLATFORM = $prevQpa
    }
}
if (-not $smokeOk) { throw "包内 exe 的离屏自检没通过（结论行要求 `SHELL-SMOKE ok … failed=0` 且退出码 0）" }

# ★★ 2026-10-07（用户裁决后半）：**平台插件必须在产物里**（不是"跑判据时临时有"）。
#   这条与上面那次"从产物里真跑"合起来才成立：判据本身要能在**用户拿到的那个目录**里重跑。
foreach ($plat in @('qwindows.dll', 'qoffscreen.dll')) {
    if (-not (Test-Path (Join-Path $stagePlat $plat))) {
        throw "包内 platforms\ 缺 $plat —— 用户裁决：部署舞台必须同时带 qwindows.dll 与 qoffscreen.dll"
    }
}
Write-Host "  平台插件判据通过：包内 platforms\ 同时有 qwindows.dll + qoffscreen.dll"

# ★★ 2026-10-05（本批 #8）：**开发版包内带全部插件** —— 判据落在**包内自检那一行**上。
#   口径（任务书 #8）：开发版把已构建插件（含测试插件）复制进包 `plugins\`；发行版仍为空
#   （既定策略：NLC 解密插件不随包发布）。
#   判据 = 开发版 `plugins\` **非空** + **包内自检打印插件数与名字**（`#8 包内插件：N 个 dll（…）
#   · Hub 启动装载 errors=M`），且 `N >= 1`、`M == 0`（加载失败会以 RegistrationError 的形式
#   出现在 `Hub::plugin_errors()` 里 —— 实测 `not_a_plugin.dll` 那种就是 LoadLibrary 失败）；
#   发行版 `N == 0`。
#   ★ 判红办法：把上面复制插件那一段删掉（⇒ N=0）⇒ 立刻红；把 smoke 里那一段 note 删掉
#     （⇒ 找不到那一行）⇒ 也红。
if ($null -eq $pluginNote) {
    throw "包内自检没有打出「#8 包内插件：…」那一行（插件数与名字必须可见）"
}
$pluginText = $pluginNote.Line.Trim()
$pluginCount = if ($pluginText -match '插件：(\d+)\s*个\s*dll') { [int]$Matches[1] } else { -1 }
$pluginLoaded = if ($pluginText -match 'errors=(\d+)') { [int]$Matches[1] } else { -1 }
$stagePlugins = @(Get-ChildItem -LiteralPath (Join-Path $stage 'plugins') -Filter '*.dll' -File `
                    -ErrorAction SilentlyContinue)
Write-Host ("  包内 plugins\：{0} 个 dll：{1}" -f $stagePlugins.Count,
    $(if ($stagePlugins.Count -gt 0) { ($stagePlugins | ForEach-Object { $_.Name }) -join ', ' }
      else { '(空)' }))
Write-Host ("  包内自检那一行：{0}" -f $pluginText)
if ($isDev) {
    if ($pluginCount -lt 1 -or $stagePlugins.Count -lt 1) {
        throw ("开发版的 plugins\ 是空的（自检报 {0} 个 / 目录 {1} 个）—— 用户约定：开发版包内带全部插件（#8）" -f `
            $pluginCount, $stagePlugins.Count)
    }
    if ($pluginLoaded -ne 0) {
        throw ("开发版包内插件**装载有失败**（Hub 启动装载 errors={0}）—— 见包内自检的日志" -f $pluginLoaded)
    }
    Write-Host ("  插件自检通过：{0} 个 dll（自检与目录一致）、Hub 启动装载 errors=0" -f $pluginCount)
} else {
    if ($pluginCount -ne 0 -or $stagePlugins.Count -ne 0) {
        throw ("发行版的 plugins\ 必须为空（既定策略：NLC 解密插件不随包发布），实测自检 {0} / 目录 {1}" -f `
            $pluginCount, $stagePlugins.Count)
    }
    Write-Host "  插件自检通过：发行版 plugins\ 为空（既定策略）"
}

# ★ 2026-09-28 用户口径（原话）：
#   「构建出来不要使用压缩包的形式，因为我是单人开发，压缩包方便分发但不方便测试。
#     构建成功的不要加时间戳，看上去太乱了。」
#   ⇒ 交付物是**一个可以直接跑的目录**（测试用），压缩包只是**顺带**给分发用；
#     成功件名**确定性**（不加时间戳，同一版本重复构建就覆盖它自己）；
#     版本层级照用户口径：主版本（2.0.0 / 2.0.1…）→ 标记（ALPHA/BETA/RC）。

Write-Host "== 落到版本目录（只出目录；历史按序号共存，不删）=="
$dist = Join-Path $root $OutDir
# 版本口径**只有一处**：version.json。`-VersionString` 没传（比如手工跑）就从它读 current。
# （原来没这一步：CLI 调 package.ps1 时其实也不传参 ⇒ 会落到 `<主版本>\dev\`，那是错的）
if ($VersionString -eq '') {
    $vj = Join-Path $root 'version.json'
    if (Test-Path $vj) {
        $vs = Get-Content -LiteralPath $vj -Raw -Encoding UTF8 | ConvertFrom-Json
        $cur = $vs.current
        if ($cur -and $cur.version) {
            $VersionString = "{0} {1}.{2}" -f $cur.version, $cur.prerelease, $cur.index
        }
    }
}
# 从 `-VersionString`（形如 "2.0.0 ALPHA.13"）拆出 主版本 / 标记 / 序号
$verMajor = '2.0.0'
$marker   = 'ALPHA'
$idx      = 0
if ($VersionString -match '^\s*(\S+)\s+([A-Za-z]+)\.?(\d*)') {
    $verMajor = $Matches[1]
    $marker   = $Matches[2].ToUpper()
    if ($Matches[3] -ne '') { $idx = [int]$Matches[3] }
}
if ($idx -le 0) {
    # 没拿到就退回 version.json 的 current.index（版本口径只有一处）
    $vj2 = Join-Path $root 'version.json'
    if (Test-Path $vj2) {
        $cur2 = (Get-Content -LiteralPath $vj2 -Raw -Encoding UTF8 | ConvertFrom-Json).current
        if ($cur2 -and $cur2.index) { $idx = [int]$cur2.index }
    }
}
if ($idx -le 0) { $idx = 1 }
$verDir = Join-Path $dist $verMajor
$preDir = Join-Path $verDir $marker
New-Item -ItemType Directory -Force -Path $preDir | Out-Null

# ★ 用户 2026-09-29 四问的两条口径：
#   ③ 名字要干净 ⇒ `<主版本>\<标记>\<标记>.<序号>\`（例 `dist\2.0.0\ALPHA\ALPHA.13\`）：
#      不要再出现 `STALKER_Toolkit_` 前缀、重复的主版本、以及 commit 那一坨。
#   ② 不要再有压缩包 ⇒ **只出目录**（目录本身就能直接跑；要分发时再单独说）。
#   ① 历史版本**不删** ⇒ 只覆盖"同一个序号"自己的目录（重建同一序号＝覆盖它自己），别的序号一律不动。
$pkgName = "{0}" -f $idx   # 只留序号；主版本/标记已经在上层目录里了（用户 2026-09-29 第 1 条）
$pkgDir  = Join-Path $preDir $pkgName
if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }
Move-Item -LiteralPath $stage -Destination $pkgDir
Write-Host "PACKAGE OK: $pkgDir"
 
