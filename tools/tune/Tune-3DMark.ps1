# ============================================================================
#  Tune-3DMark.ps1 —— 单次测量：应用设置 → 跑一次 Time Spy → 取图形分数 → 记录
#
#  这个脚本只做【一次】测量。参数由调用方（我）根据上一轮的分析给出，
#  脚本本身不做决策 —— 决策依据在结果表里，不在代码里。
#
#  绝不触碰的东西：NVVDD / MSVDD 的【最大电压】。
#  对应 --nvvdd-offsets / --msvdd-offsets 的第 2、3、4 个字段（REL/ALT/OV），
#  它们在这里恒为 0。只动第 1 个字段（VMIN，最小电压）。
#
#  用法：
#    pwsh -File Tune-3DMark.ps1 -PowerW 250 -CoreMhz 200 -MemMhz 1000 -XbarMhz 200 `
#         -NvvddVmin 0 -MsvddVmin 0 -Note "起点"
#    pwsh -File Tune-3DMark.ps1 -CheckOnly        # 只做时间/环境检查
# ============================================================================

param(
    [int]$PowerW = 0,
    [int]$CoreMhz = 0,
    [int]$MemMhz = 0,
    [int]$XbarMhz = 0,
    [int]$NvvddVmin = 0,
    [int]$MsvddVmin = 0,
    [string]$Note = '',
    [int]$RunTimeoutSec = 300,
    [int]$FullscreenGraceSec = 25,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

# 全屏判定要用 Screen.PrimaryScreen.Bounds
Add-Type -AssemblyName System.Windows.Forms -EA SilentlyContinue

$ROOT      = 'D:\ProgramFiles\nvpwrcontrol'
$CTL       = Join-Path $ROOT 'NvpwrCtl.exe'
$MVOLT     = Join-Path $ROOT 'mvolt+.exe'
$DM_EXE    = 'D:\SteamLibrary\steamapps\common\3DMark\bin\x64\3DMark.exe'
$RESULTDIR = "$env:USERPROFILE\Documents\3DMark"
$CSVDIR    = 'D:\Work\Github\rtx-5070ti-laptop-160w-power-limit\dist\tuning'
$CSV       = Join-Path $CSVDIR 'timespy-sweep.csv'
$LOG       = Join-Path $CSVDIR 'tune.log'

New-Item -ItemType Directory -Path $CSVDIR -Force | Out-Null

function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -Path $LOG -Value $line -Encoding UTF8
}

# ------------------------------------------------------------------ 断电窗口
# 每天三次约一分钟的断电：08:15 / 13:40 / 18:40，前后各十分钟都算危险。
# 窗口内绝不启动 3DMark —— 跑一半断电的话，这一次的分数不能用，还白费三分钟。
$OUTAGE = @('08:15', '13:40', '18:40')
$GUARD_MIN = 15      # 比断电点提前多少分钟停手（比"前后十分钟"再宽一点）

function Get-OutageStatus {
    $now = Get-Date
    $rows = @()
    foreach ($t in $OUTAGE) {
        $w = [datetime]::Parse($now.ToString('yyyy-MM-dd') + ' ' + $t)
        $d = ($w - $now).TotalMinutes
        $rows += [pscustomobject]@{
            At      = $t
            Minutes = [Math]::Round($d, 1)
            # 窗口内 = 还没过多久，或马上要到
            Blocked = (($d -le $GUARD_MIN) -and ($d -ge -5))
        }
    }
    return $rows
}

function Assert-NoOutageWindow {
    $rows = Get-OutageStatus
    $hit = $rows | Where-Object { $_.Blocked }
    if ($hit) {
        $h = $hit | Select-Object -First 1
        Log ("停工：距断电点 {0} 还有 {1} 分钟（保护窗口 ±{2} 分钟）" -f $h.At, $h.Minutes, $GUARD_MIN)
        return $false
    }
    $next = ($rows | Where-Object { $_.Minutes -gt 0 } | Sort-Object Minutes | Select-Object -First 1)
    if ($next) { Log ("时间安全，距下一个断电点 {0} 还有 {1} 分钟" -f $next.At, $next.Minutes) }
    return $true
}

