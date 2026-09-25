# ============================================================================
#  Set-Power.ps1 —— 直接给内核驱动发 IOCTL 设功耗墙
#
#  为什么不用 NvpwrCtl：它的命令行自己写了一张表，
#       else if (profile == 5080 || profile == 5090)
#           valid = watts >= 175 && watts <= 225;
#  所以 CLI 最高只能到 225 W。而驱动本身的上限是 POWER_CEILING_DEV = 350 W，
#  图形界面走的是直接 IOCTL，所以能设到 350。这个脚本走同一条路。
#
#  NVPWR_SET_POWER（pack 8）：
#      ULONG Version          恒为 3
#      ULONG TargetMilliwatts 目标功耗
#      ULONG Profile          5090 = 3
#      ULONG MaxMilliwatts    本次会话允许的上限（驱动会夹到 350000）
#      ULONG Reserved
# ============================================================================

param(
    [Parameter(Mandatory=$true)][int]$Watts,
    [int]$Profile = 3,          # NvpwrProfileRtx5090Laptop
    [int]$SessionMaxW = 350     # 会话上限
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Nv -Name Io -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr CreateFileW(string name, uint access, uint share,
    IntPtr sec, uint disp, uint flags, IntPtr tmpl);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool DeviceIoControl(IntPtr h, uint code,
    byte[] inBuf, uint inSize, byte[] outBuf, uint outSize, out uint returned, IntPtr overlapped);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr h);
public static byte[] U32(int a, int b, int c, int d, int e) {
    var r = new byte[20];
    System.BitConverter.GetBytes((uint)a).CopyTo(r, 0);
    System.BitConverter.GetBytes((uint)b).CopyTo(r, 4);
    System.BitConverter.GetBytes((uint)c).CopyTo(r, 8);
    System.BitConverter.GetBytes((uint)d).CopyTo(r, 12);
    System.BitConverter.GetBytes((uint)e).CopyTo(r, 16);
    return r;
}
'@

# CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, 0x801, METHOD_BUFFERED=0, READ|WRITE=3)
$IOCTL_SET = 0x22E004
$IOCTL_STATUS = 0x22E000      # 0x800 → (0x22<<16)|(3<<14)|(0x800<<2)|0

# PowerShell 把 0xC0000000 当有符号 Int32 解析（-1073741824），转不成 UInt32，
# 所以这两个常量要用十进制显式写出来。
$GENERIC_RW = [uint32]3221225472       # 0xC0000000
$GENERIC_READ = [uint32]2147483648     # 0x80000000
$OPEN_EXISTING = 3

$h = [Nv.Io]::CreateFileW('\\.\Nvpwr', $GENERIC_RW, 0, [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr](-1)) {
    Write-Host "  打不开 \\.\Nvpwr（驱动没加载？）错误 $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    exit 1
}

try {
    Write-Host ("  请求 {0} W，会话上限 {1} W，profile {2}" -f $Watts, $SessionMaxW, $Profile)
    $buf = [Nv.Io]::U32(3, ($Watts * 1000), $Profile, ($SessionMaxW * 1000), 0)
    $ret = 0
    $ok = [Nv.Io]::DeviceIoControl($h, $IOCTL_SET, $buf, 20, $null, 0, [ref]$ret, [IntPtr]::Zero)
    if (-not $ok) {
        Write-Host "  ❌ IOCTL 失败，Win32=$([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        exit 2
    }
    Write-Host "  ✅ IOCTL 已发出"
} finally {
    [void][Nv.Io]::CloseHandle($h)
}

# ---- 回读状态 ----
Start-Sleep -Milliseconds 800
$h2 = [Nv.Io]::CreateFileW('\\.\Nvpwr', $GENERIC_READ, 0, [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($h2 -ne [IntPtr]::Zero -and $h2 -ne [IntPtr](-1)) {
    try {
        $out = New-Object byte[] 256
        $ret2 = 0
        if ([Nv.Io]::DeviceIoControl($h2, $IOCTL_STATUS, $null, 0, $out, 256, [ref]$ret2, [IntPtr]::Zero)) {
            $rd = {
                param($off)
                # pack(8) 下这组字段都在 4 字节对齐处
                [System.BitConverter]::ToUInt32($out, $off)
            }
            $state = & $rd 4
            $names = @{0='UNKNOWN';1='ARMED';2='APPLIED';3='MIXED';4='WRONG_BUILD';5='MODULE_NOT_FOUND';6='GPU_NOT_FOUND';7='CONTEXT_INVALID';8='PRECONDITION_NOT_READY';9='STOCK_BASELINE'}
            Write-Host ("  状态 = {0}" -f $names[[int]$state])
            # 布局：Version0 State4 LastNt8 Detail12 ModuleBase16 TimeDate24 Size28
            #       RegistryCount32 SelectedIndex36 GpuId40 Res0 44
            #       7 个 ULONGLONG: 48..103  → 48,56,64,72,80,88,96
            #       4 个 UCHAR: 104..107 → RootInit104 Elig105 AmtActive106 PolicyKey107
            #       Ctgp108 Ppab112 Lower116 Upper120 Aux28 124 Aux2C 128
            #       MaxMode132 MaxCount133 MaxSource0 134 CurrentMode135 CurrentCount136 Res1 137-139
            #       MaxEff140 MaxSec144 MaxSrc0 148 CurEff152 CurF7 156 PredF7 160
            #       Applied164 LastNv168 Oem172 ActiveProfile176 SuppMin180 SuppMax184
            #       CeilingMax188 SessionMax192
            $oem   = [System.BitConverter]::ToUInt32($out, 172)
            $upper = [System.BitConverter]::ToUInt32($out, 120)
            $cur   = [System.BitConverter]::ToUInt32($out, 152)
            $appl  = [System.BitConverter]::ToUInt32($out, 164)
            $ceil  = [System.BitConverter]::ToUInt32($out, 188)
            $sess  = [System.BitConverter]::ToUInt32($out, 192)
            Write-Host ("  出厂基线 {0} W | UPPER {1} W | 当前生效 {2} W | 已下发 {3} W" -f ($oem/1000), ($upper/1000), ($cur/1000), ($appl/1000))
            Write-Host ("  驱动上限 {0} W | 会话上限 {1} W" -f ($ceil/1000), ($sess/1000))
        }
    } finally { [void][Nv.Io]::CloseHandle($h2) }
}
