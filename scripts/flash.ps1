# flash.ps1 — build and upload firmware to the badge, with the one piece of
# recovery advice you will actually need.
#
# Usage:
#   scripts\flash.ps1              # auto-detect port
#   scripts\flash.ps1 -Port COM7   # pick one
#   scripts\flash.ps1 -Monitor     # open the serial monitor after flashing

param(
  [string]$Port,
  [switch]$Monitor
)

$ErrorActionPreference = 'Continue'

$repoRoot = git rev-parse --show-toplevel
if (-not $?) { Write-Error 'Not inside a git repository.'; exit 1 }
Set-Location (Join-Path $repoRoot 'firmware')

# Prefer the pio on PATH; fall back to the module, since pip --user installs
# often do not add Scripts\ to PATH on Windows.
if (Get-Command pio -ErrorAction SilentlyContinue) {
  $pioCmd = @('pio')
}
else {
  $pioCmd = @('python', '-m', 'platformio')
}

$pioArgs = @('run', '-e', 'badge', '-t', 'upload')
if ($Port) { $pioArgs += @('--upload-port', $Port) }

Write-Host "Flashing badge..." -ForegroundColor Cyan
& $pioCmd[0] ($pioCmd[1..($pioCmd.Length - 1)] + $pioArgs)
$uploadExit = $LASTEXITCODE

if ($uploadExit -ne 0) {
  Write-Host ''
  Write-Host '─────────────────────────────────────────────────────────' -ForegroundColor Yellow
  Write-Host ' Upload failed. Before assuming the badge is bricked:' -ForegroundColor Yellow
  Write-Host '─────────────────────────────────────────────────────────' -ForegroundColor Yellow
  Write-Host ''
  Write-Host ' If you saw "No serial data received", enter download mode:'
  Write-Host ''
  Write-Host '   1. Unplug USB-C.'
  Write-Host '   2. HOLD the Start button down.'
  Write-Host '   3. Plug USB-C back in, still holding Start.'
  Write-Host '   4. Release, then re-run this script.'
  Write-Host ''
  Write-Host ' A BLANK SCREEN in this state is expected — that is download' -ForegroundColor Green
  Write-Host ' mode, not a brick. Start (GPIO9) is a strapping pin, and this'
  Write-Host ' board has native USB with no auto-reset, so the manual hold is'
  Write-Host ' the normal procedure rather than a workaround. (HAL guide 1.4)'
  Write-Host ''
  Write-Host ' Still nothing? Check that no serial monitor owns the port —'
  Write-Host ' only one process can hold it at a time. (HAL guide 1.5)'
  Write-Host ''
  exit $uploadExit
}

Write-Host ''
Write-Host 'Flashed.' -ForegroundColor Green

if ($Monitor) {
  $monArgs = @('device', 'monitor', '-b', '115200')
  if ($Port) { $monArgs += @('-p', $Port) }
  & $pioCmd[0] ($pioCmd[1..($pioCmd.Length - 1)] + $monArgs)
}
else {
  Write-Host 'Serial monitor:  cd firmware; pio device monitor -b 115200'
  Write-Host 'Then type `help` for the debug console.'
}