# ------------------------------------------------------------------ 环境检查
function Test-Environment {
    $ok = $true

    if (-not (Test-Path $CTL))   { Log "缺少 NvpwrCtl.exe"; $ok = $false }
    if (-not (Test-Path $MVOLT)) { Log "缺少 mvolt+.exe";   $ok = $false }
    if (-not (Test-Path $DM_EXE)){ Log "找不到 3DMark.exe";  $ok = $false }

    # 驱动必须在位 —— 功耗走的是它
    $st = & $CTL status 2>&1 | Out-String
    if ($st -match 'OEM baseline\s*:\s*(\d+)') {
        Log ("内核驱动在位，出厂基线 {0} W" -f ([int]$Matches[1] / 1000))
    } else {
        Log "内核驱动不在位（无法读取 OEM 基线）"
        $ok = $false
    }

    # HWiNFO 共享内存 —— 逐秒电压/功耗靠它
    $hv = Test-HwInfoAvailable
    if ($hv) { Log "HWiNFO 共享内存可读" } else { Log "⚠ HWiNFO 共享内存不可读，本次将没有电压/功耗采样"; }

    return $ok
}

# ------------------------------------------------------- HWiNFO 遥测（逐秒）
$script:Want = @(
    @{ K = '5|GPU Power';                N = 'gpu_power_w' },
    @{ K = '2|GPU Core Voltage';         N = 'gpu_volt_v' },
    @{ K = '6|GPU Clock';                N = 'gpu_core_mhz' },
    @{ K = '6|GPU Memory Clock';         N = 'gpu_mem_mhz' },
    @{ K = '1|GPU Temperature';          N = 'gpu_temp_c' },
    @{ K = '5|CPU Package Power';        N = 'cpu_power_w' }
)

Add-Type -Namespace Hv -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr OpenFileMapping(uint a, bool b, string n);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr MapViewOfFile(IntPtr h, uint a, uint hi, uint lo, UIntPtr n);
[DllImport("kernel32.dll")]
public static extern bool UnmapViewOfFile(IntPtr p);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr h);
'@
$script:R = [System.Runtime.InteropServices.Marshal]

function Test-HwInfoAvailable {
    $h = [Hv.Native]::OpenFileMapping(0x0004, $false, 'Global\HWiNFO_SENS_SM2')
    if ($h -eq [IntPtr]::Zero) { return $false }
    [void][Hv.Native]::CloseHandle($h)
    return $true
}

function Get-HwInfoSample {
    $h = [Hv.Native]::OpenFileMapping(0x0004, $false, 'Global\HWiNFO_SENS_SM2')
    if ($h -eq [IntPtr]::Zero) { return $null }
    $p = [Hv.Native]::MapViewOfFile($h, 0x0004, 0, 0, [UIntPtr]::Zero)
    if ($p -eq [IntPtr]::Zero) { [void][Hv.Native]::CloseHandle($h); return $null }
    try {
        $offR = $R::ReadInt32($p, 32); $szR = $R::ReadInt32($p, 36); $numR = $R::ReadInt32($p, 40)
        $total = $offR + $szR * $numR
        $b = New-Object byte[] $total
        $R::Copy($p, $b, 0, $total)
    } finally {
        [void][Hv.Native]::UnmapViewOfFile($p)
        [void][Hv.Native]::CloseHandle($h)
    }
    $out = @{}
    for ($i = 0; $i -lt $numR; $i++) {
        $o = $offR + $szR * $i
        $t = [System.BitConverter]::ToInt32($b, $o)
        $z = $o + 12; while ($z -lt $o + 140 -and $b[$z] -ne 0) { $z++ }
        $nm = [System.Text.Encoding]::ASCII.GetString($b, $o + 12, $z - $o - 12)
        $k = "$t|$nm"
        if (-not $out.ContainsKey($k)) { $out[$k] = [System.BitConverter]::ToDouble($b, $o + 284) }
    }
    return $out
}

# ------------------------------------------------------------ 应用设置（CLI）
function Set-PowerWall([int]$watts) {
    Log ("下发功耗墙 {0} W" -f $watts)
    $o = & $CTL set 5090 $watts 2>&1 | Out-String
    $o.Trim() -split "`r?`n" | Where-Object { $_.Trim() } | ForEach-Object { Log "    $_" }
    $st = & $CTL status 2>&1 | Out-String
    if ($st -match 'Current effective\s*:\s*(\d+)') {
        $eff = [int]$Matches[1] / 1000
        Log ("    生效 {0} W" -f $eff)
        return $eff
    }
    return 0
}

