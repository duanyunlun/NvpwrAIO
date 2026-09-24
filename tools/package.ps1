# Nvpwr Control —— 打包脚本
#
# 把运行完整链路需要的所有东西组装到一个目录，方便整包分发。
#
# 为什么要一起打包（每一件都是必须的，缺任何一个链路都走不通）：
#
#   NvpwrControl.exe / .dll     主程序
#   EfiDSEFix.exe               DSE 开关。必须是本仓库构建的版本 —— EfiGuard 官方 release
#                               及其附带二进制早于 commit 60a6a57，在新 Windows 上找不到
#                               g_CiOptions，每次调用都返回 STATUS_NOT_FOUND
#   Nvpwr.sys / Nvpwr.cer       内核驱动与其测试证书
#   NvpwrCtl.exe                命令行工具（服务与调试用）
#   NvpwrSvc.exe                开机自动重放设置的服务
#   bootx64.efi / EfiGuardDxe.efi  EfiGuard 引导器，安装到 ESP 用
#   mvolt+.exe                  电压调整（第三方，随包附带，界面里会调用它）

param(
    [string]$OutDir = "",
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$proj     = Join-Path $root 'NvpwrControl_61692_v1_8_0_unified_blackwell_tuner\NvpwrControl_61692_v1_8_0_unified_blackwell_tuner'
$gui      = Join-Path $proj 'gui'
$stage    = 'D:\ProgramFiles\nvpwrcontrol'          # 当前部署目录，非 GUI 组件的来源
$efiFix   = 'D:\Work\Github\EfiGuard\Application\EfiDSEFix\bin\EfiDSEFix.exe'
$efiBoot  = 'D:\Work\Github\_backup\efiguard-theirs'

if (-not $OutDir) { $OutDir = Join-Path $root 'dist\NvpwrControl-1.9.0' }

function Step($m) { Write-Host "  $m" }
function Fail($m) { Write-Host "  ✗ $m" -ForegroundColor Red; exit 1 }

Write-Host "`n=== 1. 构建 WPF 主程序 ===" -ForegroundColor Cyan
if ($SkipBuild) {
    Step "跳过（-SkipBuild）"
} else {
    $env:PATH = "$env:PATH;C:\Program Files\dotnet"
    Push-Location $gui
    $out = & dotnet publish -c $Configuration -o (Join-Path $gui 'publish') 2>&1 | Out-String
    Pop-Location
    $errs = ($out -split "`r?`n") | Where-Object { $_ -match ': error ' }
    if ($errs) { $errs | Select-Object -First 5 | ForEach-Object { Step $_ }; Fail "主程序构建失败" }
    Step "构建成功"
}

Write-Host "`n=== 2. 组装输出目录 ===" -ForegroundColor Cyan
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# --- 主程序 ---
foreach ($f in @('NvpwrControl.exe','NvpwrControl.dll','NvpwrControl.deps.json','NvpwrControl.runtimeconfig.json')) {
    $src = Join-Path $gui "publish\$f"
    if (-not (Test-Path $src)) { Fail "缺少主程序文件: $src" }
    Copy-Item $src $OutDir -Force
}
Step "主程序 4 个文件"

# --- 随包组件 ---
$components = @(
    @{ Path = $efiFix;                    Name = 'EfiDSEFix.exe';  Why = 'DSE 开关（本仓库构建）' },
    @{ Path = "$efiBoot\bootx64.efi";     Name = 'bootx64.efi';    Why = 'EfiGuard 引导器' },
    @{ Path = "$efiBoot\EfiGuardDxe.efi"; Name = 'EfiGuardDxe.efi';Why = 'EfiGuard 驱动' },
    @{ Path = "$stage\Nvpwr.sys";         Name = 'Nvpwr.sys';      Why = '内核驱动' },
    @{ Path = "$stage\Nvpwr.cer";         Name = 'Nvpwr.cer';      Why = '驱动测试证书' },
    @{ Path = "$stage\NvpwrCtl.exe";      Name = 'NvpwrCtl.exe';   Why = '命令行工具' },
    @{ Path = "$stage\NvpwrSvc.exe";      Name = 'NvpwrSvc.exe';   Why = '后台服务' }
)
foreach ($c in $components) {
    if (-not (Test-Path $c.Path)) { Fail "缺少 $($c.Name)（$($c.Why)）: $($c.Path)" }
    Copy-Item $c.Path $OutDir -Force
    Step ("{0,-20} {1}" -f $c.Name, $c.Why)
}

# --- 第三方：mVolt+（电压调整）---
$mvolt = "$stage\mvolt+.exe"
if (Test-Path $mvolt) {
    Copy-Item $mvolt $OutDir -Force
    Step ("{0,-20} {1}" -f 'mvolt+.exe', '电压调整（第三方）')
} else {
    Step "⚠ 未找到 mvolt+.exe —— 电压功能将不可用（功耗功能不受影响）"
}

# --- 说明 ---
$readme = Join-Path $root 'POWER_UNLOCK_FLOW.md'
if (Test-Path $readme) {
    Copy-Item $readme (Join-Path $OutDir '流程说明.md') -Force
    Step "流程说明.md"
}

Write-Host "`n=== 3. 结果 ===" -ForegroundColor Cyan
Get-ChildItem $OutDir -File | Sort-Object Name |
    ForEach-Object { "  {0,10} bytes  {1}" -f $_.Length, $_.Name }
$total = [math]::Round((Get-ChildItem $OutDir -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "`n  共 $((Get-ChildItem $OutDir -File).Count) 个文件，$total MB" -ForegroundColor Green
Write-Host "  输出目录: $OutDir`n" -ForegroundColor Green
