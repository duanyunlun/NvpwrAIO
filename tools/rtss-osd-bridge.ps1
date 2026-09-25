# ============================================================================
#  nvpwr-osd.ps1 —— 读 HWiNFO 传感器 → 拼英文文本 → 推进 RTSS 的 OSD
#
#  为什么需要它
#    HWiNFO 自己推给 RTSS 的 OSD 用的是界面上的中文名，而 RTSS 普通 OSD 走
#    Raster 3D 位图字体（只有 ASCII），中文渲染成乱码。OverlayEditor 虽然支持
#    中文，但它需要 Vector 渲染模式，这台机器上选不了。
#
#    所以反过来做：自己读 HWiNFO 共享内存（那里有原始英文名和数值），拼成纯
#    ASCII 文本，写进 RTSS 的 OSD 槽位。RTSS 只负责画，内容全由我们决定。
#
#  依赖
#    · HWiNFO 运行中，且已打开【共享内存支持】（设置 → 主要设置）
#    · RTSS 运行中
#
#  用法
#    pwsh -File nvpwr-osd.ps1            持续运行
#    pwsh -File nvpwr-osd.ps1 -Once      只写一次（自检用）
#    Ctrl+C 退出（自动释放槽位）
# ============================================================================

param([int]$IntervalMs = 500, [switch]$Once)

$OWNER = 'NvpwrOSD'
$RTSS_SIG = 0x52545353          # 'RTSS'

Add-Type -Namespace Osd -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr OpenFileMapping(uint a, bool b, string n);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr MapViewOfFile(IntPtr h, uint a, uint hi, uint lo, UIntPtr n);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool UnmapViewOfFile(IntPtr p);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr h);
'@
$R = [System.Runtime.InteropServices.Marshal]
$FM_READ = 0x0004
$FM_ALL  = 0xF001F

function Addr([IntPtr]$base, [int]$off) { [IntPtr]::new($base.ToInt64() + $off) }

function Get-Ascii([IntPtr]$base, [int]$off, [int]$len) {
    $b = New-Object byte[] $len
    $R::Copy((Addr $base $off), $b, 0, $len)
    $z = [Array]::IndexOf($b, [byte]0)
    if ($z -lt 0) { $z = $len }
    if ($z -eq 0) { return '' }
    return [System.Text.Encoding]::ASCII.GetString($b, 0, $z)
}

function Set-Ascii([IntPtr]$base, [int]$off, [int]$len, [string]$text) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($text)
    $n = [Math]::Min($bytes.Length, $len - 1)
    $R::Copy($bytes, 0, (Addr $base $off), $n)
    $R::WriteByte((Addr $base $off), $n, 0)
}

# ------------------------------------------------------------------ 要显示的项
# T: 1=温度 2=电压 5=功耗 6=频率
# N: HWiNFO 共享内存里的【原始英文标签 szLabelOrig】—— 匹配用的是这个，不是中文显示名
$WANT = @(
    @{ L = 'CPU Power';        U = 'W';   T = 5; N = 'CPU Package Power' },
    @{ L = 'CPU Temp';         U = 'C';   T = 1; N = 'CPU (Tctl/Tdie)' },
    @{ L = 'CPU Voltage';      U = 'V';   T = 2; N = 'CPU VDDCR_VDD Voltage (SVI3 TFN)' },
    @{ L = 'CPU Core 0';       U = 'MHz'; T = 6; N = 'Core 0 Clock (perf #1)' },
    @{ L = 'CPU Core 1';       U = 'MHz'; T = 6; N = 'Core 1 Clock (perf #1)' },
    @{ L = 'CPU Core 2';       U = 'MHz'; T = 6; N = 'Core 2 Clock (perf #5)' },
    @{ L = 'CPU Core 3';       U = 'MHz'; T = 6; N = 'Core 3 Clock (perf #2)' },
    @{ L = 'CPU Core 4';       U = 'MHz'; T = 6; N = 'Core 4 Clock (perf #3)' },
    @{ L = 'CPU Core 5';       U = 'MHz'; T = 6; N = 'Core 5 Clock (perf #4)' },
    @{ L = 'CPU Core 6';       U = 'MHz'; T = 6; N = 'Core 6 Clock (perf #6)' },
    @{ L = 'CPU Core 7';       U = 'MHz'; T = 6; N = 'Core 7 Clock (perf #7)' },
    @{ L = 'GPU Power';        U = 'W';   T = 5; N = 'GPU Power' },
    @{ L = 'GPU Voltage';      U = 'V';   T = 2; N = 'GPU Core Voltage' },
    @{ L = 'GPU Temp';         U = 'C';   T = 1; N = 'GPU Temperature' },
    @{ L = 'GPU Core Clock';   U = 'MHz'; T = 6; N = 'GPU Clock' },
    @{ L = 'GPU Memory Clock'; U = 'MHz'; T = 6; N = 'GPU Memory Clock' }
)

