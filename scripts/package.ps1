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
    try {
        & (Join-Path $stage 'xrkit_app.exe') --smoke *> $log
        $line = Select-String -Path $log -Pattern 'SHELL-SMOKE ok' | Select-Object -First 1
        if ($line) {
            Write-Host "  $($line.Line.Trim())"
            $smokeOk = $true
        } else {
            Get-Content -LiteralPath $log -Tail 10 | ForEach-Object { Write-Host $_ }
        }
    } finally {
        $ErrorActionPreference = $prevEap
        Remove-Item -Force $log -ErrorAction SilentlyContinue
    }
} finally {
    $env:QT_QPA_PLATFORM = ''
    if ($copiedOff) {
        Remove-Item -Force (Join-Path $stagePlat 'qoffscreen.dll') -ErrorAction SilentlyContinue
    }
}
if (-not $smokeOk) { throw "包内 exe 的离屏自检没通过（包可能缺 Qt 运行时）" }

Write-Host "== 压缩 =="
$dist = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$tag  = if ($VersionString -ne '') { ($VersionString -replace ' ', '_') } else { '2.0.0' }
$zip  = Join-Path $dist ("STALKER_Toolkit_{0}_{1}.zip" -f $tag, $commit)
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "PACKAGE OK: $zip"
