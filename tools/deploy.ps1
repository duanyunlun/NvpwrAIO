# Nvpwr Control —— 部署脚本
#
# 目标：把目标目录变成"包的一份精确拷贝"。
#
# 为什么不是"复制过去"就完了：只覆盖不清理，旧文件会一直留着。已经踩过两次 ——
# 一次是陈旧的 pdb，一次是早期测试签名模式的 install-cert-and-enable-testmode.cmd，
# 后者与新方案直接冲突，跑一下就会把系统切到测试签名模式。
#
# 所以顺序是：构建 → 打包 → **清空目标目录** → 整包复制 → 校验两边文件一致。
#
# 分发一致性靠的是最后一步：目标目录里除了包里的东西，不会有任何别的东西。

param(
    [string]$Target = 'D:\ProgramFiles\nvpwrcontrol',
    [string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$NoStart          # 部署完不启动程序（例如只想更新文件）
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist\NvpwrControl-1.9.0'

function Step($m) { Write-Host "  $m" }
function Fail($m) { Write-Host "  ✗ $m" -ForegroundColor Red; exit 1 }

Write-Host "`n=== 1. 停止正在使用目标目录的东西 ===" -ForegroundColor Cyan

foreach ($name in @('NvpwrControl','NvpwrSvc','NvpwrCtl','mvolt+')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
        $_.Kill(); Step "已结束进程 $name (PID $($_.Id))"
    }
}
Start-Sleep -Milliseconds 800

# The driver service holds Nvpwr.sys open, and an installed background service holds
# NvpwrSvc.exe open. Both have to let go before the directory can be emptied.
foreach ($svc in @('Nvpwr','NvpwrSvc')) {
    $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if ($s) {
        Stop-Service $svc -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        Step "已停止服务 $svc"
    }
}
# And unregister the driver service so it does not point at a file being replaced.
if (Get-Service -Name Nvpwr -ErrorAction SilentlyContinue) {
    & sc.exe delete Nvpwr 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
    Step "已注销驱动服务 Nvpwr"
}
Start-Sleep -Seconds 1

Write-Host "`n=== 2. 构建并打包 ===" -ForegroundColor Cyan
# Passed as a literal parameter rather than splatted from an array. Splatting a bare
# '-SkipBuild' string bound it positionally to package.ps1's OutDir, so the build ran anyway
# and the package was written to a directory named "-SkipBuild".
if ($SkipBuild) {
    & (Join-Path $PSScriptRoot 'package.ps1') -SkipBuild | Out-Null
} else {
    & (Join-Path $PSScriptRoot 'package.ps1') | Out-Null
}
if (-not (Test-Path $dist)) { Fail "打包未产出: $dist" }
Step "包已生成: $((Get-ChildItem $dist -File).Count) 个文件"

Write-Host "`n=== 3. 清空目标目录 ===" -ForegroundColor Cyan
if (-not (Test-Path $Target)) {
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    Step "已创建 $Target"
} else {
    $victims = Get-ChildItem $Target -Force
    foreach ($v in $victims) {
        try {
            Remove-Item $v.FullName -Recurse -Force -ErrorAction Stop
        } catch {
            Fail "无法删除 $($v.Name)：$($_.Exception.Message)`n" +
                 "        多半是还有进程占用它。关掉程序、解除服务和驱动后重试。"
        }
    }
    Step "已清空（删除 $($victims.Count) 项）"
}

Write-Host "`n=== 4. 复制包内容 ===" -ForegroundColor Cyan
foreach ($f in Get-ChildItem $dist -File) {
    Copy-Item $f.FullName $Target -Force
}
Step "已复制 $((Get-ChildItem $dist -File).Count) 个文件"

Write-Host "`n=== 5. 校验 ===" -ForegroundColor Cyan
$src = Get-ChildItem $dist  -File | Sort-Object Name
$dst = Get-ChildItem $Target -File | Sort-Object Name

$missing = $src | Where-Object { $_.Name -notin $dst.Name }
$extra   = $dst | Where-Object { $_.Name -notin $src.Name }
$differs = @()
foreach ($f in $src) {
    $t = $dst | Where-Object { $_.Name -eq $f.Name }
    if ($t -and ($t.Length -ne $f.Length -or (Get-FileHash $f.FullName).Hash -ne (Get-FileHash $t.FullName).Hash)) {
        $differs += $f.Name
    }
}

if ($missing) { Fail "缺少: " + ($missing.Name -join ', ') }
if ($extra)   { Fail "多出: " + ($extra.Name -join ', ') }
if ($differs) { Fail "内容不一致: " + ($differs -join ', ') }

Step "文件清单与哈希全部一致 —— 目标目录是包的一份精确拷贝 ✅"
Step "$($src.Count) 个文件，$([math]::Round(($src | Measure-Object Length -Sum).Sum / 1MB, 1)) MB"

Write-Host "`n=== 6. 启动 ===" -ForegroundColor Cyan
if ($NoStart) {
    Step "按 -NoStart 跳过"
} else {
    $exe = Join-Path $Target 'NvpwrControl.exe'
    if (-not (Test-Path $exe)) { Fail "找不到 $exe" }
    Start-Process -FilePath $exe | Out-Null
    Step "已启动 $exe"
}

Write-Host "`n  部署完成: $Target`n" -ForegroundColor Green