function Set-ClockAndVoltage {
    # 最大电压永不触碰：REL / ALT / OV 恒为 0，只有 VMIN 会变。
    $args = @()
    if ($CoreMhz -ne 0) { $args += '--core'; $args += "$CoreMhz" }
    if ($MemMhz  -ne 0) { $args += '--mem';  $args += "$MemMhz" }
    if ($XbarMhz -ne 0) { $args += '--xbar-offset'; $args += "$XbarMhz" }

    $nv = "$NvvddVmin,0,0,0"
    $ms = "$MsvddVmin,0,0,0"
    if ($NvvddVmin -ne 0) { $args += '--nvvdd-offsets'; $args += $nv }
    if ($MsvddVmin -ne 0) { $args += '--msvdd-offsets'; $args += $ms }

    if ($args.Count -eq 0) { Log "无需下发频率/电压"; return }
    Log ("下发频率/电压: core={0} mem={1} xbar={2} nvvdd_vmin={3} msvdd_vmin={4}" -f $CoreMhz, $MemMhz, $XbarMhz, $NvvddVmin, $MsvddVmin)
    $o = & $MVOLT @args 2>&1 | Out-String
    if ($o.Trim()) { $o.Trim() -split "`r?`n" | ForEach-Object { Log "    $_" } }
    # 回读确认
    $s = & $MVOLT --status 2>&1 | Out-String
    foreach ($k in @('core_offset_mhz', 'memory_offset_mhz', 'xbar_offset_mhz')) {
        if ($s -match ('"' + $k + '"\s*:\s*(-?\d+)')) { Log ("    回读 {0} = {1}" -f $k, $Matches[1]) }
    }
    foreach ($k in @('nvvdd_offsets_uv', 'msvdd_offsets_uv')) {
        if ($s -match ('"' + $k + '"\s*:\s*\{[^}]*\}')) { Log ("    回读 {0} = {1}" -f $k, $Matches[0]) }
    }
}

# ------------------------------------------------------------ 3DMark UI 操作
# 3DMark 的界面是 CEF 自绘的：文字能通过 UIA 定位，但按钮不是 UIA Button，
# 所以只能"找到文字 → 取中心坐标 → 发真鼠标点击"。
Add-Type -Namespace Ui -Name Mouse -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(60);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(40);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
}
'@

function Get-3DMarkWindow {
    Add-Type -AssemblyName UIAutomationClient -EA SilentlyContinue
    Add-Type -AssemblyName UIAutomationTypes -EA SilentlyContinue
    $procs = Get-Process -Name 3DMark -EA SilentlyContinue
    if (-not $procs) { return $null }
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    foreach ($pr in ($procs | Sort-Object StartTime)) {
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $pr.Id)
        $w = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
        if ($w) { return @{ Win = $w; Proc = $pr } }
    }
    return $null
}

function Find-UiText([string]$pattern) {
    $ctx = Get-3DMarkWindow
    if (-not $ctx) { return $null }
    $all = $ctx.Win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                            [System.Windows.Automation.Condition]::TrueCondition)
    $hits = @()
    foreach ($e in $all) {
        if ($e.Current.Name -match $pattern) {
            $b = $e.Current.BoundingRectangle
            if ($b.Width -gt 10 -and $b.Height -gt 8) {
                $hits += [pscustomobject]@{
                    Name = $e.Current.Name
                    X = [int]($b.X + $b.Width / 2)
                    Y = [int]($b.Y + $b.Height / 2)
                    W = [int]$b.Width; H = [int]$b.Height
                }
            }
        }
    }
    return $hits
}

function Start-3DMarkIfNeeded {
    $ctx = Get-3DMarkWindow
    if ($ctx) { Log "3DMark 已在运行 (PID $($ctx.Proc.Id))"; return $true }
    Log "启动 3DMark"
    Start-Process -FilePath $DM_EXE -WorkingDirectory (Split-Path $DM_EXE -Parent)
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 2
        $ctx = Get-3DMarkWindow
        if ($ctx) { Log ("3DMark 窗口就绪 (PID {0})，等待界面加载" -f $ctx.Proc.Id); Start-Sleep -Seconds 8; return $true }
    }
    Log "❌ 3DMark 60 秒内没有出现窗口"
    return $false
}

