param(
    [string]$SourcePath = (Join-Path $PSScriptRoot 'quest-controller-lineart-source.png'),
    [string]$OutputPath = (Join-Path $PSScriptRoot 'quest-controller-mapping.png')
)

Add-Type -AssemblyName System.Drawing

$canvasWidth = 2048
$canvasHeight = 1024
$accent = [System.Drawing.Color]::FromArgb(255, 235, 35, 35)
$textColor = [System.Drawing.Color]::FromArgb(255, 242, 242, 242)
$transparent = [System.Drawing.Color]::Transparent

$source = $null
$lineArt = $null
$canvas = $null
$graphics = $null
$bodyFont = $null
$headingFont = $null
$linePen = $null
$ringPen = $null
$textBrush = $null
$leftFormat = $null
$rightFormat = $null

function Draw-Callout {
    param(
        [System.Drawing.Graphics]$Graphics,
        [System.Drawing.Pen]$LinePen,
        [System.Drawing.Pen]$RingPen,
        [System.Drawing.Brush]$TextBrush,
        [System.Drawing.Font]$Font,
        [System.Drawing.StringFormat]$Format,
        [string[]]$Lines,
        [int]$TextX,
        [int]$TextY,
        [System.Drawing.Point]$Target,
        [bool]$LabelOnLeft
    )

    $anchorY = $TextY + 15
    if ($LabelOnLeft) {
        $points = [System.Drawing.Point[]]@(
            [System.Drawing.Point]::new(397, $anchorY),
            [System.Drawing.Point]::new(414, $anchorY),
            [System.Drawing.Point]::new($Target.X - 28, $Target.Y),
            $Target
        )
    }
    else {
        $points = [System.Drawing.Point[]]@(
            [System.Drawing.Point]::new(1598, $anchorY),
            [System.Drawing.Point]::new(1586, $anchorY),
            [System.Drawing.Point]::new($Target.X + 28, $Target.Y),
            $Target
        )
    }

    $Graphics.DrawLines($LinePen, $points)
    $Graphics.DrawEllipse($RingPen, $Target.X - 8, $Target.Y - 8, 16, 16)

    for ($index = 0; $index -lt $Lines.Count; $index++) {
        $Graphics.DrawString(
            $Lines[$index],
            $Font,
            $TextBrush,
            [System.Drawing.PointF]::new(
                [single]$TextX,
                [single]($TextY + ($index * 25))),
            $Format)
    }
}

try {
    $source = [System.Drawing.Bitmap]::FromFile($SourcePath)
    $lineArt = [System.Drawing.Bitmap]::new(
        $source.Width,
        $source.Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)

    for ($y = 0; $y -lt $source.Height; $y++) {
        for ($x = 0; $x -lt $source.Width; $x++) {
            $pixel = $source.GetPixel($x, $y)
            $luminance = [int](($pixel.R + $pixel.G + $pixel.B) / 3)
            $alpha = 255 - $luminance
            if ($alpha -lt 10) { $alpha = 0 }
            $lineArt.SetPixel($x, $y,
                [System.Drawing.Color]::FromArgb($alpha, 245, 245, 245))
        }
    }

    $canvas = [System.Drawing.Bitmap]::new(
        $canvasWidth,
        $canvasHeight,
        [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $graphics.Clear($transparent)
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # Preserve the original controller geometry. Each half is only scaled and translated.
    $graphics.DrawImage(
        $lineArt,
        [System.Drawing.Rectangle]::new(464, 180, 520, 656),
        [System.Drawing.Rectangle]::new(0, 0, 355, 448),
        [System.Drawing.GraphicsUnit]::Pixel)
    $graphics.DrawImage(
        $lineArt,
        [System.Drawing.Rectangle]::new(1064, 180, 520, 656),
        [System.Drawing.Rectangle]::new(356, 0, 355, 448),
        [System.Drawing.GraphicsUnit]::Pixel)

    $bodyFont = [System.Drawing.Font]::new('Segoe UI', 19, [System.Drawing.FontStyle]::Regular)
    $headingFont = [System.Drawing.Font]::new('Segoe UI Semibold', 28, [System.Drawing.FontStyle]::Bold)
    $linePen = [System.Drawing.Pen]::new($accent, 5)
    $linePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $linePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $linePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $ringPen = [System.Drawing.Pen]::new($accent, 4)
    $textBrush = [System.Drawing.SolidBrush]::new($textColor)
    $leftFormat = [System.Drawing.StringFormat]::new()
    $leftFormat.SetTabStops(0, [single[]]@(125))
    $rightFormat = [System.Drawing.StringFormat]::new()
    $rightFormat.SetTabStops(0, [single[]]@(205))

    $graphics.DrawString('LEFT CONTROLLER', $headingFont, $textBrush, 45, 24)
    $graphics.DrawString('RIGHT CONTROLLER', $headingFont, $textBrush, 1610, 24)

    # Left Touch controller: one callout per physical control.
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $leftFormat `
        @("Stick`tMove", "Stick click`tMenu / Pause") 45 176 `
        ([System.Drawing.Point]::new(710, 246)) $true
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $leftFormat `
        @("Y`tSwitch weapon", "`tmodification") 45 274 `
        ([System.Drawing.Point]::new(779, 307)) $true
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $leftFormat `
        @("X`tDossier") 45 344 `
        ([System.Drawing.Point]::new(688, 349)) $true
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $leftFormat `
        @("Trigger`tEquipment /", "`tgrenade") 45 414 `
        ([System.Drawing.Point]::new(908, 435)) $true
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $leftFormat `
        @("Grip`tTwo-hand support", "Grip hold`tBFG", "`twhen not supporting", "`ta weapon", "Grip tap`tNext equipment", "`toutside grab range") 45 534 `
        ([System.Drawing.Point]::new(688, 546)) $true

    # Right Touch controller: directions and click are grouped on the same thumbstick.
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $rightFormat `
        @("Stick L / R`tTurn", "Stick up`tChainsaw", "Stick down tap`tSwitch weapon", "Stick down hold`tWeapon wheel", "`taim with left stick", "Stick click`tUse / Melee /", "`tGlory Kill") 1610 76 `
        ([System.Drawing.Point]::new(1338, 246)) $false
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $rightFormat `
        @("B`tJump") 1610 284 `
        ([System.Drawing.Point]::new(1273, 307)) $false
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $rightFormat `
        @("A`tCrouch") 1610 354 `
        ([System.Drawing.Point]::new(1357, 349)) $false
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $rightFormat `
        @("Trigger`tFire") 1610 424 `
        ([System.Drawing.Point]::new(1140, 435)) $false
    Draw-Callout $graphics $linePen $ringPen $textBrush $bodyFont $rightFormat `
        @("Grip in front`tWeapon mod /", "`tsecondary fire", "Grip behind`tDraw selected", "shoulder`tShoulder Weapon") 1610 522 `
        ([System.Drawing.Point]::new(1360, 546)) $false

    $canvas.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    if ($rightFormat) { $rightFormat.Dispose() }
    if ($leftFormat) { $leftFormat.Dispose() }
    if ($textBrush) { $textBrush.Dispose() }
    if ($ringPen) { $ringPen.Dispose() }
    if ($linePen) { $linePen.Dispose() }
    if ($headingFont) { $headingFont.Dispose() }
    if ($bodyFont) { $bodyFont.Dispose() }
    if ($graphics) { $graphics.Dispose() }
    if ($canvas) { $canvas.Dispose() }
    if ($lineArt) { $lineArt.Dispose() }
    if ($source) { $source.Dispose() }
}
