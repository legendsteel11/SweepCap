# Builds res/sweepcap.ico from the artwork in assets/.
#
# The .ico is assembled by hand from 32bpp BGRA plus an AND mask, rather than
# through Bitmap.Save, because that writes a single image and Windows then
# scales one size to all the others. A tray icon is drawn at 16-24 px, where
# scaling from 256 is exactly where detail turns to mush.
#
# The PNG stays the source of truth; this only converts it.
Add-Type -AssemblyName System.Drawing

$OutPath = $args[0]
$SrcPath = $args[1]
if (-not $OutPath) { $OutPath = "res\sweepcap.ico" }
if (-not $SrcPath) { $SrcPath = "assets\SweepCap-Icon-s2-png-256.png" }

if (-not (Test-Path $SrcPath)) {
    Write-Error "Source artwork not found: $SrcPath"
    exit 1
}

$sizes = 16,20,24,32,40,48,64,128,256

$source = [System.Drawing.Bitmap]::FromFile((Resolve-Path $SrcPath))

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

    # Drawn edge to edge. The artwork carries its own padding, so adding more
    # here would only make it smaller than its neighbours in the tray.
    $dst = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $g.DrawImage($source, $dst, 0, 0, $source.Width, $source.Height,
                 [System.Drawing.GraphicsUnit]::Pixel)
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

$source.Dispose()

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
[System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $OutPath), $out.ToArray())
$out.Dispose()

Write-Output "Wrote $OutPath from $SrcPath ($($images.Count) sizes)"
