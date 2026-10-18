# scripts/clean.ps1 —— 清**构建残留**（审计 B-6）。
#
# 为什么需要它（修前事实）：`build\logs\` 里堆着几百个批次临时脚本（`_*.py`）、
#   `build\commit_msg*.txt`，`dist\FAIL\` 里堆着每次打包失败的数字目录 —— 它们**都是未跟踪的运行期产物**
#   （`.gitignore` 已经挡住，`git ls-files build dist` 为 0），但会把"这棵树现在是什么状态"读花。
#
# ★ 口径（**先看再删**）：
#   1. **默认只列不删**（`-Apply` 才真删）—— 残留里经常有**正在用的取证材料**
#      （批次报告引用的红证脚本/日志），删之前先把要留的复制到交接区；
#   2. 只清三处**白名单**：`dist\FAIL\`、`build\logs\_*`、`build\commit_msg*.txt`；
#   3. **绝不动** `dist\<主版本>\<标记>\<序号>\`（正式产物）与 `build\msvc-*\`（构建树）。
#
# 用法：
#   pwsh -File scripts\clean.ps1            # 只列（默认）
#   pwsh -File scripts\clean.ps1 -Apply     # 真删（白名单内）
param(
    [switch]$Apply,
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

function Show-Target {
    param([string]$Label, [object[]]$Items)
    $count = @($Items).Count
    $bytes = 0
    foreach ($it in $Items) { $bytes += [int64]$it.Length }
    Write-Host ("  {0,-38} {1,6} 项  {2,10:N0} 字节" -f $Label, $count, $bytes)
    return $count
}

$failDir = Join-Path $Root 'dist\FAIL'
$failItems = @(Get-ChildItem -LiteralPath $failDir -Directory -ErrorAction SilentlyContinue)
$scrapPy = @(Get-ChildItem -LiteralPath (Join-Path $Root 'build\logs') -File -Filter '_*' -ErrorAction SilentlyContinue)
$commitMsg = @(Get-ChildItem -LiteralPath (Join-Path $Root 'build') -File -Filter 'commit_msg*.txt' -ErrorAction SilentlyContinue)

Write-Host "== 构建残留（白名单三处）=="
$n1 = Show-Target -Label 'dist\FAIL\（打包失败件）' -Items $failItems
$n2 = Show-Target -Label 'build\logs\_*（临时脚本/日志）' -Items $scrapPy
$n3 = Show-Target -Label 'build\commit_msg*.txt' -Items $commitMsg

if (-not $Apply) {
    Write-Host ""
    Write-Host "  （默认**只列不删**；要真删加 -Apply）"
    Write-Host "  ★ 删之前先把批次报告要引用的取证材料复制到交接区。"
    exit 0
}

Write-Host ""
Write-Host "== 开始清理（-Apply）=="
foreach ($d in $failItems) {
    Remove-Item -LiteralPath $d.FullName -Recurse -Force
}
foreach ($f in $scrapPy) {
    Remove-Item -LiteralPath $f.FullName -Force
}
foreach ($f in $commitMsg) {
    Remove-Item -LiteralPath $f.FullName -Force
}
Write-Host ("  已删：dist\FAIL {0} 项 / build\logs\_* {1} 项 / commit_msg*.txt {2} 项" -f $n1, $n2, $n3)
Write-Host "  ★ 没有碰 dist\<版本>\… 与 build\msvc-*（正式产物与构建树）"
