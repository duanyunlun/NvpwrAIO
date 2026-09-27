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
#
# 外部依赖：最后三样不是本仓库的产物，需要你自己准备。
# 它们原本写成本机绝对路径，别人 clone 之后那些路径不存在，脚本会在拷贝阶段
# 失败，而失败信息指向一个他们从没听说过的目录，所以改成了参数。
#
#   -Stage   已经部署好的目录。非 GUI 组件（.sys / .cer / EFI 引导器）从这里取，
#            而不是从构建输出取 —— 那些文件不是每次构建都会重新生成，构建输出
#            里的可能比实际部署的旧。
#   -EfiFix  EfiDSEFix.exe。★ 必须是从 EfiGuard 源码自行构建的版本，官方 release
#            及其附带二进制早于 commit 60a6a57，在新 Windows 上找不到 g_CiOptions，
#            每次调用都返回 STATUS_NOT_FOUND。
#   -EfiBoot EfiGuard 的引导器目录，需含 bootx64.efi 与 EfiGuardDxe.efi。
#            来源：https://github.com/Mattiwatti/EfiGuard （自行构建）

param(
    [string]$OutDir = "",
    [string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [string]$Stage   = 'D:\ProgramFiles\nvpwrcontrol',
    [string]$EfiFix  = 'D:\Work\Github\EfiGuard\Application\EfiDSEFix\bin\EfiDSEFix.exe',
    [string]$EfiBoot = 'D:\Work\Github\_backup\efiguard-theirs'
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$proj     = Join-Path $root 'src'
$gui      = Join-Path $proj 'gui'

$stage    = $Stage
$efiFix   = $EfiFix
$efiBoot  = $EfiBoot

if (-not $OutDir) { $OutDir = Join-Path $root 'release' }

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

# --- 主程序：publish 目录的全部内容 ---
#
# Copied wholesale rather than as a list of four named files. The list missed the NuGet
# dependencies the app needs at startup (System.ServiceProcess.ServiceController and the
# EventLog messages assembly), which produced a package that installed cleanly and then failed
# to launch. Whatever publish emits is what the app needs.
$publish = Join-Path $gui 'publish'
if (-not (Test-Path $publish)) { Fail "缺少 publish 目录，先构建: $publish" }
$mainCount = 0
foreach ($f in Get-ChildItem $publish -File) {
    Copy-Item $f.FullName $OutDir -Force
    $mainCount++
}
Step "主程序 $mainCount 个文件（publish 全部内容）"

# --- 随包组件 ---
$components = @(
    @{ Path = $efiFix;                    Name = 'EfiDSEFix.exe';  Why = 'DSE 开关（本仓库构建）' },
    @{ Path = "$efiBoot\bootx64.efi";     Name = 'bootx64.efi';    Why = 'EfiGuard 引导器' },
    @{ Path = "$efiBoot\EfiGuardDxe.efi"; Name = 'EfiGuardDxe.efi';Why = 'EfiGuard 驱动' },
    # Nvpwr.sys 和它的证书取【构建产物】，不是部署目录里那份。
    #
    # 这两个原先归在"外部产物"里，从 -Stage 取 —— 但那是错的：build.ps1 每次都会
    # 重新编译并签名它们，所以部署目录里那份必然是【上一次】的。后果就是"改了驱动
    # 源码 → 重新打包 → 部署"，报告一路成功，装上去的却是旧驱动，而这一点只能靠比对
    # 文件大小或哈希才发现。实测踩到过一次：源码里 31,632 B 的新驱动，部署下去的是
    # 28,048 B 的旧驱动。
    #
    # 真正的外部产物只有上面那三件（EfiGuard 的引导器与 DSE 开关），它们不由本仓库构建。
    @{ Path = (Join-Path $proj 'driver\x64\Release\Nvpwr.sys'); Name = 'Nvpwr.sys'; Why = '内核驱动（本仓库构建）' },
    @{ Path = (Join-Path $proj 'driver\x64\Release\Nvpwr.cer'); Name = 'Nvpwr.cer'; Why = '驱动测试证书（本仓库构建）' },
    # NvpwrCtl 取本仓库刚构建出来的那份，不是部署目录里那份。
    #
    # 部署目录之所以是"非 GUI 组件的来源"，是因为 EfiDSEFix / bootx64 / Nvpwr.sys 这些
    # 属于外部产物，本仓库只负责打包。但 NvpwrCtl 是本仓库自己构建的，混在一起取会让
    # "改源码 → 重新打包" 静默地用上旧二进制 —— 打包报告成功，内容却没变。
    @{ Path = (Join-Path $proj 'cli\x64\Release\NvpwrCtl.exe'); Name = 'NvpwrCtl.exe'; Why = '命令行工具（本仓库构建）' },
    # 同理，服务也是本仓库构建的，取构建产物而不是部署目录里那份。
    @{ Path = (Join-Path $proj 'service\x64\Release\NvpwrSvc.exe'); Name = 'NvpwrSvc.exe'; Why = '后台服务（本仓库构建）' }
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
#
# Two documents, for two readers. 使用说明 is the one that ships to whoever receives the
# package — prerequisites, install steps, and the things that bite (Defender deleting the
# loader, VBS and EfiGuard being mutually exclusive). 流程说明 is the implementation record
# and is only useful to someone working on the code.
$manual = Join-Path $root 'docs\使用说明.txt'
if (Test-Path $manual) {
    Copy-Item $manual $OutDir -Force
    Step "使用说明.txt"
} else {
    Step "⚠ 未找到 docs\使用说明.txt —— 发布包将没有使用说明"
}

# 这两个文档现在都在 docs\ 下。之前 流程说明 的源文件在仓库根目录，搬进 docs 之后
# 这一行没跟着改，于是 Test-Path 失败、拷贝被静默跳过 —— 发布包少了一个文件，而
# 脚本的输出里只有一句"流程说明.md"没出现，很容易看漏。改成找不到就明确报错。
$readme = Join-Path $root 'docs\POWER_UNLOCK_FLOW.md'
if (Test-Path $readme) {
    Copy-Item $readme (Join-Path $OutDir '流程说明.md') -Force
    Step "流程说明.md"
} else {
    Step "⚠ 未找到 docs\POWER_UNLOCK_FLOW.md —— 发布包将没有流程说明"
}

Write-Host "`n=== 3. 结果 ===" -ForegroundColor Cyan
Get-ChildItem $OutDir -File | Sort-Object Name |
    ForEach-Object { "  {0,10} bytes  {1}" -f $_.Length, $_.Name }
$total = [math]::Round((Get-ChildItem $OutDir -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "`n  共 $((Get-ChildItem $OutDir -File).Count) 个文件，$total MB" -ForegroundColor Green
Write-Host "  输出目录: $OutDir`n" -ForegroundColor Green
