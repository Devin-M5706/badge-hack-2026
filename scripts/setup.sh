#!/bin/sh
# setup.sh — one-time repo setup. Safe to re-run.
#
# Everything here is local git config plus a dependency check. It installs
# nothing globally except, optionally, PlatformIO when you say yes.

set -e

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "Not inside a git repository." >&2
  exit 1
}
cd "$repo_root"

echo "Setting up $(basename "$repo_root")"
echo

# ── Git hooks ──────────────────────────────────────────────────────────────
# core.hooksPath points git at version-controlled hooks, so every clone gets
# them. The alternative (copying into .git/hooks) silently rots.
git config core.hooksPath .githooks
chmod +x .githooks/* 2>/dev/null || true
echo "  hooks        .githooks (commit-msg, pre-commit)"

git config commit.template .gitmessage
echo "  template     .gitmessage"

# Makes `git log` readable without arguments.
git config alias.lg "log --oneline --graph --decorate -20"
echo "  alias        git lg"

echo

# ── Toolchain ──────────────────────────────────────────────────────────────
if command -v pio >/dev/null 2>&1; then
  echo "  platformio   $(pio --version)"
elif python -m platformio --version >/dev/null 2>&1; then
  echo "  platformio   $(python -m platformio --version) (via python -m)"
  echo "               'pio' is not on PATH; use 'python -m platformio' instead"
else
  echo "  platformio   NOT FOUND"
  echo
  echo "  Install it with:  python -m pip install --user platformio"
  echo "  Then re-run this script."
fi

if command -v clang-format >/dev/null 2>&1; then
  echo "  clang-format $(clang-format --version)"
else
  echo "  clang-format not found"
  echo "               install the pinned version with:"
  echo "               python -m pip install --user clang-format==23.1.3"
fi

echo
echo "Done. Next:"
echo "  cd firmware && pio run -e badge        # compile"
echo "  cd firmware && pio test -e native      # run logic tests on this machine"
echo "  scripts/flash.sh                       # flash a connected badge"
