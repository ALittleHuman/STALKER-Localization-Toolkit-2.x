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
    [string]$VersionString = ''
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

Write-Host "== windeployqt（铺 Qt 运行时；不铺翻译/示例）=="
& (Join-Path $qt 'bin\windeployqt.exe') --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw (Join-Path $stage 'xrkit_app.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败（退出码 $LASTEXITCODE）" }

Write-Host "== README / version =="
$commit = (& git -C $root rev-parse --short HEAD)
$stamp  = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
@"
STALKER 汉化工具集 2.0.0（xrkit）
--------------------------------
安装：整个目录解压到任意位置，双击 xrkit_app.exe。
回退：删除该目录即可（本工具**不改动**游戏目录里的任何文件；
      唯一的写盘位置是用户设置 user.ltx，删除它即恢复默认）。
插件：plugins\ 为空目录。需要插件时把 <name>.dll 放进去，重启程序；
      插件加载失败不会阻塞启动，问题会显示在启动后的日志栏里。
不含：ffmpeg（视频页要你自己在页面里手动指定）、NLC 解密插件、实验性引擎补丁。
自检：命令行跑 `xrkit_app.exe --smoke`（离屏，ASCII 输出）可核对包是否完整。
"@ | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8
$verLine = if ($VersionString -ne '') { $VersionString } else { '2.0.0' }
@"
version=$verLine
index=$BuildIndex
commit=$commit
built=$stamp
"@ | Set-Content -LiteralPath (Join-Path $stage 'version.txt') -Encoding UTF8

Write-Host "== 打包判据：从包目录跑一次离屏自检 =="
# `windeployqt` 不会铺 offscreen 平台插件（它按 app 的实际调用挑），而我们的自检是离屏的。
# 处置：**临时**拷进去跑判据，跑完删掉 —— 发行包里仍然不带它（保持包精简）。
$off = Join-Path $qt 'plugins\platforms\qoffscreen.dll'
$stagePlat = Join-Path $stage 'platforms'
$copiedOff = $false
if (Test-Path $off) {
    New-Item -ItemType Directory -Force -Path $stagePlat | Out-Null
    Copy-Item -LiteralPath $off -Destination $stagePlat
    $copiedOff = $true
}
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
        $proc = Start-Process -FilePath (Join-Path $stage 'xrkit_app.exe') -ArgumentList '--smoke' `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError $errLog
        $line = Select-String -Path $log -Pattern 'SHELL-SMOKE ok' | Select-Object -First 1
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
    $env:QT_QPA_PLATFORM = ''
    if ($copiedOff) {
        Remove-Item -Force (Join-Path $stagePlat 'qoffscreen.dll') -ErrorAction SilentlyContinue
    }
}
if (-not $smokeOk) { throw "包内 exe 的离屏自检没通过（包可能缺 Qt 运行时）" }

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
 
