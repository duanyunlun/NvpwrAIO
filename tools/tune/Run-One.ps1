# ============================================================================
#  Run-One.ps1 —— 应用设置 → 点「再次运行」→ 等结果 → 解析 → 记 CSV
#
#  前置：3DMark 已停在【结果页】（有「再次运行」按钮）
#  绝不触碰 NVVDD / MSVDD 的【最大电压】：REL / ALT / OV 恒为 0。
# ============================================================================

param(
    [int]$PowerW = 0,
    [int]$CoreMhz = 0,
    [int]$MemMhz = 0,
    [int]$XbarMhz = 0,
    [int]$NvvddVmin = 0,
    [int]$MsvddVmin = 0,
    [string]$Note = '',
    [int]$TimeoutSec = 600
)

$ErrorActionPreference = 'Stop'
$ROOT   = 'D:\ProgramFiles\nvpwrcontrol'
$CTL    = Join-Path $ROOT 'NvpwrCtl.exe'
$MVOLT  = Join-Path $ROOT 'mvolt+.exe'
$RESDIR = "$env:USERPROFILE\Documents\3DMark"
$CSVDIR = 'D:\Work\Github\rtx-5070ti-laptop-160w-power-limit\dist\tuning'
$CSV    = Join-Path $CSVDIR 'timespy-sweep.csv'
New-Item -ItemType Directory -Path $CSVDIR -Force | Out-Null

# 断电窗口守卫
$now = Get-Date
foreach ($t in @('08:15','13:40','18:40')) {
    $w = [datetime]::Parse($now.ToString('yyyy-MM-dd') + ' ' + $t)
    $d = ($w - $now).TotalMinutes
    if ($d -le 15 -and $d -ge -5) { Write-Host "停工：距断电点 $t 还有 $([Math]::Round($d,1)) 分钟"; exit 2 }
}

Write-Host "=== 本轮: ${PowerW}W  core=$CoreMhz mem=$MemMhz xbar=$XbarMhz  NVVDDvmin=$NvvddVmin MSVDDvmin=$MsvddVmin ==="

