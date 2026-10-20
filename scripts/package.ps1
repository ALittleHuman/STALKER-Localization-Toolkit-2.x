#requires -Version 5.1
<#
  2.0.0 打包（M3 第一步）：把 msvc-shell 的产物做成一个**可回退**的发行包。

  口径（见 docs/2.0.0-打包与发版规格.md）：
   ① 包内**不带** ffmpeg、**不带** NLC 解密插件、**不带**实验性引擎补丁；`plugins\` 目录**存在但为空**
      （插件由用户自己放；发行包为空是既有决策）；
      ★★ P17-81（甲）：**产品实现＝C++ 内置六页**；`plugins\*_page.py` 与 `hello.py` 一律是
      **插件 API 能力验证夹具（非产品）** ⇒ 开发版把它们放在 `plugins\_fixtures\`（加载器**不递归**，
      因而随包携带、默认不加载），发行版**一枚都不进包**。两份形态的包内自检都断言
      "默认可见页＝六个内置页"（见下面"包内自检"那一段）。
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
    [bool]$Dev = $false,
    # ★★ 决策 4（用户 2026-10-18/19）：**只算计划、不动盘**。
    #   `-PlanOnly` 输出"每个文件落到哪"的 JSON 就退出（不复制、不写 version.txt、不做自检）；
    #   与真打包**共用同一批分类规则**（`Get-PackageDest`）—— 判据验的是这份规则，
    #   不是另写一套模拟流程。
    [switch]$PlanOnly,
    # 计划模式可以指向一个**假源树**（判据用临时目录造夹具）；缺省＝真实源
    [string]$SourceRoot = ''
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root 'build\package_stage'
$shell = Join-Path $root 'build\msvc-shell'
# ★★ P17-81（甲：**产品实现＝C++ 内置六页**；`plugins\*_page.py` 与 `hello.py` 一律降级为
#   **插件 API 能力验证夹具**，非产品）：随包插件不再落进 `plugins\` 顶层，而是 `plugins\_fixtures\`。
#   为什么：顶层是**加载器扫描路径**（`loader.cpp:108` / `py_host.cpp:107/3428` 三处都是
#   **非递归**的 `directory_iterator`）⇒ 放子目录＝"随包携带、默认不加载"。
#   于是"同一个 exe 里有没有 `plugins\` 决定它跑哪一份实现"这件事（P17-78 的致命项）消失：
#   **开发包与发行包默认都跑内置六页**（各自的包内自检都会断言这件事）。
#   ★ 夹具页仍然被验：`package.ps1` 与门禁**都把 `_fixtures\` 显式抬到顶层跑一次**
#     （同一次运行里既取包内插件计数、又断"插件页真的生效"）—— 见下面"包内自检"那一段的口径。
$pluginDrop = Join-Path $stage 'plugins\_fixtures'
$qt    = 'E:\Software\Tools\Qt\6.8.3\msvc2022_64'

# ══ 决策 4：**分类规则**（计划与真打包共用这一份，不许写第二套）════════════════
#   口径（用户 2026-10-18/19）：
#     · 必须留根：`xrkit_app.exe`、`python3XX.dll`/`python3.dll`（exe 的导入表）、
#       `vcruntime140*.dll`、`Qt6*.dll`、README/version/LICENSE；
#     · `bin\`：`ffmpeg.exe`、`ffprobe.exe`、`download_ffmpeg.py`；
#     · `python\`：embeddable 的其余件（*.pyd、python3*.zip、python3*._pth、python.exe/
#       pythonw.exe/python.cat、sqlite3.dll、libcrypto-3.dll、libssl-3.dll、libffi-8.dll）；
#     · **不进包**：运行期产物 `logs\*`、`user.ltx`（自检是重定向到 %TEMP% 的，
#       包里出现它们只说明"有人在舞台上跑过 exe"）。
#   返回 '' 表示留根；返回 $null 表示**不进包**。
function Get-PackageDest {
    param([string]$RelPath)
    $name = Split-Path $RelPath -Leaf
    $lower = $name.ToLowerInvariant()
    if ($RelPath -match '(^|[\\/])(logs)([\\/]|$)' -or $lower -eq 'user.ltx') { return $null }
    if ($lower -eq 'xrkit_app.exe') { return '' }
    if ($lower -in @('ffmpeg.exe', 'ffprobe.exe', 'download_ffmpeg.py')) { return 'bin' }
    if ($lower -in @('readme.txt', 'version.txt', 'license.txt')) { return '' }
    if ($lower -match '^(python3\d*|pythonwd?)\.dll$') { return '' }
    if ($lower -match '^(vcruntime140.*|qt6.*)\.dll$') { return '' }
    if ($lower -match '\.pyd$') { return 'python' }
    if ($lower -match '^python3.*\.(zip|_pth)$') { return 'python' }
    if ($lower -in @('python.exe', 'pythonw.exe', 'python.cat', 'sqlite3.dll',
                     'libcrypto-3.dll', 'libssl-3.dll', 'libffi-8.dll')) { return 'python' }
    return ''
}

$exe = Get-ChildItem -Path $shell -Recurse -Filter 'xrkit_app.exe' -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $exe) { throw "找不到 msvc-shell 的 xrkit_app.exe（先跑 scripts\build.ps1 -Preset msvc-shell）" }
if (-not (Test-Path (Join-Path $qt 'bin\windeployqt.exe'))) { throw "找不到 windeployqt：$qt" }

# ★★ 2026-10-20（P17-176 的真因）：**打包前不许有残留进程**。
#   那次"vcruntime140.dll 权限不足"十几次，最后查明是**自检跑完没退干净的 xrkit_app**
#   还在从 dist 目录里跑、把 DLL 映射住了 ⇒ Copy-Item 报 PermissionDenied。
#   ⇒ 这里先查一遍：占着就直接拒绝并说清楚（不自动杀：用户可能正在用界面）。
$busy = @(Get-Process xrkit_app, xrkit_cli, ffmpeg, ffprobe -ErrorAction SilentlyContinue |
          Where-Object { $_.Id -ne $PID })
if ($busy.Count -gt 0) {
    # 用户口径（2026-10-20）：「要么明确拒绝并提示先关掉，要么**停止并写日志**」。
    #   ★★ 但**绝不能杀自己的父进程**：`xrkit_cli --build-package` 就是靠 `package.ps1` 打包的，
    #     而它也叫 `xrkit_cli.exe` —— 第一版预检把父进程杀了 ⇒ `_pkg.py` 每次都以 -1 结束、
    #     包里一个文件都没有（P17-181 连挂两次）。所以：`xrkit_cli` 只**提示不杀**。
    foreach ($proc in $busy) {
        if ($proc.ProcessName -eq 'xrkit_cli') {
            Write-Host ("PACKAGE warn 有 xrkit_cli 在跑（PID {0}）—— 很可能就是本次打包的父进程，**不杀**" -f $proc.Id)
            continue
        }
        Write-Host ("PACKAGE stop-process {0} (PID {1}) {2}" -f $proc.ProcessName, $proc.Id, $proc.Path)
        try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {
            throw ("停不掉进程 {0} (PID {1})：{2}`n  它是 vcruntime140.dll 复制失败的典型原因，先手工关掉再打包。" -f `
                   $proc.ProcessName, $proc.Id, $_.Exception.Message)
        }
    }
    Start-Sleep -Milliseconds 800
    $still = @(Get-Process xrkit_app, ffmpeg, ffprobe -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $PID })
    if ($still.Count -gt 0) {
        throw ("打包前仍有 {0} 个进程占着文件 —— 先关掉再打包（否则 vcruntime140.dll 会 PermissionDenied）。" -f $still.Count)
    }
}

