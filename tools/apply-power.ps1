# 功耗墙下发链路 —— 原型脚本
#
# 这是要移植进 C# GUI 的逻辑原型。核心不变量：
#   1. DSE 一旦关闭，必须在 finally 里恢复，无论中间发生什么
#   2. 驱动一旦加载，必须在 finally 里卸载
#   3. EfiGuard 未生效时直接放弃，不做任何修改（fail-safe）

param(
    [Parameter(Mandatory = $true)][int]$Watts,
    [string]$Profile = '5090',
    [string]$Stage   = 'D:\ProgramFiles\nvpwrcontrol',
    [string]$DseFix  = 'D:\Work\Github\EfiGuard\Application\EfiDSEFix\bin\EfiDSEFix.exe'
)

$ErrorActionPreference = 'Stop'
$svc    = 'Nvpwr'
$sys    = Join-Path $Stage 'Nvpwr.sys'
$ctl    = Join-Path $Stage 'NvpwrCtl.exe'

function Write-Step($msg) { Write-Host "  $msg" }

# ---- 前置检查 -------------------------------------------------------------

if (-not (Test-Path $DseFix)) { throw "找不到 EfiDSEFix.exe: $DseFix" }
if (-not (Test-Path $sys))    { throw "找不到驱动: $sys" }
if (-not (Test-Path $ctl))    { throw "找不到 CLI: $ctl" }

Write-Host "[1/6] 检查 EfiGuard 是否生效"
$null = & $DseFix -c 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Step "EfiGuard 未生效（-c 退出码 $LASTEXITCODE）"
    Write-Step "本次未经 EfiGuard 引导，放弃修改，功耗墙保持出厂值"
    exit 2
}
Write-Step "EfiGuard hook 可用"

# ---- 主链路（带 finally 保护）--------------------------------------------

$dseDisabled = $false
$driverUp    = $false

try {
    Write-Host "[2/6] 关闭 DSE"
    $null = & $DseFix -d 2>&1
    if ($LASTEXITCODE -ne 0) { throw "EfiDSEFix -d 失败，退出码 $LASTEXITCODE" }
    $dseDisabled = $true
    Write-Step "DSE 已关闭"

    Write-Host "[3/6] 加载驱动"
    & sc.exe stop   $svc 2>&1 | Out-Null
    & sc.exe delete $svc 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
    & sc.exe create $svc type= kernel binPath= $sys 2>&1 | Out-Null
    $startOut = & sc.exe start $svc 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "sc start 失败:`n$startOut" }
    $driverUp = $true
    Write-Step "驱动已加载"

    Write-Host "[4/6] 下发功耗 $Watts W（型号 $Profile）"
    $out = & $ctl set $Profile $Watts 2>&1 | Out-String
    $state = ($out -split "`r?`n" | Where-Object { $_ -match '^\s*State\s*:' }) -replace '.*:\s*',''
    $nt    = ($out -split "`r?`n" | Where-Object { $_ -match 'Last NTSTATUS' }) -replace '.*:\s*',''
    $nv    = ($out -split "`r?`n" | Where-Object { $_ -match 'Last NVIDIA status' }) -replace '.*:\s*',''
    Write-Step "State=$($state.Trim())  NTSTATUS=$($nt.Trim())  NVIDIA=$($nv.Trim())"
    if ($state -notmatch 'APPLIED') { throw "下发未生效，State=$state" }
}
finally {
    Write-Host "[5/6] 恢复 DSE"
    if ($dseDisabled) {
        $null = & $DseFix -e 2>&1
        if ($LASTEXITCODE -eq 0) { Write-Step "DSE 已恢复" }
        else { Write-Step "⚠ DSE 恢复失败（退出码 $LASTEXITCODE）—— 需要重启！" }
    } else {
        Write-Step "DSE 未曾关闭，跳过"
    }

    Write-Host "[6/6] 卸载驱动"
    if ($driverUp) {
        & sc.exe stop $svc 2>&1 | Out-Null
        Write-Step "驱动已卸载"
    } else {
        Write-Step "驱动未加载，跳过"
    }
}

# ---- 结果验证 -------------------------------------------------------------

Write-Host ""
Write-Host "结果验证:"
$limit = (& nvidia-smi --query-gpu=power.max_limit --format=csv,noheader 2>&1).Trim()
Write-Host "  nvidia-smi max_limit = $limit"
if ($limit -match [regex]::Escape("$Watts.00")) { Write-Host "  ✅ 下发成功且保持"; exit 0 }
else { Write-Host "  ⚠ 值与请求不符"; exit 1 }
