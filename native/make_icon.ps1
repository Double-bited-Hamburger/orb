# Generates orb.ico (PNG-compressed frames: 256, 48, 32, 16) next to this script.
Add-Type -AssemblyName System.Drawing
$sizes = 256, 48, 32, 16
$frames = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $m = $s * 0.05
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($m, $m, $s - 2 * $m, $s - 2 * $m)
    $br = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
    $br.CenterPoint = New-Object System.Drawing.PointF ($s * 0.40), ($s * 0.36)
    $br.CenterColor = [System.Drawing.Color]::FromArgb(255, 200, 240, 255)
    $br.SurroundColors = [System.Drawing.Color[]]@([System.Drawing.Color]::FromArgb(255, 18, 60, 160))
    $g.FillPath($br, $path)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(140, 120, 100, 255)), ([Math]::Max(1, $s * 0.025))
    $g.DrawEllipse($pen, $m, $m, $s - 2 * $m, $s - 2 * $m)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , $ms.ToArray()
    $bmp.Dispose()
}
$out = Join-Path $PSScriptRoot 'orb.ico'
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$frames[$i].Length); $w.Write([UInt32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $w.Write($f) }
$w.Close()
"wrote $out"