# ══ 决策 4：`-PlanOnly` —— 只算计划、不动盘 ══════════════════════════════════
#   用法：pwsh -File scripts/package.ps1 -PlanOnly [-SourceRoot <目录>]
#   输出一行 JSON（`PLAN ` 前缀），内容包括：每个文件的 `src/dest`、被排除的运行期产物、
#   以及**根目录散文件数**。真打包走的是同一批规则（`Get-PackageDest`）。
if ($PlanOnly) {
    $srcRoot = if ($SourceRoot) { $SourceRoot } else { $shell }
    if (-not (Test-Path $srcRoot)) { throw "PlanOnly：源目录不存在 $srcRoot" }
    $files = @(Get-ChildItem -LiteralPath $srcRoot -Recurse -File -ErrorAction SilentlyContinue)
    $entries = @()
    $excluded = @()
    foreach ($f in $files) {
        $rootFull = (Resolve-Path -LiteralPath $srcRoot).Path
        $rel = $f.FullName.Substring($rootFull.Length).TrimStart('\', '/')
        $dest = Get-PackageDest -RelPath $rel
        $leaf = $f.Name
        if ($null -eq $dest) {
            $excluded += [ordered]@{ src = $rel; reason = '运行期产物，不进包' }
            continue
        }
        $target = if ($dest -eq '') { $leaf } else { "$dest/$leaf" }
        $entries += [ordered]@{ src = $rel; dest = $target; dir = $dest }
    }
    $rootCount = @($entries | Where-Object { $_.dir -eq '' }).Count
    $payload = [ordered]@{
        plan        = 'package-structure/v1'
        source_root = $srcRoot
        layout      = 'bin+python'
        entries     = $entries
        excluded    = $excluded
        root_files  = $rootCount
    }
    Write-Host ("PLAN " + ($payload | ConvertTo-Json -Depth 6 -Compress))
    Write-Host ("PLAN summary files={0} root={1} bin={2} python={3} excluded={4}" -f `
        $entries.Count, $rootCount,
        @($entries | Where-Object { $_.dir -eq 'bin' }).Count,
        @($entries | Where-Object { $_.dir -eq 'python' }).Count,
        $excluded.Count)
    exit 0
}

Write-Host "== 清理舞台 =="
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'plugins') | Out-Null

Write-Host "== 复制主程序 =="
Copy-Item -LiteralPath $exe.FullName -Destination $stage

# ★★ 插件改装 P2a/P2c（2026-10-07）：**CPython 运行期 + 标准库都随包**。
#   引擎链接了 `python3XX.lib`（插件＝`.py`，宿主在引擎层）：
#     · 缺 `python3XX.dll` ⇒ **主程序根本起不来**（不是"插件不可用"）；
#     · 缺 `python3XX.zip`（标准库）⇒ 宿主起不来（`Failed to import encodings module`），
#       而开发态之所以"看起来能跑"，是因为构建期把**本机 Python 路径**烘焙进了
#       `XRKIT_PYTHON_HOME_DEFAULT` —— 发行到**没装 Python 的机器**上就露馅。
#   来源＝vendored 的 **embeddable Python**（`python tools/fetch_third_party.py --only python-embed`），
#   **扁平铺进包根**：embeddable 包本来就是"解压即用"的扁平布局（DLL/.zip/*.pyd 同层），
#   解释器按 `resolve_python_home` 的"exe 旁有 `python3XX.zip`"这条认出它（引擎侧判据见 py_host.cpp）。
#   ★ 判据化：**缺件就 throw** —— 宁可打包失败，也不许打出一个双击起不来的包。
$pyEmbed = Join-Path $root 'third_party\python-embed'
$pyZip = @(Get-ChildItem -LiteralPath $pyEmbed -Filter 'python3*.zip' -File -ErrorAction SilentlyContinue)
if ($pyZip.Count -eq 0) {
    throw ("找不到随包用的 embeddable Python（$pyEmbed\python3*.zip）。`n" +
           "  先跑：python tools\fetch_third_party.py --only python-embed`n" +
           "  （用户口径：**发布随包自带 embeddable Python**，发行到没装 Python 的机器也要能起插件宿主）")
}
$pyFiles = @(Get-ChildItem -LiteralPath $pyEmbed -File -Recurse)
if ($pyFiles.Count -eq 0) { throw "embeddable Python 目录是空的：$pyEmbed" }
# ★★ 决策 4：**逐文件按 `Get-PackageDest` 落位**（与 `-PlanOnly` 同一份规则）。
#   `python3XX.dll` / `python3.dll` 留根（exe 的导入表要它们）；其余进 `python\`。
$pyMoved = 0
foreach ($f in $pyFiles) {
    $dest = Get-PackageDest -RelPath $f.Name
    if ($null -eq $dest) { continue }
    $target = if ($dest -eq '') { $stage } else { Join-Path $stage $dest }
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $target -Force
    if ($dest -ne '') { ++$pyMoved }
}
Write-Host ("  CPython：留根 {0} 个，进 python\ {1} 个" -f ($pyFiles.Count - $pyMoved), $pyMoved)
Write-Host ("  CPython 运行期 + 标准库：{0} 个文件（{1:N1} MB，来源 third_party\python-embed）" `
            -f $pyFiles.Count, (($pyFiles | Measure-Object -Property Length -Sum).Sum / 1MB))

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
#   口径（本批定；★★ P17-81 改定"落点"那一半，其余不变）：
#     · **开发版**（标记 ∈ {ALPHA, BETA}）⇒ 把已构建插件**复制进包**，但落点是
#       `plugins\_fixtures\`（**夹具**，非产品；见文件头 `$pluginDrop` 的理由）；
#     · **发行版**（标记 ∈ {RC, FINAL}，或显式 `-Dev:$false`）⇒ **仍按既定策略**：
#       `plugins\` 存在但为空、**NLC 解密插件不随包发布**，且**夹具一枚都不进包**。
#   ★ 判据（可判红，P17-81 改写：开发版与发行版**同向**）：`plugins\` **顶层必须为空**
#     ＋ 包内自检的"可见页集合"必须是**六个内置页** ＋ `_fixtures\` 的夹具枚数与清单一致
#     （发行版：`_fixtures\` 也必须为空）。
#     ⇒ 把这一段删掉 / 把落点改回顶层 / 把可见页那个 note 删掉，都会立刻红。
$isDev = ($Prerelease -match '^(?i)ALPHA|BETA')   # 标记口径见 docs/2.0.0-打包与发版规格.md
if ($PSBoundParameters.ContainsKey('Dev')) { $isDev = $Dev }   # 显式 -Dev 覆盖标记推导
$copiedPlugins = @()
if ($isDev) {
    Write-Host "== 开发版：插件作为**夹具**随包携带（落 plugins\_fixtures\，默认不加载）=="
    # 夹具落点要先建出来（P17-81：`plugins\` 顶层保持为空 —— 它是加载器扫描路径）。
    New-Item -ItemType Directory -Force -Path $pluginDrop | Out-Null
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
                Copy-Item -LiteralPath $_.FullName -Destination $pluginDrop -Force
                $copiedPlugins += $_.Name
            }
    }
    # ★★ P16-2a（切打包口径）：开发版带**随包的全部 Python 插件** ——
    #   六页插件版（fs/convert/text/xml/video/fontpack）＋ **三插件同页**（`toolchain_page.py`）。
    #   ★ 这里是**显式清单**（不放宽）：这 7 枚必须都在源里、也都必须进包；少一枚立刻 throw。
    #     修前只带一枚 `hello.py` 夹具 ⇒ "六页 + 三插件进包"这件事在包里**根本没发生**，
    #     而包内自检又只看 dll ⇒ 谁把这一段删掉都不会红（切打包口径要修的就是这个）。
    $expectedPages = @(
        'fs_page.py', 'convert_page.py', 'text_page.py',
        'xml_page.py', 'video_page.py', 'fontpack_page.py',
        'toolchain_page.py'
    )
    $srcPlugins = Join-Path $root 'plugins'
    foreach ($name in $expectedPages) {
        $srcPlugin = Join-Path $srcPlugins $name
        if (-not (Test-Path $srcPlugin)) {
            throw "随包插件缺失：$srcPlugin（开发版包内必须带齐六页插件版 + 三插件同页）"
        }
        Copy-Item -LiteralPath $srcPlugin -Destination $pluginDrop -Force
        $copiedPlugins += $name
    }
    # ★★ 插件改装 P2b/P2c：另外仍带一枚 **最小 Python 夹具** —— 它验的是"包内 exe 能不能
    #   加载 `.py`"这一条最短路（与上面 7 枚真实插件互补）。
    #   发行版不带它（与"发行口径包里 `plugins\` 顶层与 `_fixtures\` 都为空"同一条口径）。
    $pyFixture = Join-Path $root 'tests\python_plugins\hello.py'
    if (-not (Test-Path $pyFixture)) {
        throw "找不到 Python 插件夹具：$pyFixture（开发版包内自检要真验 `.py` 加载链）"
    }
    Copy-Item -LiteralPath $pyFixture -Destination $pluginDrop -Force
    $copiedPlugins += 'hello.py'
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
# ★★ 决策 4：ffmpeg/ffprobe 进 `bin\`（`find_ffmpeg` 已认这个落点；根目录优先的老口径不变）
$binDir = Join-Path $stage 'bin'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
Copy-Item -LiteralPath (Join-Path $depSrc 'ffmpeg.exe') -Destination $binDir
Copy-Item -LiteralPath (Join-Path $depSrc 'ffprobe.exe') -Destination $binDir
# ★★ 2026-10-15（P17-123）：**这条守卫原来自己把 ffmpeg 掉死** ——
#   `& ffmpeg -version 2>&1 | Select-Object -First 1` 拿到首行就**掉断上游管道**
#   ⇒ ffmpeg 被终止 ⇒ `$LASTEXITCODE` 变成 -1（终止码）⇒ 下一行误报「起不来」。
#   日志里**明明打出了正确的版本行**，这正是与事实相反的证词；且**间歇**
#   （取决于 ffmpeg 在输出首行之后是否还活着）。
#   ⇒ 改成**先落文件、再读首行**：管道不截断，退出码就是它真实的退出码。
#   ★ 三重守卫（起不来 / 版本行认不出 / 主版本 ≠ 7）**一条不放松**。
$ffLog = Join-Path $env:TEMP ('xrkit_ffprobe_{0}.txt' -f $PID)
# ★★ P17-128：同理包一层 `Continue` —— ffmpeg 也会往 stderr 写正常信息
#   （它的 `-version` 输出本来就包含配置/库列表）。真失败仍然靠下面那条 `-ne 0` 拦。
$prevEapFf = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& (Join-Path $binDir 'ffmpeg.exe') -version *> $ffLog
$ffExit = $LASTEXITCODE
$ErrorActionPreference = $prevEapFf
$ffVersion = (Get-Content -LiteralPath $ffLog -TotalCount 1)
Remove-Item -LiteralPath $ffLog -Force -ErrorAction SilentlyContinue
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
Copy-Item -LiteralPath (Join-Path $root 'scripts\download_ffmpeg.py') -Destination $binDir

Write-Host "== windeployqt（铺 Qt 运行时；不铺翻译/示例）=="
# ★★ P17-128（复审计 P2-1 同族的发行链缺口）：**这里得包一层 `Continue`** ——
#   `$ErrorActionPreference='Stop'` 下，`windeployqt` 往 **stderr** 写的正常警告
#   （实测："Translations will not be available…"）会被当成**终止错误** ⇒ 脚本中断。
#   ★ 本脚本**早就有这个惯用法**（见 `:370-406`、`:433-473`），这里是**漏了**；
#     只包这一次调用，**不**动全局的 `Stop`（真失败仍然要爆）。
$prevEapQt = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& (Join-Path $qt 'bin\windeployqt.exe') --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw (Join-Path $stage 'xrkit_app.exe')
$qtExit = $LASTEXITCODE
$ErrorActionPreference = $prevEapQt
if ($qtExit -ne 0) { throw "windeployqt 失败（退出码 $qtExit）" }

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
# ★★ P17-81：口径改为"**开发版把插件作为夹具带在 `plugins\_fixtures\`**（非产品、默认不加载）"——
#   顶层 `plugins\` 现在**真的**是空的（加载器只扫顶层、非递归），所以说明书必须照这个说，
#   不许再写"plugins\ 已带 N 个插件"（那是夹具被当产品的旧口径，正是 P17-78 的根）。
if ($isDev) {
    $pluginLine = if ($copiedPlugins.Count -gt 0) {
        # ★★ P17-81 实测踩到的第二个坑：`("A" + "B" -f x)` 这种**拼接后面直接挂 `-f`** 的写法
        #   在 PS 5.1 与 pwsh 下**都不做格式化**（`-f` 被当成拼接链的一部分，`{0}` 原样落地）——
        #   README 是**用户可见面**，印出 `{0}` 就是谎报（零静默红线同源）。
        #   可靠写法：**先把串拼好，再单独一句 `-f`**（实测 PS 5.1 / pwsh 都正确）。
        $pluginLineHead = ("插件：本包是**开发版**，随包夹具 {0} 个文件（1 个 dll + {1} 个 py）在 " +
                           "plugins\_fixtures\ 子目录（**非产品**：产品实现是 C++ 内置六页；" +
                           "该子目录**不参与加载** —— `plugins\` 顶层为空、程序默认跑的仍是内置六页）：{2}。")
        $pluginLineHead -f $copiedPlugins.Count, ($copiedPlugins.Count - 1), ($copiedPlugins -join ', ')
    } else {
        "插件：本包是**开发版**，但构建产物里没有 xrkit_*_plugin.dll，plugins\ 为空。"
    }
} else {
    $pluginLine = "插件：plugins\ 为空目录（发行版策略：NLC 解密插件闭源、不随包发布；" +
                  "随包夹具一律不进发行口径包）。需要时把 <name>.dll 放进去，重启程序。"
}
@"
STALKER 汉化工具集 2.0.0（xrkit）
--------------------------------
安装：整个目录解压到任意位置，双击 xrkit_app.exe。
回退：删除该目录即可（本工具**不会自动安装或修改游戏文件**；
      唯一的自动写盘位置是用户设置 user.ltx，删除它即恢复默认）。
注意：解包（以及封包/字库产物）写到**您选择的目录**；但**默认落点是"来源所在目录"** ——
      直接点开始、没改落点的话，产物会落进**来源包所在的游戏/模组目录**里。请先确认落点。
$pluginLine
      插件加载失败不会阻塞启动，问题会显示在启动后的日志栏里。
运行日志（logs\log_*.txt）里既有引擎行、也有界面上出现的每一行；没有「导出日志」按钮 ——
落盘文件已含全部内容，导出是重复实现。需要发日志时直接附上这个文件。
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
# ★★ 插件改装 P2c：把"随包带了哪个 CPython 运行期/标准库"写进**包自己的契约声明** ——
#   门禁（tests\run_ci.py 的 `[python-embed]` 步）看到这一行，才会对**这个序号**执行
#   "从产物目录跑 `xrkit_app.exe --python-host` 真能起宿主"（口径同 `platform_plugins=`：
#   一旦声明就必须成立；早期没声明的包只能记台账，不许无条件跑 —— dist\ 是运行期产物）。
$pyZipName = (Get-ChildItem -LiteralPath $pyEmbed -Filter 'python3*.zip' -File |
              Select-Object -First 1).Name
$pyDllName = (Get-ChildItem -LiteralPath $pyEmbed -Filter 'python3*.dll' -File |
              Select-Object -First 1).Name
$pyContract = "zip=$pyZipName,dll=$pyDllName"
@"
version=$verLine
index=$BuildIndex
commit=$commit
built=$stamp
platform_plugins=qwindows,qoffscreen
python_runtime=$pyContract
layout=bin+python
exe_identity=$exeIdentity
"@ | Set-Content -LiteralPath (Join-Path $stage 'version.txt') -Encoding UTF8

# ★★ 决策 4 验收：**包根散文件 ≤ 12**（"外面的文件太多了"变成判据）。
$rootFiles = @(Get-ChildItem -LiteralPath $stage -File)
Write-Host ("  包根散文件：{0} 个" -f $rootFiles.Count)
if ($rootFiles.Count -gt 12) {
    throw ("包根散文件 {0} 个（上限 12）：{1}`n  分类规则在 `Get-PackageDest` 里；多出来的该进 python\\ 或 bin\\。" -f `
           $rootFiles.Count, (($rootFiles | Select-Object -ExpandProperty Name | Sort-Object) -join ', '))
}

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
        # ★★ P17-81：**可见页集合**那一行 —— 交付物断言（"默认可见＝六个内置页"）的唯一依据。
        #   与 `$pluginNote` 取自**同一次**运行（P17-79 §十·2 的"同机制"）。
        $pageNote = Select-String -Path $log -Pattern 'PLUGIN-PAGES 可见页集合=' | Select-Object -First 1
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