# ---------------------------------------------------------------- HWiNFO 读取
function Read-HwInfo {
    $h = [Osd.Native]::OpenFileMapping($FM_READ, $false, 'Global\HWiNFO_SENS_SM2')
    if ($h -eq [IntPtr]::Zero) { return $null }
    $p = [Osd.Native]::MapViewOfFile($h, $FM_READ, 0, 0, [UIntPtr]::Zero)
    if ($p -eq [IntPtr]::Zero) { [void][Osd.Native]::CloseHandle($h); return $null }
    try {
        $offS = $R::ReadInt32($p, 20); $szS = $R::ReadInt32($p, 24); $numS = $R::ReadInt32($p, 28)
        $offR = $R::ReadInt32($p, 32); $szR = $R::ReadInt32($p, 36); $numR = $R::ReadInt32($p, 40)
        $total = $offR + $szR * $numR
        $b = New-Object byte[] $total
        $R::Copy($p, $b, 0, $total)
    } finally {
        [void][Osd.Native]::UnmapViewOfFile($p)
        [void][Osd.Native]::CloseHandle($h)
    }

    $out = @{}
    for ($i = 0; $i -lt $numR; $i++) {
        $o = $offR + $szR * $i
        $type = [System.BitConverter]::ToInt32($b, $o)
        # szLabelOrig 在读数元素 +12，长 128
        $z = $o + 12
        while ($z -lt $o + 140 -and $b[$z] -ne 0) { $z++ }
        $name = [System.Text.Encoding]::ASCII.GetString($b, $o + 12, $z - $o - 12)
        $key = "$type|$name"
        if (-not $out.ContainsKey($key)) {
            $out[$key] = [System.BitConverter]::ToDouble($b, $o + 284)
        }
    }
    return $out
}

function Format-Osd($data) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($w in $WANT) {
        $key = "$($w.T)|$($w.N)"
        if ($data -and $data.ContainsKey($key)) {
            $v = $data[$key]
            $num = if ($w.U -eq 'MHz') { '{0,8:N0}' -f $v } else { '{0,7:N1}' -f $v }
            [void]$sb.AppendLine($w.L.PadRight(17) + $num + ' ' + $w.U)
        } else {
            [void]$sb.AppendLine($w.L.PadRight(17) + '     n/a')
        }
    }
    return $sb.ToString().TrimEnd("`r", "`n")
}

# ------------------------------------------------------------------ RTSS 通道
function Open-Rtss {
    $h = [Osd.Native]::OpenFileMapping($FM_ALL, $false, 'RTSSSharedMemoryV2')
    if ($h -eq [IntPtr]::Zero) { return $null }
    $p = [Osd.Native]::MapViewOfFile($h, $FM_ALL, 0, 0, [UIntPtr]::Zero)
    if ($p -eq [IntPtr]::Zero) { [void][Osd.Native]::CloseHandle($h); return $null }
    if ($R::ReadInt32($p, 0) -ne $RTSS_SIG) {
        [void][Osd.Native]::UnmapViewOfFile($p); [void][Osd.Native]::CloseHandle($h); return $null
    }
    return @{ H = $h; P = $p
              Size = $R::ReadInt32($p, 20)
              Off  = $R::ReadInt32($p, 24)
              Count = $R::ReadInt32($p, 28)
              Ver  = $R::ReadInt32($p, 4) }
}

