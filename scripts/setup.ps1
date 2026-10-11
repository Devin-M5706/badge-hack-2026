# setup.ps1 — one-time repo setup for Windows. Safe to re-run.
#
# Same job as setup.sh: point git at the version-controlled hooks, install the
# commit template, and report whether the toolchain is present.

$ErrorActionPreference = 'Stop'

$repoRoot = git rev-parse --show-toplevel
if (-not $?) { Write-Error 'Not inside a git repository.'; exit 1 }
Set-Location $repoRoot

Write-Host "Setting up $(Split-Path -Leaf $repoRoot)"
Write-Host ''

# ── Git hooks ───────────────────────────────────────────────────────────────
# Note: the hooks are POSIX shell. They run fine on Windows because Git for
# Windows ships its own sh.exe and uses it for hooks — you do not need WSL.
git config core.hooksPath .githooks
Write-Host '  hooks        .githooks (commit-msg, pre-commit)'

git config commit.template .gitmessage
Write-Host '  template     .gitmessage'

git config alias.lg 'log --oneline --graph --decorate -20'
Write-Host '  alias        git lg'

Write-Host ''

# ── Toolchain ───────────────────────────────────────────────────────────────
$pio = Get-Command pio -ErrorAction SilentlyContinue
if ($null -ne $pio) {
  Write-Host "  platformio   $(pio --version)"
}
else {
  $viaPython = $null
  try { $viaPython = python -m platformio --version 2>$null } catch {}
  if ($viaPython) {
    Write-Host "  platformio   $viaPython (via python -m)"
    Write-Host "               'pio' is not on PATH; use 'python -m platformio'"
  }
  else {
    Write-Host '  platformio   NOT FOUND'
    Write-Host ''
    Write-Host '  Install it with:  python -m pip install --user platformio'
    Write-Host '  Then re-run this script.'
  }
}

$clang = Get-Command clang-format -ErrorAction SilentlyContinue
if ($null -ne $clang) {
  Write-Host "  clang-format $(clang-format --version)"
}
else {
  Write-Host '  clang-format not found'
  Write-Host '               install the pinned version with:'
  Write-Host '               python -m pip install --user clang-format==23.1.3'
}

Write-Host ''
Write-Host 'Done. Next:'
Write-Host '  cd firmware; pio run -e badge        # compile'
Write-Host '  cd firmware; pio test -e native      # logic tests on this machine'
Write-Host '  scripts\flash.ps1                    # flash a connected badge'
