$ErrorActionPreference = "Stop"
$glyphs = (Get-Content "$PSScriptRoot\glyphs.txt" -Raw -Encoding UTF8).Trim()
Write-Host "glyph count: $($glyphs.Length)"
$codes = ($glyphs.ToCharArray() | ForEach-Object { "0x{0:X4}" -f [int]$_ }) -join ","
[System.IO.File]::WriteAllText("$PSScriptRoot\ranges.txt", $codes, [System.Text.Encoding]::ASCII)
Write-Host "range length: $($codes.Length)"
& npx lv_font_conv --font "e:\PathFinder_Single\components\service_ui\fonts\NotoSansSC-Bold.otf" --size 34 --bpp 4 --format lvgl --no-compress --lv-font-name pf_font_cn_34 -o "e:\PathFinder_Single\components\service_ui\fonts\nav_road_font.c" --range $codes
Write-Host "exit: $LASTEXITCODE"
Get-Item "e:\PathFinder_Single\components\service_ui\fonts\nav_road_font.c" | Select-Object Length
