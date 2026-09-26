# Nvpwr Control —— 图标生成脚本
#
# 从零画出一个多分辨率 .ico，不依赖任何图形库或外部素材。
#
# 为什么每个尺寸单独画，而不是画一张大图缩下去：
#   16 px 下，芯片的四个引脚只有不到 1 像素宽，描边会糊成灰边，闪电的转折会
#   消失 —— 缩出来的 16 px 就是一团绿点。所以小尺寸用简化造型（实心底 + 粗
#   闪电，无引脚无描边），大尺寸才加细节。这是图标设计里唯一真正重要的规则。
#
# 为什么用 PNG 压缩的 ICO 条目：
#   Vista 之后 Windows 支持 ICO 里存放 PNG。256×256 存成未压缩 BMP 要 256 KB，
#   一张图标就顶掉整个包的一大块；PNG 只要几 KB。16/24/32 仍存 BMP —— 老工具
#   链和某些 shell 扩展对小尺寸的 PNG 条目支持不完整，而 BMP 在这个尺寸本来
#   就只有几 KB。
#
# 配色取自界面本身（gui\MainWindow.xaml 的资源字典），所以任务栏里的图标和
# 窗口里的强调色是同一个绿。

param(
    [string]$OutFile = "",
    [string]$PreviewDir = ""
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutFile)    { $OutFile    = Join-Path $root 'src\gui\NvpwrControl.ico' }
if (-not $PreviewDir) { $PreviewDir = Join-Path $env:TEMP 'nvpwr-icon-preview' }

# ---- 配色（与 MainWindow.xaml 一致）---------------------------------------
$COL_BG_TOP   = [System.Drawing.Color]::FromArgb(255, 0x23, 0x29, 0x36)  # FieldBg
$COL_BG_BOT   = [System.Drawing.Color]::FromArgb(255, 0x14, 0x18, 0x21)  # 比 Rail 再深一点
$COL_EDGE     = [System.Drawing.Color]::FromArgb(255, 0x39, 0x43, 0x5A)  # FieldBorder
$COL_ACCENT   = [System.Drawing.Color]::FromArgb(255, 0x76, 0xB9, 0x00)  # Accent（NVIDIA 绿）
$COL_ACCENT_HI= [System.Drawing.Color]::FromArgb(255, 0xA6, 0xE0, 0x3A)  # 闪电高光
$COL_PIN      = [System.Drawing.Color]::FromArgb(255, 0x64, 0x74, 0x8B)  # TextDim

# 闪电轮廓，归一化坐标（0..1）。顺时针，闭合。
$BOLT = @(
    @(0.630, 0.045),
    @(0.240, 0.560),
    @(0.455, 0.560),
    @(0.375, 0.955),
    @(0.765, 0.430),
    @(0.545, 0.430)
)

function New-RoundedPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($r -le 0.01) { $p.AddRectangle(([System.Drawing.RectangleF]::new($x, $y, $w, $h))); return $p }
    $d = $r * 2
    $p.AddArc($x,           $y,           $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y,           $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d,   0, 90)
    $p.AddArc($x,           $y + $h - $d, $d, $d,  90, 90)
    $p.CloseFigure()
    return $p
}

function New-BoltPath([float]$size, [float]$inset) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $s = $size
    $pts = foreach ($pt in $BOLT) {
        [System.Drawing.PointF]::new(($pt[0] * $s), ($pt[1] * $s))
    }
    $p.AddPolygon([System.Drawing.PointF[]]$pts)
    return $p
}

