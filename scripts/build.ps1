#requires -Version 5.1
<#
  2.x 构建入口。

  三条纪律都是本机实测踩出来的，别改回去：
   ① 必须注入 MSVC 环境 —— `cl.exe` 不在 PATH 里；
   ② 环境用 `cmd /c "call vcvars64.bat >nul && set > env.txt"` **导出成文件**再注入。
      不要在批处理里对同一个日志文件反复 `>>` 追加 —— 实测会报"文件被占用"而失败；
   ③ 必须在**全权文件模式**下跑：workspace-write 沙箱会让 cmake→ninja→cl→link
      这条深层进程链死锁（CMake 永久卡在 "Detecting CXX compiler ABI info"）。

  用法：pwsh -File scripts/build.ps1 [-Preset msvc-release|msvc-shell] [-Reconfigure]
#>
param(
    [string]$Preset = 'msvc-release',
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $root 'build'
$envFile   = Join-Path $buildRoot 'msvc_env.txt'

$vs     = 'E:\Software\Tools\Visual Studio\2022\BuildTools'
$qt     = 'E:\Software\Tools\Qt\6.8.3\msvc2022_64'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

if ($Reconfigure -or -not (Test-Path $envFile)) {
    Remove-Item $envFile -Force -ErrorAction SilentlyContinue
    & cmd.exe /c ('call "{0}" >nul 2>&1 && set > "{1}"' -f $vcvars, $envFile)
    if (-not (Test-Path $envFile)) { throw "vcvars 环境导出失败：$vcvars" }
}

Get-Content -LiteralPath $envFile | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:Path = "$cmakeBin;$ninjaBin;$qt\bin;$env:Path"

Push-Location $root
try {
    Write-Host "== configure ($Preset) =="
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure 失败（退出码 $LASTEXITCODE）" }

    Write-Host "== build ($Preset) =="
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build 失败（退出码 $LASTEXITCODE）" }
} finally {
    Pop-Location
}

Write-Host "BUILD OK: $Preset"