function Publish-Osd($rtss, [string]$text) {
    $p = $rtss.P
    for ($pass = 0; $pass -lt 2; $pass++) {
        # 第 1 遍：找已属于自己的槽；第 2 遍：认领空槽（从 1 开始，0 号留给 AB/Precision）
        for ($e = 1; $e -lt $rtss.Count; $e++) {
            $base = $rtss.Off + $e * $rtss.Size
            $owner = Get-Ascii $p ($base + 256) 256
            if ($pass -eq 1) {
                if ($owner -ne '') { continue }
                Set-Ascii $p ($base + 256) 256 $OWNER
                $owner = $OWNER
            }
            if ($owner -ne $OWNER) { continue }

            if ($rtss.Ver -ge 0x00020007) { Set-Ascii $p ($base + 512) 4096 $text }
            else                          { Set-Ascii $p ($base + 0)   256  $text }
            if ($rtss.Ver -ge 0x0002000e) { $R::WriteInt32($p, 36, 0) }   # 清 dwBusy
            $R::WriteInt32($p, 32, $R::ReadInt32($p, 32) + 1)              # dwOSDFrame++

            # 写回校验，不再假报成功
            $back = if ($rtss.Ver -ge 0x00020007) { Get-Ascii $p ($base + 512) 4096 }
                    else                          { Get-Ascii $p ($base + 0) 256 }
            return ($back -eq $text)
        }
    }
    return $false
}

function Close-Rtss($rtss) {
    if (-not $rtss) { return }
    $p = $rtss.P
    for ($e = 1; $e -lt $rtss.Count; $e++) {
        $base = $rtss.Off + $e * $rtss.Size
        if ((Get-Ascii $p ($base + 256) 256) -eq $OWNER) {
            Set-Ascii $p ($base + 256) 256 ''
            Set-Ascii $p ($base + 512) 4096 ''
            Set-Ascii $p ($base + 0) 256 ''
            $R::WriteInt32($p, 32, $R::ReadInt32($p, 32) + 1)
        }
    }
    [void][Osd.Native]::UnmapViewOfFile($rtss.P)
    [void][Osd.Native]::CloseHandle($rtss.H)
}

# ---------------------------------------------------------------------- 主循环
$rtss = Open-Rtss
if (-not $rtss) { Write-Host "打不开 RTSSSharedMemoryV2 —— RTSS 没运行？" -ForegroundColor Red; exit 1 }

$probe = Read-HwInfo
if (-not $probe) {
    Write-Host "打不开 HWiNFO 共享内存 —— 请在 HWiNFO 里打开【共享内存支持】(设置 → 主要设置)" -ForegroundColor Yellow
}

Write-Host ("已连接 RTSS (v{0}.{1})。按 Ctrl+C 退出。" -f (($rtss.Ver -shr 16) -band 0xFFFF), ($rtss.Ver -band 0xFFFF)) -ForegroundColor Green

try {
    while ($true) {
        $data = Read-HwInfo
        $text = Format-Osd $data
        $ok = Publish-Osd $rtss $text
        $miss = ($text -split "`r?`n" | Where-Object { $_ -match 'n/a' }).Count
        $line = "[{0}] {1}  {2} 字符, 缺 {3} 项" -f (Get-Date -Format 'HH:mm:ss'), $(if ($ok) { '写入成功' } else { '写入失败' }), $text.Length, $miss
        Write-Host $line -ForegroundColor $(if ($ok) { 'Gray' } else { 'Red' })
        if ($Once) { break }
        Start-Sleep -Milliseconds $IntervalMs
    }
} finally {
    Close-Rtss $rtss
    Write-Host "已释放 OSD 槽位。" -ForegroundColor Yellow
}