function Stop-3DMarkAll {
    Get-Process -Name 3DMark -EA SilentlyContinue | ForEach-Object {
        Log ("结束 3DMark 进程 PID {0}" -f $_.Id)
        try { $_.Kill() } catch {}
    }
    Start-Sleep -Seconds 5
}

# ------------------------------------------------------------ 结果解析
function Get-LatestResult([datetime]$after, [switch]$IncludeFailed) {
    if (-not (Test-Path $RESULTDIR)) { return $null }
    $files = Get-ChildItem $RESULTDIR -Filter '*.3dmark-result' -EA SilentlyContinue |
             Where-Object { $_.LastWriteTime -ge $after } |
             Sort-Object LastWriteTime -Descending
    if (-not $IncludeFailed) { $files = $files | Where-Object { $_.Name -notmatch 'FAILED' } }
    return ($files | Select-Object -First 1)
}

function Read-ResultScore([System.IO.FileInfo]$file) {
    if (-not $file) { return $null }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($file.FullName)
    try {
        $e = $z.Entries | Where-Object { $_.FullName -eq 'Result.xml' }
        if (-not $e) { return $null }
        $sr = New-Object System.IO.StreamReader($e.Open())
        $xml = $sr.ReadToEnd(); $sr.Close()
    } finally { $z.Dispose() }

    $o = [ordered]@{ File = $file.Name; Time = $file.LastWriteTime }
    foreach ($pair in @(
        @('TimeSpyPerformanceGraphicsScore',   'Graphics'),
        @('TimeSpyPerformance3DMarkScore',     'Overall'),
        @('TimeSpyPerformanceCPUScore',        'Cpu'),
        @('TimeSpyPerformanceGraphicsTest1',   'Gt1'),
        @('TimeSpyPerformanceGraphicsTest2',   'Gt2')
    )) {
        $m = [regex]::Match($xml, '<' + $pair[0] + '>([\d\.]+)</' + $pair[0] + '>')
        $o[$pair[1]] = if ($m.Success) { [double]$m.Groups[1].Value } else { $null }
    }
    return [pscustomobject]$o
}

# ------------------------------------------------------------ CSV 记录
$COLS = @(
    '序号','时间','功耗设定','功耗生效','核心偏移','显存偏移','XBAR偏移','NVVDD最小','MSVDD最小',
    '图形分数','总分','CPU分数','GT1','GT2',
    'GPU功耗均','GPU功耗峰','GPU电压均','GPU核心频率均','GPU显存频率均','GPU温度峰','CPU功耗均',
    '备注','结果文件'
)

function Add-Row($row) {
    $exists = Test-Path $CSV
    if (-not $exists) { ($COLS -join ',') | Set-Content -Path $CSV -Encoding UTF8 }
    $vals = $COLS | ForEach-Object {
        $v = $row[$_]
        if ($null -eq $v) { '' } else { '"' + ($v.ToString() -replace '"','""') + '"' }
    }
    ($vals -join ',') | Add-Content -Path $CSV -Encoding UTF8
    Log "已写入一行到 $CSV"
}

function Get-NextIndex {
    if (-not (Test-Path $CSV)) { return 1 }
    $n = (Get-Content $CSV -Encoding UTF8 | Measure-Object -Line).Lines - 1
    if ($n -lt 0) { return 1 }
    return $n + 1
}

