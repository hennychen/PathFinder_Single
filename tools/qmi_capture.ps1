# QMI8658 minimal bring-up UART capture helper (COM3).
# Usage: powershell -File tools\qmi_capture.ps1 [-Seconds 30] [-OutFile <path>] [-NoReset]
# - Toggles RTS to hard-reset the board (same as idf.py monitor reset), then
#   captures raw UART bytes for the given duration into a log file.

param(
    [string]$Port = "COM3",
    [int]$Seconds = 30,
    [string]$OutFile = "",
    [switch]$NoReset
)

$ErrorActionPreference = "Stop"

if ($OutFile -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutFile = Join-Path $PSScriptRoot "..\logs\qmi-capture-$stamp.log"
}
$outDir = Split-Path $OutFile -Parent
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}
if (Test-Path $OutFile) {
    Remove-Item $OutFile
}

$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$serial.ReadBufferSize = 1048576
$serial.DtrEnable = $false
$serial.RtsEnable = $false

# RTS high -> EN low on auto-reset circuits -> board held in reset.
if (-not $NoReset) {
    $serial.Open()
    $serial.RtsEnable = $true
    Start-Sleep -Milliseconds 150
    $serial.RtsEnable = $false
}

try {
    if (-not $serial.IsOpen) {
        $serial.Open()
    }
    $stream = [System.IO.StreamWriter]::new($OutFile, $false)
    $stream.AutoFlush = $true

    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $n = $serial.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            [void]$serial.Read($buf, 0, $n)
            $text = [System.Text.Encoding]::UTF8.GetString($buf)
            $stream.Write($text)
            Write-Host -NoNewline $text
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    $stream.Close()
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}

Write-Host ""
Write-Host "saved: $OutFile"
