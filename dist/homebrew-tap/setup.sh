#!/bin/sh
# setup.sh — Create the zw3rk/homebrew-hyper-linux tap repository
#
# This script creates the Homebrew tap repo on GitHub and pushes the
# initial formula. Run once to set up the tap.
#
# Prerequisites:
#   - gh CLI authenticated with repo creation permissions
#   - Formula/hl.rb exists in the hyper-linux repo
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

info()  { printf '\033[0;34m==>\033[0m %s\n' "$1"; }
ok()    { printf '\033[0;32m==>\033[0m %s\n' "$1"; }
err()   { printf '\033[0;31merror:\033[0m %s\n' "$1" >&2; exit 1; }

# ── Pre-flight ───────────────────────────────────────────────────

command -v gh >/dev/null 2>&1 || err "gh CLI not found"
test -f "${PROJECT}/Formula/hl.rb" || err "Formula/hl.rb not found in project"

# Check if repo already exists
if gh repo view zw3rk/homebrew-hyper-linux >/dev/null 2>&1; then
    info "Repository zw3rk/homebrew-hyper-linux already exists"
    printf "  Update formula? [y/N] "
    read -r ans
    [ "$ans" = "y" ] || [ "$ans" = "Y" ] || exit 0
    UPDATE_ONLY=1
else
    UPDATE_ONLY=0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ "$UPDATE_ONLY" = "0" ]; then
    # ── Create the tap repo ──────────────────────────────────────

    info "Creating zw3rk/homebrew-hyper-linux..."
    gh repo create zw3rk/homebrew-hyper-linux \
        --public \
        --description "Homebrew tap for hyper-linux (hl) — Run Linux binaries on macOS" \
        --clone="$TMPDIR/homebrew-hyper-linux"

    cd "$TMPDIR/homebrew-hyper-linux"
else
    # ── Clone existing repo ──────────────────────────────────────

    info "Cloning zw3rk/homebrew-hyper-linux..."
    gh repo clone zw3rk/homebrew-hyper-linux "$TMPDIR/homebrew-hyper-linux"
    cd "$TMPDIR/homebrew-hyper-linux"
fi

# ── Set up formula ───────────────────────────────────────────────

mkdir -p Formula
cp "${PROJECT}/Formula/hl.rb" Formula/hl.rb

# ── Commit and push ──────────────────────────────────────────────

git add Formula/hl.rb
if git diff --cached --quiet; then
    ok "Formula is already up-to-date"
else
    git commit -m "Update hl formula to $(grep 'version "' Formula/hl.rb | head -1 | sed 's/.*"\(.*\)".*/\1/')"
    git push origin "$(git rev-parse --abbrev-ref HEAD)"
    ok "Formula pushed to zw3rk/homebrew-hyper-linux"
fi

# ── Verify ───────────────────────────────────────────────────────

printf "\n"
ok "Homebrew tap is ready!"
printf "  Install:  brew install zw3rk/hyper-linux/hl\n"
printf "  Update:   brew upgrade hl\n"
printf "  Untap:    brew untap zw3rk/hyper-linux\n"
printf "\n"
info "Remember to add HOMEBREW_TAP_TOKEN to GitHub secrets"
printf "  (PAT with 'repo' scope for auto-updating formula on release)\n"