function Draw-Icon([int]$size) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $f = [float]$size
    # 留边：小尺寸几乎不留，否则可用面积太小
    $pad = if ($size -le 24) { $f * 0.02 } elseif ($size -le 48) { $f * 0.045 } else { $f * 0.06 }
    $x = $pad; $y = $pad; $w = $f - 2 * $pad; $h = $f - 2 * $pad
    $radius = if ($size -le 24) { $f * 0.18 } elseif ($size -le 48) { $f * 0.20 } else { $f * 0.22 }

    # ---- 底盘 ----
    $bgPath = New-RoundedPath $x $y $w $h $radius
    $bgBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        ([System.Drawing.PointF]::new($x, $y)),
        ([System.Drawing.PointF]::new($x, $y + $h)),
        $COL_BG_TOP, $COL_BG_BOT)
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose()

    # 描边：≤24 px 不画 —— 1 px 的线在深色任务栏上只会让边缘发灰
    if ($size -ge 32) {
        $edgeW = if ($size -ge 128) { [float]($f * 0.012) } else { [float]1.5 }
        $edgePen = [System.Drawing.Pen]::new($COL_EDGE, $edgeW)
        $g.DrawPath($edgePen, $bgPath)
        $edgePen.Dispose()
    }

    # ---- 芯片引脚（仅大尺寸）----
    # 12 px 一个引脚在 16/24/32 上根本画不出来，画了也只是脏点。
    #
    # 引脚刻意做得【短】。第一版做长了将近一倍，结果四边像刺猬 —— 芯片引脚是
    # 从封装边缘探出的一小截，不是天线。长度取底盘的 5%，几乎只是给圆角矩形
    # 加了一圈规整的毛边，那才是这个尺寸下能读出来的东西。
    if ($size -ge 48) {
        $pinLen = $f * 0.052
        $pinW   = [Math]::Max(1.4, $f * 0.026)
        $pinPen = [System.Drawing.Pen]::new($COL_PIN, [float]$pinW)
        $pinPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pinPen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round

        # 引脚的起止：从底盘边缘向外伸 $pinLen，再向内压进 $pinLen*0.6 让根部和底
        # 盘有重叠，否则抗锯齿会在接缝处留一条透明缝。
        $bite = $pinLen * 0.6
        $count = 3
        for ($i = 0; $i -lt $count; $i++) {
            $t  = ($i + 1) / ($count + 1)
            $px = $x + $w * $t
            $py = $y + $h * $t
            $g.DrawLine($pinPen, $px, ($y + $bite),        $px, ($y - $pinLen))            # 上
            $g.DrawLine($pinPen, $px, ($y + $h - $bite),   $px, ($y + $h + $pinLen))      # 下
            $g.DrawLine($pinPen, ($x + $bite),        $py, ($x - $pinLen),        $py)     # 左
            $g.DrawLine($pinPen, ($x + $w - $bite),   $py, ($x + $w + $pinLen),   $py)     # 右
        }
        $pinPen.Dispose()
    }

    # ---- 闪电 ----
    # 按归一化轮廓的【实际外接框】居中，而不是按底盘内框。闪电的轮廓左右不对称
    # （0.240 到 0.765），直接用底盘中心会让它视觉上偏左。
    $boltScale = $f * 0.70
    $boltPath = New-BoltPath $boltScale 0
    $bbX = 0.240 * $boltScale
    $bbY = 0.045 * $boltScale
    $bbW = (0.765 - 0.240) * $boltScale
    $bbH = (0.955 - 0.045) * $boltScale
    $tx = ($f - $bbW) / 2 - $bbX
    $ty = ($f - $bbH) / 2 - $bbY
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([float]$tx, [float]$ty)
    $boltPath.Transform($m)
    $m.Dispose()

    $boltBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        ([System.Drawing.PointF]::new(($f * 0.3), ($f * 0.1))),
        ([System.Drawing.PointF]::new(($f * 0.7), ($f * 0.9))),
        $COL_ACCENT_HI, $COL_ACCENT)
    $g.FillPath($boltBrush, $boltPath)
    $boltBrush.Dispose()
    $boltPath.Dispose()

    $g.Dispose()
    return $bmp
}

function ConvertTo-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

# 32bpp BMP 条目（含 AND 掩码）。小尺寸用这个，老 shell 扩展更稳。
function ConvertTo-BmpBytes([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    # BITMAPINFOHEADER —— ICO 里高度要写成两倍（XOR + AND 两张图）
    $bw.Write([uint32]40)
    $bw.Write([int32]$w)
    $bw.Write([int32]($h * 2))
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)          # BI_RGB
    $bw.Write([uint32]($w * $h * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)

    # XOR 位图：自下而上，BGRA
    for ($y = $h - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G)
            $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }
    # AND 掩码：32bpp 下不用，但结构上必须有，每行按 4 字节对齐
    $maskRow = [int][Math]::Ceiling($w / 32.0) * 4
    $zero = New-Object byte[] $maskRow
    for ($y = 0; $y -lt $h; $y++) { $bw.Write($zero, 0, $maskRow) }

    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return $bytes
}

# ---- 生成各尺寸 -----------------------------------------------------------
$sizes = @(16, 24, 32, 48, 64, 128, 256)
New-Item -ItemType Directory -Path $PreviewDir -Force | Out-Null

$entries = @()
foreach ($s in $sizes) {
    $bmp = Draw-Icon $s
    $bmp.Save((Join-Path $PreviewDir "icon-$s.png"), [System.Drawing.Imaging.ImageFormat]::Png)

    if ($s -le 48) { $data = ConvertTo-BmpBytes $bmp; $isPng = $false }
    else           { $data = ConvertTo-PngBytes $bmp; $isPng = $true }

    $entries += [pscustomobject]@{ Size = $s; Data = $data; IsPng = $isPng }
    $bmp.Dispose()
    Write-Host ("  绘制 {0,3}x{0,-3}  {1,6:N0} B  {2}" -f $s, $data.Length, $(if ($isPng) { 'PNG' } else { 'BMP' }))
}

# ---- 组装 ICO -------------------------------------------------------------
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([uint16]0)                  # reserved
$bw.Write([uint16]1)                  # type = icon
$bw.Write([uint16]$entries.Count)     # count

$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 256 记作 0
    $bw.Write([byte]$dim)             # width
    $bw.Write([byte]$dim)             # height
    $bw.Write([byte]0)                # palette count
    $bw.Write([byte]0)                # reserved
    $bw.Write([uint16]1)              # planes
    $bw.Write([uint16]32)             # bpp
    $bw.Write([uint32]$e.Data.Length) # bytes
    $bw.Write([uint32]$offset)        # offset
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data, 0, $e.Data.Length) }
$bw.Flush()

$icoBytes = $ms.ToArray()
$bw.Dispose(); $ms.Dispose()

$dir = Split-Path -Parent $OutFile
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
[System.IO.File]::WriteAllBytes($OutFile, $icoBytes)

Write-Host ""
Write-Host ("  ✅ {0}" -f $OutFile) -ForegroundColor Green
Write-Host ("     {0} 个尺寸，{1:N0} B" -f $entries.Count, $icoBytes.Length) -ForegroundColor Green
Write-Host ("     预览图: {0}" -f $PreviewDir) -ForegroundColor DarkGray