# ★★ 插件改装 P2a/P2c：**包内 Python 判据** —— 起宿主 + 加载 `.py`，全部在**包目录里**跑
#   （`xrkit_app.exe` 的同名开关与 CLI 共用引擎层的**唯一一份**实现 `run_python_host_probe`）。
#   为什么必须在包内跑：开发态的宿主用的是"构建期烘焙的本机 Python 路径"，**只有包内那一份**
#   才是 `exe 旁 python3XX.zip` ⇒ 只有从包里跑，才真的验了"发行到没装 Python 的机器上也能起插件"。
$prevEapPy = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $pyLog = Join-Path $stage 'pyhost.log'
    $pyErr = Join-Path $stage 'pyhost.err.log'
    $pyProc = Start-Process -FilePath (Join-Path $stage 'xrkit_app.exe') -ArgumentList '--python-host' `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $pyLog -RedirectStandardError $pyErr
    $pyLine = Select-String -Path $pyLog -Pattern 'PYHOST enabled=1 initialized=1 gil=released' |
        Select-Object -First 1
    if (-not $pyLine -or $pyProc.ExitCode -ne 0) {
        Get-Content -LiteralPath $pyLog -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        Get-Content -LiteralPath $pyErr -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        throw ("包内 CPython 宿主没起来（要求 `PYHOST enabled=1 initialized=1 gil=released` 且退出码 0；" +
               "实得退出码 $($pyProc.ExitCode)）—— 见 include/xrkit/engine/plugin/py_host.h 的口径")
    }
    Write-Host "  $($pyLine.Line.Trim())  (exit=$($pyProc.ExitCode))"
    Remove-Item -Force $pyLog, $pyErr -ErrorAction SilentlyContinue

    if ($isDev) {
        # 开发版才随包携带 Python 夹具（发行口径包里 `plugins\` 与 `_fixtures\` 都为空）。
        # ★★ P17-81：**这条链要显式指向 `_fixtures\`** —— 顶层 `plugins\` 现在是空的
        #   （夹具默认不加载，见文件头 `$pluginDrop`），照旧指顶层会得到 `loaded=0` ⇒ 假红。
        #   验的是"包内 exe 能不能加载 `.py`、注册能不能走通"这条链路，与"默认形态"无关；
        #   默认形态由下面那两行 note 断言（顶层 0 个插件 ＋ 可见页＝六个内置页）。
        $plugLog = Join-Path $stage 'pyplugins.log'
        $plugErr = Join-Path $stage 'pyplugins.err.log'
        $plugProc = Start-Process -FilePath (Join-Path $stage 'xrkit_app.exe') `
            -ArgumentList '--py-plugins', "`"$pluginDrop`"" `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $plugLog -RedirectStandardError $plugErr
        $plugLine = Select-String -Path $plugLog -Pattern 'PYPLUGINS files=\d+ loaded=[1-9]\d* .*errors=0' |
            Select-Object -First 1
        if (-not $plugLine -or $plugProc.ExitCode -ne 0) {
            Get-Content -LiteralPath $plugLog -Tail 8 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
            throw ("包内 Python 插件加载链没通（要求 `PYPLUGINS … loaded>=1 … errors=0` 且退出码 0；" +
                   "夹具目录 $pluginDrop；实得退出码 $($plugProc.ExitCode)）")
        }
        Write-Host "  $($plugLine.Line.Trim())  (exit=$($plugProc.ExitCode))"
        Remove-Item -Force $plugLog, $plugErr -ErrorAction SilentlyContinue
    }
} finally {
    $ErrorActionPreference = $prevEapPy
}

