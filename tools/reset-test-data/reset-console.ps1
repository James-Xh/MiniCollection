# Clear the Windows console's local test data:
#   - all cursor.txt files under the configured cursor root
#   - downloaded *.mseed / *.mseed.part files under outputDir and [DeviceDirs]
#
# Usage (run in PowerShell):
#   .\reset-console.ps1 -ConsoleDir "D:\MseedConsumer\dist-win64" -DryRun
#   .\reset-console.ps1 -ConsoleDir "D:\MseedConsumer\dist-win64"
#
# The console process is stopped automatically (unless -DryRun).
[CmdletBinding()]
param(
    [string]$ConsoleDir = ".",
    [switch]$Yes,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Resolve-ConsolePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ConsoleDir $Path))
}

function Read-Ini {
    param([string]$Path)
    $ini = @{}
    $section = ""
    foreach ($line in Get-Content -LiteralPath $Path) {
        $text = $line.Trim()
        if ($text -eq "" -or $text.StartsWith(";") -or $text.StartsWith("#")) { continue }
        if ($text -match '^\[(.+)\]$') {
            $section = $matches[1]
            continue
        }
        $index = $text.IndexOf("=")
        if ($index -lt 1) { continue }
        $key = $text.Substring(0, $index).Trim()
        $value = $text.Substring($index + 1).Trim()
        $ini["$section/$key"] = $value
    }
    return $ini
}

$ConsoleDir = (Resolve-Path -LiteralPath $ConsoleDir).Path
$configPath = Join-Path $ConsoleDir "config.ini"
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    Write-Error "config.ini not found: $configPath"
    exit 1
}

$ini = Read-Ini -Path $configPath
$stateRaw = $ini["Consumer/stateFile"]
if ([string]::IsNullOrWhiteSpace($stateRaw)) { $stateRaw = "data/state" }
$outputRaw = $ini["Consumer/outputDir"]
if ([string]::IsNullOrWhiteSpace($outputRaw)) { $outputRaw = "data/mseed" }

$statePath = Resolve-ConsolePath $stateRaw
$stateRoot = $statePath
if ([System.IO.Path]::GetExtension($statePath) -ieq ".txt") {
    $stateRoot = Split-Path -Parent $statePath
}
$outputRoot = Resolve-ConsolePath $outputRaw

$deviceDirs = @()
foreach ($key in @($ini.Keys)) {
    if ($key -like "DeviceDirs/*") {
        $path = Resolve-ConsolePath $ini[$key]
        if ($path) { $deviceDirs += $path }
    }
}

$searchDirs = @(@($outputRoot) + $deviceDirs | Where-Object { $_ } | Select-Object -Unique)

$cursorFiles = @()
if ($stateRoot -and (Test-Path -LiteralPath $stateRoot -PathType Container)) {
    $cursorFiles = @(Get-ChildItem -LiteralPath $stateRoot -Recurse -Filter "cursor.txt" -File -ErrorAction SilentlyContinue)
}

$mseedFiles = @()
foreach ($dir in $searchDirs) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
    $mseedFiles += @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*.mseed" -or $_.Name -like "*.mseed.part" })
}
$mseedFiles = @($mseedFiles | Sort-Object FullName -Unique)

Write-Host "Console dir:  $ConsoleDir"
Write-Host "Cursor root:  $stateRoot"
Write-Host "Output root:  $outputRoot"
Write-Host "Device dirs:  $($deviceDirs -join '; ')"
Write-Host "cursor.txt:   $($cursorFiles.Count)"
Write-Host "mseed files:  $($mseedFiles.Count)"
Write-Host ""

if ($DryRun) {
    Write-Host "Files that would be deleted:"
    foreach ($file in $cursorFiles) { Write-Host "  CURSOR $($file.FullName)" }
    foreach ($file in $mseedFiles) { Write-Host "  MSEED  $($file.FullName)" }
    Write-Host ""
    Write-Host "Dry run only. Nothing was deleted."
    exit 0
}

if (-not $Yes) {
    $answer = Read-Host "Type RESET to delete console test data"
    if ($answer -cne "RESET") {
        Write-Host "Cancelled."
        exit 1
    }
}

$processes = @(Get-Process -Name "MseedConsumer" -ErrorAction SilentlyContinue)
if ($processes.Count -gt 0) {
    Write-Host "Stopping MseedConsumer..."
    $processes | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

foreach ($file in $cursorFiles) {
    Remove-Item -LiteralPath $file.FullName -Force -ErrorAction SilentlyContinue
}
foreach ($file in $mseedFiles) {
    Remove-Item -LiteralPath $file.FullName -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Console test data cleared."
Write-Host "cursor.txt deleted: $($cursorFiles.Count)"
Write-Host "mseed files deleted: $($mseedFiles.Count)"
Write-Host "Next start: $ConsoleDir\MseedConsumer.exe"
