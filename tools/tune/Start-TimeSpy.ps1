# ============================================================================
#  Start-TimeSpy.ps1 —— 启动 3DMark（如未运行）并导航到 Time Spy 页面
#
#  3DMark 的界面是 CEF 自绘的：按钮不是 UIA Button，只能"定位文字 → 取坐标 → 真鼠标点击"。
#  而且 3DMark 是 DPI-unaware，UIA 报的是逻辑坐标，必须 ×2 才是物理像素。
#
#  导航路径：基准测试 → 滚动 → Time Spy 磁贴
#  结果：停在 Time Spy 详情页（有「运行」按钮）
# ============================================================================

param([switch]$ForceRestart)

$ErrorActionPreference = 'Stop'
$DM = 'D:\SteamLibrary\steamapps\common\3DMark\bin\x64\3DMark.exe'
$APPID = 223850

Add-Type -Namespace N -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
public const uint LEFTDOWN=0x0002, LEFTUP=0x0004, WHEEL=0x0800;
public static void Click(int x, int y) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
}
public static void Scroll(int x, int y, int notches) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(150);
    for (int i = 0; i < System.Math.Abs(notches); i++) {
        mouse_event(WHEEL, 0, 0, (uint)(notches > 0 ? -120 : 120), UIntPtr.Zero);
        System.Threading.Thread.Sleep(120);
    }
}
'@
Add-Type -AssemblyName UIAutomationClient  -EA SilentlyContinue
Add-Type -AssemblyName UIAutomationTypes    -EA SilentlyContinue
Add-Type -AssemblyName System.Windows.Forms -EA SilentlyContinue

function Get-DmWin {
    $p = Get-Process -Name 3DMark -EA SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $pc = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
    $w = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $pc)
    if (-not $w) { return $null }
    return @{ Proc = $p; Win = $w }
}

# 找元素并返回物理坐标中心（UIA 逻辑坐标 ×2）
function Find-Phys($win, [string]$pattern, [string]$typeFilter = '') {
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        if ($e.Current.Name -match $pattern) {
            if ($typeFilter -and ($e.Current.ControlType.ProgrammaticName -notmatch $typeFilter)) { continue }
            $b = $e.Current.BoundingRectangle
            if ($b.Width -lt 5) { continue }
            return @{ X = [int](($b.X + $b.Width/2) * 2); Y = [int](($b.Y + $b.Height/2) * 2); Name = $e.Current.Name }
        }
    }
    return $null
}

# ---- 1. 确保 3DMark 在运行 ----
if ($ForceRestart) {
    Get-Process -Name 3DMark -EA SilentlyContinue | ForEach-Object { Write-Host "  结束 3DMark PID $($_.Id)"; try { $_.Kill() } catch {} }
    Start-Sleep -Seconds 6
}

$ctx = Get-DmWin
if (-not $ctx) {
    Write-Host "  启动 3DMark (Steam appid $APPID)"
    Start-Process "steam://rungameid/$APPID"
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Seconds 3
        $ctx = Get-DmWin
        if ($ctx) { Write-Host "  窗口就绪，等界面加载"; Start-Sleep -Seconds 20; break }
    }
    if (-not $ctx) { Write-Host "  ❌ 3DMark 起不来"; exit 1 }
} else {
    Write-Host "  3DMark 已在运行 PID $($ctx.Proc.Id)"
}

[void][N.U]::ShowWindow($ctx.Proc.MainWindowHandle, 3)
[void][N.U]::SetForegroundWindow($ctx.Proc.MainWindowHandle)
Start-Sleep -Seconds 2

# ---- 2. 如果已经在 Time Spy 详情页（有「运行」），直接收工 ----
$ctx = Get-DmWin
$run = Find-Phys $ctx.Win '^\s*运行\s*$' 'Hyperlink'
if ($run) { Write-Host "  ✅ 已在 Time Spy 页面，「运行」在 ($($run.X),$($run.Y))"; exit 0 }

# ---- 3. 点「基准测试」 ----
$ctx = Get-DmWin
$nav = Find-Phys $ctx.Win '^\s*基准测试\s*$' 'Hyperlink'
if (-not $nav) { Write-Host "  ❌ 找不到「基准测试」导航"; exit 2 }
Write-Host "  点击「基准测试」($($nav.X),$($nav.Y))"
[N.U]::Click($nav.X, $nav.Y)
Start-Sleep -Seconds 7

# ---- 4. 滚动到 Time Spy 可见 ----
$ctx = Get-DmWin
for ($try = 1; $try -le 6; $try++) {
    $ts = Find-Phys $ctx.Win '^\s*Time Spy\s*$'
    if ($ts) { Write-Host "  Time Spy 磁贴已在 ($($ts.X),$($ts.Y))"; break }
    Write-Host "  第 $try 次滚动"
    [N.U]::Scroll(2200, 1600, 3)
    Start-Sleep -Seconds 2
    $ctx = Get-DmWin
}

# ---- 5. 点 Time Spy 磁贴（点文字左侧一点，避开右下角的「已安装」） ----
$ctx = Get-DmWin
$ts = Find-Phys $ctx.Win '^\s*Time Spy\s*$'
if (-not $ts) { Write-Host "  ❌ 找不到 Time Spy 磁贴"; exit 3 }
$tx = $ts.X
$ty = $ts.Y
Write-Host "  点击 Time Spy 磁贴 ($tx,$ty)"
[N.U]::Click($tx, $ty)
Start-Sleep -Seconds 7

# ---- 6. 确认到了详情页 ----
$ctx = Get-DmWin
$run = Find-Phys $ctx.Win '^\s*运行\s*$' 'Hyperlink'
if ($run) { Write-Host "  ✅ 到达 Time Spy 页面，「运行」在 ($($run.X),$($run.Y))"; exit 0 }
Write-Host "  ⚠ 点完磁贴后没看到「运行」，再试一次点磁贴"
[N.U]::Click($tx, $ty)
Start-Sleep -Seconds 6
$ctx = Get-DmWin
$run = Find-Phys $ctx.Win '^\s*运行\s*$' 'Hyperlink'
if ($run) { Write-Host "  ✅ 到达 Time Spy 页面，「运行」在 ($($run.X),$($run.Y))"; exit 0 }
Write-Host "  ❌ 导航失败"
exit 4
