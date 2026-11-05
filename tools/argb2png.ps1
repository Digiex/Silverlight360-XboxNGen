<#
.SYNOPSIS
Makes a PNG of an app's own screenshot (the SDK's samples\Common\Screenshot.cs, Silverdash360's ui.screenshot): rows of
four bytes a pixel, alpha, red, green and blue, premultiplied, the size given separately.

.DESCRIPTION
The alpha is dropped (an app's root is opaque, so the colours are as drawn).

.EXAMPLE
.\tools\argb2png.ps1 screenshot.argb 1280 720 out.png
#>
param(
    [Parameter(Mandatory, Position = 0)] [string]$Argb,
    [Parameter(Mandatory, Position = 1)] [int]$Width,
    [Parameter(Mandatory, Position = 2)] [int]$Height,
    [Parameter(Mandatory, Position = 3)] [string]$Png
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Argb).Path)
if ($bytes.Length -ne $Width * $Height * 4) { throw "${Argb}: $($bytes.Length) bytes, not ${Width}x$Height pixels" }
# GDI+ keeps a pixel as B, G, R, A in memory: the whole buffer reversed is that, with the pixels in reverse order,
# which turning the bitmap half a turn puts back.
[Array]::Reverse($bytes)
$bitmap = New-Object Drawing.Bitmap $Width, $Height, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
try {
    $lock = $bitmap.LockBits((New-Object Drawing.Rectangle 0, 0, $Width, $Height), [Drawing.Imaging.ImageLockMode]::WriteOnly,
        [Drawing.Imaging.PixelFormat]::Format32bppRgb)
    for ($y = 0; $y -lt $Height; $y++) {
        [Runtime.InteropServices.Marshal]::Copy($bytes, $y * $Width * 4, [IntPtr]($lock.Scan0.ToInt64() + $y * $lock.Stride), $Width * 4)
    }
    $bitmap.UnlockBits($lock)
    $bitmap.RotateFlip([Drawing.RotateFlipType]::Rotate180FlipNone)
    $bitmap.Save([IO.Path]::GetFullPath($Png), [Drawing.Imaging.ImageFormat]::Png)
} finally {
    $bitmap.Dispose()
}