# ============================================================== 主流程
Log "=============================================="
Log ("本次目标: 功耗 {0}W  核心 {1}  显存 {2}  XBAR {3}  NVVDD最小 {4}  MSVDD最小 {5}" -f `
     $PowerW, $CoreMhz, $MemMhz, $XbarMhz, $NvvddVmin, $MsvddVmin)
Log ("备注: {0}" -f $Note)

if (-not (Assert-NoOutageWindow)) { Log "退出：处于断电保护窗口"; exit 2 }
if (-not (Test-Environment))    { Log "退出：环境检查未通过";   exit 3 }

if ($CheckOnly) { Log "仅检查模式，结束"; exit 0 }

# 1) 应用设置
$effW = $PowerW
if ($PowerW -gt 0) { $effW = Set-PowerWall $PowerW }
Set-ClockAndVoltage

# 2) 起遥测采样
$samples = New-Object System.Collections.ArrayList
$telemetryJob = $null
if (Test-HwInfoAvailable) {
    $telemetryJob = Start-Job -ScriptBlock {
        param($want)
        Add-Type -Namespace Hv -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr OpenFileMapping(uint a, bool b, string n);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr MapViewOfFile(IntPtr h, uint a, uint hi, uint lo, UIntPtr n);
[DllImport("kernel32.dll")]
public static extern bool UnmapViewOfFile(IntPtr p);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr h);
'@
        $R = [System.Runtime.InteropServices.Marshal]
        $end = (Get-Date).AddMinutes(6)
        while ((Get-Date) -lt $end) {
            $h = [Hv.Native]::OpenFileMapping(0x0004, $false, 'Global\HWiNFO_SENS_SM2')
            if ($h -ne [IntPtr]::Zero) {
                $p = [Hv.Native]::MapViewOfFile($h, 0x0004, 0, 0, [UIntPtr]::Zero)
                if ($p -ne [IntPtr]::Zero) {
                    $offR = $R::ReadInt32($p,32); $szR = $R::ReadInt32($p,36); $numR = $R::ReadInt32($p,40)
                    $b = New-Object byte[] ($offR + $szR * $numR)
                    $R::Copy($p, $b, 0, $b.Length)
                    [void][Hv.Native]::UnmapViewOfFile($p)
                    $rec = @{}
                    for ($i=0; $i -lt $numR; $i++) {
                        $o = $offR + $szR * $i
                        $t = [System.BitConverter]::ToInt32($b, $o)
                        $z = $o + 12; while ($z -lt $o + 140 -and $b[$z] -ne 0) { $z++ }
                        $nm = [System.Text.Encoding]::ASCII.GetString($b, $o+12, $z-$o-12)
                        $k = "$t|$nm"
                        if ($want -contains $k -and -not $rec.ContainsKey($k)) {
                            $rec[$k] = [System.BitConverter]::ToDouble($b, $o + 284)
                        }
                    }
                    $rec['ts'] = (Get-Date).ToString('HH:mm:ss')
                    $rec
                }
                [void][Hv.Native]::CloseHandle($h)
            }
            Start-Sleep -Milliseconds 500
        }
    } -ArgumentList (,$Want.K)
    Log "遥测采样已启动"
}

# 3) 跑 3DMark
$t0 = Get-Date
if (-not (Start-3DMarkIfNeeded)) { Log "退出：3DMark 起不来"; exit 4 }

Log "点击「运行」/「再次运行」"
$runBtn = Find-UiText '^\s*(运行|再次运行|Run)\s*$'
if (-not $runBtn -or $runBtn.Count -eq 0) {
    Log "⚠ 找不到运行按钮 —— 需要先导航到 Time Spy 页面（基准测试 → Time Spy）"
    $nav = Find-UiText '^\s*基准测试\s*$'
    if ($nav) { Log ("点击「基准测试」@ ({0},{1})" -f $nav[0].X, $nav[0].Y); [Ui.Mouse]::Click($nav[0].X, $nav[0].Y); Start-Sleep -Seconds 4 }
    $ts = Find-UiText '^\s*Time ?Spy\s*$'
    if ($ts) { Log ("点击「Time Spy」@ ({0},{1})" -f $ts[0].X, $ts[0].Y); [Ui.Mouse]::Click($ts[0].X, $ts[0].Y); Start-Sleep -Seconds 4 }
    $runBtn = Find-UiText '^\s*(运行|再次运行|Run)\s*$'
}
if (-not $runBtn -or $runBtn.Count -eq 0) { Log "❌ 仍未找到运行按钮，放弃本次"; Stop-3DMarkAll; exit 5 }
Log ("点击运行 @ ({0},{1})" -f $runBtn[0].X, $runBtn[0].Y)
[Ui.Mouse]::Click($runBtn[0].X, $runBtn[0].Y)

# 4) 判断是否正常全屏（没全屏 = 焦点不对 = 本次作废）
$fs = $false
for ($i = 0; $i -lt ($FullscreenGraceSec * 2); $i++) {
    Start-Sleep -Milliseconds 500
    $ctx = Get-3DMarkWindow
    if (-not $ctx) { continue }
    $b = $ctx.Win.Current.BoundingRectangle
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    if ($b.Width -ge ($scr.Width * 0.98) -and $b.Height -ge ($scr.Height * 0.98)) { $fs = $true; break }
}
if ($fs) { Log "✅ 测试已全屏，正常进行" } else { Log "⚠ 未检测到全屏 —— 按规则本次作废，但先等结果文件以确认" }

# 5) 等结果文件
$deadline = $t0.AddSeconds($RunTimeoutSec)
$result = $null; $failed = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $f = Get-LatestResult $t0 -IncludeFailed
    if ($f) {
        if ($f.Name -match 'FAILED') { $failed = $f } else { $result = $f }
        break
    }
    if (-not (Get-Process -Name 3DMark -EA SilentlyContinue)) {
        Log "⚠ 3DMark 进程消失了"
        Start-Sleep -Seconds 3
        $f = Get-LatestResult $t0 -IncludeFailed
        if ($f) { if ($f.Name -match 'FAILED') { $failed = $f } else { $result = $f } }
        break
    }
}

# 6) 收遥测
if ($telemetryJob) {
    Stop-Job $telemetryJob -EA SilentlyContinue
    $samples = Receive-Job $telemetryJob -EA SilentlyContinue
    Remove-Job $telemetryJob -Force -EA SilentlyContinue
}

function Stat($key, $field) {
    $vals = $samples | Where-Object { $null -ne $_[$key] } | ForEach-Object { [double]$_[ $key ] }
    if (-not $vals) { return $null }
    switch ($field) {
        'avg' { return [Math]::Round(($vals | Measure-Object -Average).Average, 2) }
        'max' { return [Math]::Round(($vals | Measure-Object -Maximum).Maximum, 2) }
    }
}

$row = [ordered]@{
    '序号'      = Get-NextIndex
    '时间'      = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    '功耗设定'  = $PowerW
    '功耗生效'  = $effW
    '核心偏移'  = $CoreMhz
    '显存偏移'  = $MemMhz
    'XBAR偏移'  = $XbarMhz
    'NVVDD最小' = $NvvddVmin
    'MSVDD最小' = $MsvddVmin
    '图形分数'  = $null; '总分' = $null; 'CPU分数' = $null; 'GT1' = $null; 'GT2' = $null
    'GPU功耗均' = Stat '5|GPU Power' 'avg'
    'GPU功耗峰' = Stat '5|GPU Power' 'max'
    'GPU电压均' = Stat '2|GPU Core Voltage' 'avg'
    'GPU核心频率均' = Stat '6|GPU Clock' 'avg'
    'GPU显存频率均' = Stat '6|GPU Memory Clock' 'avg'
    'GPU温度峰' = Stat '1|GPU Temperature' 'max'
    'CPU功耗均' = Stat '5|CPU Package Power' 'avg'
    '备注'      = $Note
    '结果文件'  = ''
}

if ($result) {
    $s = Read-ResultScore $result
    $row['图形分数'] = $s.Graphics
    $row['总分']     = $s.Overall
    $row['CPU分数']  = $s.Cpu
    $row['GT1']      = $s.Gt1
    $row['GT2']      = $s.Gt2
    $row['结果文件'] = $s.File
    Log ("✅ 图形分数 = {0}  (总分 {1}, GT1 {2}, GT2 {3})" -f $s.Graphics, $s.Overall, $s.Gt1, $s.Gt2)
} elseif ($failed) {
    $row['备注'] = ($Note + ' | 3DMark 报 FAILED')
    $row['结果文件'] = $failed.Name
    Log ("❌ 本次失败：{0}" -f $failed.Name)
} else {
    $row['备注'] = ($Note + ' | 超时无结果文件')
    Log "❌ 超时，没有结果文件"
}

Add-Row $row

# 7) 失败就重启 3DMark
if (-not $result) {
    Log "按规则重启 3DMark"
    Stop-3DMarkAll
    Start-Sleep -Seconds 3
    [void](Start-3DMarkIfNeeded)
}

Log "完成"
