[CmdletBinding()]
param(
    # Where the PNGs and the icon go. Defaults to the launcher's Branding folder.
    [string]$OutputDirectory = '',
    # Also write a preview of the mark at 512 px on light and dark backgrounds.
    [string]$PreviewPath = ''
)

# Renders the launcher icon (and optional previews) from the vector mark in
# launcher/LavenderShift.Launcher/Branding/Mark.xaml, so the art has one
# source. Runs under Windows PowerShell (WPF needs an STA thread).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase

$root = Split-Path -Parent $PSScriptRoot
$branding = Join-Path $root 'launcher/LavenderShift.Launcher/Branding'
if (-not $OutputDirectory) { $OutputDirectory = $branding }
$dictionary = [Windows.Markup.XamlReader]::Parse((Get-Content -Raw -LiteralPath (Join-Path $branding 'Mark.xaml')))
$geometry = $dictionary['MarkGeometry']

function New-MarkPng {
    param([int]$Size, [string]$Color, [string]$Background = '')
    $visual = New-Object Windows.Media.DrawingVisual
    $context = $visual.RenderOpen()
    if ($Background) {
        $context.DrawRectangle((New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString($Background))),
            $null, (New-Object Windows.Rect 0, 0, $Size, $Size))
    }
    # The mark's 400 x 420 box, centred with a small margin.
    $scale = ($Size * 0.94) / 420
    $context.PushTransform((New-Object Windows.Media.TranslateTransform (($Size - 400 * $scale) / 2), ($Size * 0.03)))
    $context.PushTransform((New-Object Windows.Media.ScaleTransform $scale, $scale))
    $brush = New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString($Color))
    $context.DrawGeometry($brush, $null, $geometry)
    $context.Pop(); $context.Pop(); $context.Close()
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap $Size, $Size, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = New-Object IO.MemoryStream
    $encoder.Save($stream)
    , $stream.ToArray()
}

# The icon: lavender on a dark rounded square reads on light and dark
# taskbars alike; PNG frames (Windows Vista and later).
function New-IconPng {
    param([int]$Size)
    $visual = New-Object Windows.Media.DrawingVisual
    $context = $visual.RenderOpen()
    $radius = $Size * 0.22
    $context.DrawRoundedRectangle((New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString('#2B1F44'))),
        $null, (New-Object Windows.Rect 0, 0, $Size, $Size), $radius, $radius)
    $scale = ($Size * 0.80) / 420
    $context.PushTransform((New-Object Windows.Media.TranslateTransform (($Size - 400 * $scale) / 2), ($Size * 0.10)))
    $context.PushTransform((New-Object Windows.Media.ScaleTransform $scale, $scale))
    $brush = New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString('#C9B3F5'))
    $context.DrawGeometry($brush, $null, $geometry)
    $context.Pop(); $context.Pop(); $context.Close()
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap $Size, $Size, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = New-Object IO.MemoryStream
    $encoder.Save($stream)
    , $stream.ToArray()
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @($sizes | ForEach-Object { , (New-IconPng -Size $_) })
$icon = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter $icon
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $edge = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$edge); $writer.Write([byte]$edge); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($frame in $frames) { $writer.Write($frame) }
$writer.Flush()
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'lavender-shift.ico'), $icon.ToArray())

if ($PreviewPath) {
    $previewDir = Split-Path -Parent $PreviewPath
    $name = [IO.Path]::GetFileNameWithoutExtension($PreviewPath)
    [IO.File]::WriteAllBytes((Join-Path $previewDir "$name-dark.png"), (New-MarkPng -Size 512 -Color '#C9B3F5' -Background '#0E0B12'))
    [IO.File]::WriteAllBytes((Join-Path $previewDir "$name-light.png"), (New-MarkPng -Size 512 -Color '#3A2A5A' -Background '#FFFFFF'))
    [IO.File]::WriteAllBytes((Join-Path $previewDir "$name-icon.png"), $frames[-1])
}
'ok'