# ---- 1. 应用设置 ----
# 功耗走 Set-Power.ps1 直连 IOCTL，不用 NvpwrCtl：后者的命令行把 5080/5090 写死在
# 175..225 W，超过就拒绝，而驱动本身的上限是 350 W。
if ($PowerW -gt 0) {
    $sp = Join-Path $PSScriptRoot 'Set-Power.ps1'
    $o = & $sp -Watts $PowerW 2>&1 | Out-String
    $o.Trim() -split "`r?`n" | Where-Object { $_.Trim() } | ForEach-Object { Write-Host "  $_" }
}
$args = @()
if ($CoreMhz -ne 0) { $args += '--core'; $args += "$CoreMhz" }
if ($MemMhz  -ne 0) { $args += '--mem';  $args += "$MemMhz" }
if ($XbarMhz -ne 0) { $args += '--xbar-offset'; $args += "$XbarMhz" }
if ($NvvddVmin -ne 0) { $args += '--nvvdd-offsets'; $args += "$NvvddVmin,0,0,0" }
if ($MsvddVmin -ne 0) { $args += '--msvdd-offsets'; $args += "$MsvddVmin,0,0,0" }
if ($args.Count -gt 0) {
    $o2 = & $MVOLT @args 2>&1 | Out-String
    $s = & $MVOLT --status 2>&1 | Out-String
    Write-Host ("  回读 core={0} mem={1} xbar={2}" -f `
        ([regex]::Match($s,'"core_offset_mhz":(-?\d+)').Groups[1].Value), `
        ([regex]::Match($s,'"memory_offset_mhz":(-?\d+)').Groups[1].Value), `
        ([regex]::Match($s,'"xbar_offset_mhz":(-?\d+)').Groups[1].Value))
    Write-Host ("  回读 nvvdd={0}" -f ([regex]::Match($s,'"nvvdd_offsets_uv":\{[^}]*\}').Value))
    Write-Host ("  回读 msvdd={0}" -f ([regex]::Match($s,'"msvdd_offsets_uv":\{[^}]*\}').Value))
    # 确认最大电压没被碰
    if ($s -notmatch '"nvvdd_max_mv":1025' -or $s -notmatch '"msvdd_max_mv":1025') {
        Write-Host "  ⚠ 最大电压不是 1025！立刻中止" -ForegroundColor Red; exit 4
    }
}

# ---- 2. 找「再次运行」并点击 ----
Add-Type -Namespace R -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
public const uint LEFTDOWN=0x0002, LEFTUP=0x0004;
public static void Click(int x, int y) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
}
'@
Add-Type -AssemblyName UIAutomationClient -EA SilentlyContinue
Add-Type -AssemblyName UIAutomationTypes -EA SilentlyContinue

$p = Get-Process -Name 3DMark -EA SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Host "  ❌ 3DMark 不在运行"; exit 3 }
[void][R.U]::ShowWindow($p.MainWindowHandle, 3)
[void][R.U]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Seconds 2

$root = [System.Windows.Automation.AutomationElement]::RootElement
$pc = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $pc)
$all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$btn = $null
foreach ($e in $all) {
    if ($e.Current.Name -match '^\s*再次运行\s*$' -and $e.Current.ControlType.ProgrammaticName -eq 'ControlType.Hyperlink') {
        $bb = $e.Current.BoundingRectangle
        $btn = @{ X = [int](($bb.X + $bb.Width/2) * 2); Y = [int](($bb.Y + $bb.Height/2) * 2) }
        break
    }
}
if (-not $btn) { Write-Host "  ❌ 找不到「再次运行」（当前不在结果页？）"; exit 5 }
Write-Host ("  点击「再次运行」物理({0},{1})" -f $btn.X, $btn.Y)

$t0 = Get-Date
[R.U]::Click($btn.X, $btn.Y)

# ---- 3. 等结果 ----
$result = $null
for ($i = 0; $i -lt ($TimeoutSec / 5); $i++) {
    Start-Sleep -Seconds 5
    $f = Get-ChildItem $RESDIR -Filter '*.3dmark-result' -EA SilentlyContinue |
         Where-Object { $_.LastWriteTime -ge $t0 } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) { $result = $f; break }
    if ($i % 12 -eq 11) { Write-Host ("    [{0}s] 还在跑…" -f ($i*5+5)) }
    if (-not (Get-Process -Name 3DMark -EA SilentlyContinue)) { Write-Host "  ⚠ 3DMark 进程消失"; break }
}
if (-not $result) { Write-Host "  ❌ 超时无结果"; exit 6 }

$elapsed = [int]((Get-Date) - $t0).TotalSeconds
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::OpenRead($result.FullName)
$e = $z.Entries | Where-Object { $_.FullName -eq 'Result.xml' }
$sr = New-Object System.IO.StreamReader($e.Open()); $xml = $sr.ReadToEnd(); $sr.Close()
$z.Dispose()

$score = [regex]::Match($xml,'<TimeSpyPerformanceGraphicsScore>(\d+)</TimeSpyPerformanceGraphicsScore>').Groups[1].Value
$overall = [regex]::Match($xml,'<TimeSpyPerformance3DMarkScore>(\d+)</TimeSpyPerformance3DMarkScore>').Groups[1].Value
$cpu = [regex]::Match($xml,'<TimeSpyPerformanceCPUScore>(\d+)</TimeSpyPerformanceCPUScore>').Groups[1].Value
$gt1 = [regex]::Match($xml,'<TimeSpyPerformanceGraphicsTest1>([\d\.]+)</TimeSpyPerformanceGraphicsTest1>').Groups[1].Value
$gt2 = [regex]::Match($xml,'<TimeSpyPerformanceGraphicsTest2>([\d\.]+)</TimeSpyPerformanceGraphicsTest2>').Groups[1].Value
$failed = $result.Name -match 'FAILED'

Write-Host ("  ✅ 图形 {0}  总分 {1}  CPU {2}  GT1 {3}  GT2 {4}   用时 {5}s" -f $score,$overall,$cpu,$gt1,$gt2,$elapsed)

# ---- 4. 记 CSV ----
if (-not (Test-Path $CSV)) {
    '"序号","时间","功耗设定","核心偏移","显存偏移","XBAR偏移","NVVDD最小","MSVDD最小","图形分数","总分","CPU分数","GT1","GT2","备注","结果文件"' |
        Set-Content -Path $CSV -Encoding UTF8
}
$n = (Get-Content $CSV -Encoding UTF8 | Measure-Object -Line).Lines
$row = '"{0}","{1}","{2}","{3}","{4}","{5}","{6}","{7}","{8}","{9}","{10}","{11}","{12}","{13}","{14}"' -f `
    $n, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $PowerW, $CoreMhz, $MemMhz, $XbarMhz, $NvvddVmin, $MsvddVmin, `
    $(if ($failed) { 'FAILED' } else { $score }), $overall, $cpu, $gt1, $gt2, $Note, $result.Name
$row | Add-Content -Path $CSV -Encoding UTF8
Write-Host "  已记入 CSV 第 $n 行"
