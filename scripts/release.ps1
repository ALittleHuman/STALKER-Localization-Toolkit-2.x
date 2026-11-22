# 2.0 发包驱动（2026-11-06）
#
# 为什么需要它：`--build-package` 是 CLI 自己在跑；如果在它里面 bump 版本再重编 CLI，
# 会撞"正在运行的 exe 被占用"（LNK1104）。所以把 **bump 与打包分到两个进程**：
#
#   1) --write-version     改 version.h / .rc（不动 version.json 的 current）
#   2) build.ps1 两档       此时没有任何进程占着 xrkit_cli.exe
#   3) --build-package --prebuilt  只算 sha + 打包 + 收尾
#   4) 校验：包内 app / cli / version.txt 的版本号一致
#
# 用法：pwsh -File scripts/release.ps1 -Index 127 [-Version 2.0.0] [-Mark BETA]
# 注意：本脚本只构建/打包/校验，不发布；BETA/RC 上传 GitHub 时必须设为 Prerelease，正式版才用 Release。
param(
    [Parameter(Mandatory = $true)][int]$Index,
    [string]$Version = '2.0.0',
    [string]$Mark = 'BETA'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cli = Join-Path $root 'build\msvc-release\tools\cli\xrkit_cli.exe'

Write-Host "== 1/4 写版本号（version.h / .rc）=="
& $cli --write-version $Version $Mark $Index
if ($LASTEXITCODE -ne 0) { throw "write-version 失败" }

Write-Host "== 2/4 编译两档（app + cli）=="
& (Join-Path $root 'scripts\build.ps1') -Preset msvc-shell
if ($LASTEXITCODE -ne 0) { throw "msvc-shell 编译失败" }
& (Join-Path $root 'scripts\build.ps1') -Preset msvc-release
if ($LASTEXITCODE -ne 0) { throw "msvc-release 编译失败" }

Write-Host "== 3/4 打包（--prebuilt）=="
& $cli --build-package version.json $Version $Mark $Index --prebuilt
if ($LASTEXITCODE -ne 0) { throw "build-package 失败" }

Write-Host "== 4/4 校验包内身份 =="
$pkg = Join-Path $root ("dist\{0}\{1}\{2}" -f $Version, $Mark, $Index)
$vt = Get-Content (Join-Path $pkg 'version.txt') -ErrorAction Stop
$idxLine = ($vt | Where-Object { $_ -like 'index=*' })
$wantIdx = ($idxLine -split '=')[1]
$buildLine = ($vt | Where-Object { $_ -like 'build=*' })
$wantBuild = if ($buildLine) { ($buildLine -split '=')[1] } else { '' }
if (-not $wantBuild) {
    $vLine = ($vt | Where-Object { $_ -like 'version=*' })
    if ($vLine -match '^version=.*\.(\d+)$') { $wantBuild = $Matches[1] }
}
if (-not $wantBuild) { throw "version.txt 缺 build= / 版本串，无法核对包内身份" }
$cliOut = & (Join-Path $pkg 'xrkit_cli.exe') --version
$appOut = (& $cli --exe-version (Join-Path $pkg 'xrkit_app.exe'))
if ($cliOut -notmatch ("build=" + $wantBuild)) { throw "包内 CLI build 与 version.txt 不一致：$cliOut vs build=$wantBuild" }
if ($appOut -notmatch ("numeric=[0-9]+\.[0-9]+\.[0-9]+\." + $wantBuild)) { throw "包内 app 版本与 version.txt 不一致：$appOut" }
Write-Host "RELEASE ok index=$wantIdx build=$wantBuild cli=$cliOut"
