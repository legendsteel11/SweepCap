# Generates the SweepCap tray icon.
# Assembles a multi-size .ico directly from 32bpp BGRA data plus AND masks.
Add-Type -AssemblyName System.Drawing

$OutPath = $args[0]
$sizes = 16,20,24,32,40,48,64,128,256

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x,           $y,           $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y,           $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d,   0, 90)
    $p.AddArc($x,           $y + $h - $d, $d, $d,  90, 90)
    $p.CloseFigure()
    return $p
}

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Plate: a rounded square. A filled plate rather than bare white marks, so
    # the icon reads on both light and dark taskbars.
    $inset = [single]($s * 0.045)
    $side  = [single]($s - 2 * $inset)
    $radius = [single]($s * 0.235)
    $path = New-RoundedPath $inset $inset $side $side $radius
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point(0, $s)),
        [System.Drawing.Color]::FromArgb(255, 96, 156, 255),
        [System.Drawing.Color]::FromArgb(255, 33, 92, 214))
    $g.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()

    # Mark: corner brackets standing for a capture region. A diagonal stroke was
    # tried alongside them and dropped: it merged with the brackets and read as
    # a resize icon.
    #
    # Small sizes snap to the pixel grid. The tray actually uses 16-24px, where
    # fractional coordinates turn into antialiased mush.
    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
    $swi = [int][Math]::Max(1, [Math]::Round($s * 0.075))
    $pen = New-Object System.Drawing.Pen($white, [single]$swi)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round

    if ($s -le 24) { $mi = [int][Math]::Round($s * 0.20) }
    else           { $mi = [int][Math]::Round($s * 0.235) }

    # Odd stroke widths land on pixel centres (x.5), even widths on pixel
    # boundaries; anything else bleeds.
    $off = 0.0
    if ($swi % 2 -eq 1) { $off = 0.5 }
    $lo = [single]($mi + $off)
    $hi = [single]($s - $mi - $off)

    if ($s -lt 24) {
        # Brackets fall apart at this size: draw a plain outline with AA off.
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Flat
        $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Flat
        $g.DrawRectangle($pen, $lo, $lo, $hi - $lo, $hi - $lo)
    } else {
        $arm = [single][Math]::Round(($hi - $lo) * 0.33)
        $g.DrawLines($pen, [System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new($lo + $arm, $lo),
            [System.Drawing.PointF]::new($lo, $lo),
            [System.Drawing.PointF]::new($lo, $lo + $arm)))
        $g.DrawLines($pen, [System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new($hi - $arm, $lo),
            [System.Drawing.PointF]::new($hi, $lo),
            [System.Drawing.PointF]::new($hi, $lo + $arm)))
        $g.DrawLines($pen, [System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new($lo, $hi - $arm),
            [System.Drawing.PointF]::new($lo, $hi),
            [System.Drawing.PointF]::new($lo + $arm, $hi)))
        $g.DrawLines($pen, [System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new($hi, $hi - $arm),
            [System.Drawing.PointF]::new($hi, $hi),
            [System.Drawing.PointF]::new($hi - $arm, $hi)))
    }
    $pen.Dispose()
    $g.Dispose()
    return $bmp
}

function Get-Bgra([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $buf = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)

    # XOR data in an ICO is stored bottom-up.
    $out = New-Object byte[] ($w * $h * 4)
    for ($y = 0; $y -lt $h; $y++) {
        [Array]::Copy($buf, ($h - 1 - $y) * $stride, $out, $y * $w * 4, $w * 4)
    }
    return ,$out
}

function Get-AndMask([byte[]]$bgra, [int]$w, [int]$h) {
    $rowBytes = [int]([Math]::Floor(($w + 31) / 32) * 4)
    $mask = New-Object byte[] ($rowBytes * $h)
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $alpha = $bgra[($y * $w + $x) * 4 + 3]
            if ($alpha -eq 0) {
                $mask[$y * $rowBytes + [int][Math]::Floor($x / 8)] = `
                    $mask[$y * $rowBytes + [int][Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8))
            }
        }
    }
    return ,$mask
}

$images = @()
foreach ($s in $sizes) {
    $bmp = Render $s
    $bgra = Get-Bgra $bmp
    $mask = Get-AndMask $bgra $s $s
    $bmp.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER: height covers XOR + AND, so it is written doubled.
    $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
    $bw.Write([uint16]1);  $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($bgra.Length + $mask.Length))
    $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $bw.Write([byte[]]$bgra); $bw.Write([byte[]]$mask); $bw.Flush()
    $images += ,@{ size = $s; bytes = $ms.ToArray() }
    $ms.Dispose()
}

$out = New-Object System.IO.MemoryStream
$w2 = New-Object System.IO.BinaryWriter($out)
$w2.Write([uint16]0); $w2.Write([uint16]1); $w2.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = $img.size
    if ($dim -ge 256) { $dim = 0 }
    $w2.Write([byte]$dim); $w2.Write([byte]$dim); $w2.Write([byte]0); $w2.Write([byte]0)
    $w2.Write([uint16]1); $w2.Write([uint16]32)
    $w2.Write([uint32]$img.bytes.Length); $w2.Write([uint32]$offset)
    $offset += $img.bytes.Length
}
foreach ($img in $images) { $w2.Write([byte[]]$img.bytes) }
$w2.Flush()
[System.IO.File]::WriteAllBytes($OutPath, $out.ToArray())
$out.Dispose()

Write-Output ("wrote {0} ({1} bytes, {2} sizes: {3})" -f $OutPath, (Get-Item $OutPath).Length, $images.Count, ($sizes -join ','))