# ★★ P17-81 改写（甲：产品实现＝C++ 内置六页；`plugins\*_page.py` 降级为**夹具**）：
#   修前这里的口径是"开发版 `plugins\` **非空** ＋ 自检报出插件数"。夹具挪进 `plugins\_fixtures\`
#   之后（见文件头 `$pluginDrop`），那句口径**正好反过来** —— 顶层必须**为空**，否则就是
#   "夹具随包默认加载"（P17-78 的致命项：验收物 ≠ 交付物）。
#   现在两条断言都来自**包内自检自己的两行 note**（`src/shell/smoke.cpp`）：
#     ① `#8 包内插件：N 个文件（D 个 dll / P 个 py）…` —— **顶层**（＝加载器扫描路径）的插件数与名字；
#     ② `PLUGIN-PAGES 可见页集合=N 页（内置=B / 插件py=P）[1=fs …] 结束` —— **当前可见页**逐页名单。
#   断言（开发版与发行版**同向** —— 交付物口径一致）：
#     · 顶层没插件（①的 0 个文件/0 dll/0 py）；
#     · **可见页＝六个内置页**（②的内置=6 / 插件py=0 / 且名单恰是 6 个**无 `_py` 后缀**的 id）；
#     · 夹具仍在包里（`plugins\_fixtures\` 的枚数与 `$copiedPlugins` 清单一致）；
#     · 发行版额外：**整个包一个夹具都没有**（顶层与 `_fixtures\` 都为空）。
#   ★ **"插件页真的生效"** 这件事由**门禁**那一步显式把 `_fixtures\` 抬到顶层跑一次来断
#     （同一次运行里既取"抬上来之后的计数"、又断可见页＝七个插件页）——
#     两条断言**同一个机制**（不许分两处各修一半）。理由：包内自检必须验的是**交付物形态**
#     （默认），把夹具抬上去再跑属于"验夹具链路"，那条链路的入口在门禁侧（＝P17-79 §十·2）。
if ($null -eq $pluginNote) {
    throw "包内自检没有打出「#8 包内插件：…」那一行（插件数与名字必须可见）"
}
if ($null -eq $pageNote) {
    throw "包内自检没有打出「PLUGIN-PAGES 可见页集合=…」那一行（可见页名单必须可见）"
}
$pluginText = $pluginNote.Line.Trim()
# 新格式（P16-2a）：`#8 包内插件：N 个文件（D 个 dll / P 个 py）（名字…）· Hub 启动装载 errors=E · …`
$pluginTotal = if ($pluginText -match '插件：(\d+)\s*个文件') { [int]$Matches[1] } else { -1 }
$pluginDlls = if ($pluginText -match '（(\d+)\s*个 dll') { [int]$Matches[1] } else { -1 }
$pluginPys = if ($pluginText -match '(\d+)\s*个 py）') { [int]$Matches[1] } else { -1 }
# 兼容旧格式（只有 dll）——**只为报错更清楚**，判据本身要求新格式（下面会 throw）
if ($pluginDlls -lt 0 -and $pluginText -match '插件：(\d+)\s*个\s*dll') { $pluginDlls = [int]$Matches[1] }
$pluginLoaded = if ($pluginText -match 'errors=(\d+)') { [int]$Matches[1] } else { -1 }
$pageText = $pageNote.Line.Trim()
$pageTotal  = if ($pageText -match '可见页集合=(\d+)\s*页') { [int]$Matches[1] } else { -1 }
$pageBuiltin = if ($pageText -match '内置=(\d+)') { [int]$Matches[1] } else { -1 }
$pagePluginPy = if ($pageText -match '插件py=(\d+)') { [int]$Matches[1] } else { -1 }
$pageList = if ($pageText -match '\[(.*?)\]\s*结束') { $Matches[1].Trim() } else { '' }
# 逐页取 id：条目形如 `1=fs=`（内置，尾部带"内置标记"串）或 `1=fs_py`（插件）。
# 口径：**内置页的 id 不带 `_py` 后缀**（插件六枚 replaces 用的 id 都带）⇒ 这样分类与
# `smoke.cpp` 里"按 owner"的分类互相独立，不会一起错。
$pageBuiltinIds = @()
$pagePluginIds = @()
if ($pageList -ne '' -and $pageList -ne '(空)') {
    foreach ($item in ($pageList -split '\s+')) {
        if ($item -notmatch '^\d+=(.+)$') { continue }
        $pageId = $Matches[1].TrimEnd('=')
        if ($pageId -like '*_py') { $pagePluginIds += $pageId } else { $pageBuiltinIds += $pageId }
    }
}
$builtinSlots = @('fs', 'convert', 'text', 'xml', 'video', 'fontpack')   # 与 smoke.cpp 的 kSlots 同序
$fixtureDir = Join-Path $stage 'plugins\_fixtures'
$fixtureFiles = @(Get-ChildItem -LiteralPath $fixtureDir -File -ErrorAction SilentlyContinue)
Write-Host ("  包内 plugins\ 顶层：{0} 个文件（自检态）" -f $pluginTotal)
Write-Host ("  包内 self-check 第 1 行：#8 包内插件：{0}" -f $pluginText)
Write-Host ("  包内 self-check 第 2 行：{0}" -f $pageText)
if ($isDev) {
    # ★ P16-2a：新格式必须解析得出两组计数（旧格式在这里就红 —— 判据跟着口径一起升级）
    if ($pluginTotal -lt 0 -or $pluginPys -lt 0) {
        throw ("包内自检的插件行不是新格式（要同时报 dll 与 py）—— 实测：{0}" -f $pluginText)
    }
    if ($pageTotal -lt 0 -or $pageBuiltin -lt 0) {
        throw ("包内自检的可见页行不是新格式（要给出总页数 / 内置数 / 插件py 数）—— 实测：{0}" -f $pageText)
    }
    # ① 顶层（加载器扫描路径）必须**没有**插件 —— 夹具不是产品，默认不许被加载
    if ($pluginTotal -ne 0 -or $pluginDlls -ne 0 -or $pluginPys -ne 0) {
        throw ("开发版包内 `plugins\` 顶层必须为空（夹具一律落 `plugins\_fixtures\`、默认不加载）—— " +
               "实测自检：{0} 个文件（{1} 个 dll / {2} 个 py）" -f $pluginTotal, $pluginDlls, $pluginPys)
    }
    # ② 可见页＝六个内置页（逐名点名，不许"数目对了就算"）
    if ($pageBuiltin -ne $builtinSlots.Count -or $pagePluginPy -ne 0 -or `
        $pageBuiltinIds.Count -ne $builtinSlots.Count) {
        throw ("开发版包内**可见页必须是六个内置页**（实测：总 {0} 页 / 内置 {1} / 插件py {2} / " +
               "名单 {3}）—— 夹具一旦落进顶层就会被加载，那就又成了两套实现" -f `
               $pageTotal, $pageBuiltin, $pagePluginPy, $pageList)
    }
    for ($i = 0; $i -lt $builtinSlots.Count; ++$i) {
        if ($pageBuiltinIds[$i] -ne $builtinSlots[$i]) {
            throw ("开发版包内第 {0} 个可见页要是内置页 `{1}`，实测 `{2}`（名单：{3}）" -f `
                   ($i + 1), $builtinSlots[$i], $pageBuiltinIds[$i], $pageList)
        }
    }
    # ③ 夹具仍随包携带（枚数与清单一致）—— "随包携带、默认不加载"是两件事，都要成立
    if ($fixtureFiles.Count -ne $copiedPlugins.Count) {
        throw ("`plugins\_fixtures\` 里的夹具枚数与清单不一致（目录 {0} / 清单 {1}）—— " +
               "夹具必须随包携带（默认不加载），少了就是「验得到的东西没打进去」" -f `
               $fixtureFiles.Count, $copiedPlugins.Count)
    }
    foreach ($name in $expectedPages) {
        if (-not (Test-Path (Join-Path $fixtureDir $name))) {
            throw ("开发版包内 `plugins\_fixtures\` 缺随包夹具：{0}（六页插件版 + 三插件同页必须齐）" -f $name)
        }
    }
    if ($pluginLoaded -ne 0) {
        throw ("开发版包内自检**装载有失败**（Hub 启动装载 errors={0}）—— 见包内自检的日志" -f $pluginLoaded)
    }
    Write-Host ("  插件自检通过：顶层 0 个插件（可见页＝六个内置页）、`_fixtures\` 随包携带 {0} 枚夹具、装载 errors=0" -f `
        $fixtureFiles.Count)
} else {
    if ($pluginTotal -ne 0 -or $pluginDlls -ne 0 -or $pluginPys -ne 0) {
        throw ("发行版的 `plugins\` 顶层必须为空（既定策略：NLC 解密插件不随包发布），" +
               "实测自检 {0} 个文件（{1} dll / {2} py）" -f $pluginTotal, $pluginDlls, $pluginPys)
    }
    if ($pageBuiltin -ne $builtinSlots.Count -or $pagePluginPy -ne 0) {
        throw ("发行版包内可见页必须是六个内置页（实测 内置 {0} / 插件py {1}）" -f $pageBuiltin, $pagePluginPy)
    }
    if ($fixtureFiles.Count -ne 0) {
        throw ("发行口径包里不许有夹具（实测 `plugins\_fixtures\` 有 {0} 个文件）" -f $fixtureFiles.Count)
    }
    Write-Host "  插件自检通过：发行版 `plugins\` 顶层与 `_fixtures\` 都为空（可见页＝六个内置页）"
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
 
