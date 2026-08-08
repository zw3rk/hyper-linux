#!/bin/sh
# release.sh — Interactive release workflow for hyper-linux
#
# Prepares release metadata, builds exact artifacts, creates an annotated tag,
# and optionally pushes a DRAFT GitHub release. Publishing is a separate,
# CI-gated operation handled by dist/publish-release.sh.
#
# Usage: dist/release.sh
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=release-lib.sh
. "${SCRIPT_DIR}/release-lib.sh"
cd "$PROJECT"

# ── Helpers ──────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RESET='\033[0m'

info()    { printf "${BLUE}==>${RESET} %s\n" "$1"; }
ok()      { printf "${GREEN}==>${RESET} %s\n" "$1"; }
warn()    { printf "${YELLOW}==>${RESET} %s\n" "$1"; }
err()     { printf "${RED}error:${RESET} %s\n" "$1" >&2; exit 1; }
confirm() { printf "${YELLOW}?${RESET} %s [y/N] " "$1"; read -r ans; [ "$ans" = "y" ] || [ "$ans" = "Y" ]; }

# Portable in-place sed (GNU sed vs macOS sed)
if sed --version 2>/dev/null | grep -q 'GNU'; then
    sedi() { sed -i "$@"; }
else
    sedi() { sed -i '' "$@"; }
fi

# ── Pre-flight checks ───────────────────────────────────────────

command -v git   >/dev/null 2>&1 || err "git not found"
command -v gh    >/dev/null 2>&1 || err "gh CLI not found"

# Must be on master with clean tree
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$BRANCH" != "master" ]; then
    err "Must be on master branch (currently on: $BRANCH)"
fi

if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    err "Working tree is dirty. Commit or stash changes first."
fi

# ── Determine current version ───────────────────────────────────

CURRENT_TAG="$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")"
COMMITS_SINCE="$(git rev-list "${CURRENT_TAG}..HEAD" --count 2>/dev/null || echo "0")"

info "Current version: ${CURRENT_TAG} (${COMMITS_SINCE} commits since)"

if [ "$COMMITS_SINCE" = "0" ]; then
    err "No new commits since ${CURRENT_TAG}. Nothing to release."
fi

# ── Parse current version and show commit summary ────────────────

if ! release_parse_version "$CURRENT_TAG"; then
    err "Cannot parse version '${CURRENT_TAG}' — expected vMAJOR.MINOR.PATCH[-rcN]"
fi

COMMIT_LOG="$(git log "${CURRENT_TAG}..HEAD" --pretty=format:'%s' --no-merges)"

printf "\n"
info "Commit summary (${COMMITS_SINCE} commits since ${CURRENT_TAG}):"
printf "%s\n" "$COMMIT_LOG" | head -20
printf "\n"

# ── Prompt for version ───────────────────────────────────────────

printf '%s\n' "${YELLOW}Select version bump:${RESET}"
if [ -n "$RELEASE_RC_NUMBER" ]; then
    printf '  1) %snext RC%s → v%s\n' "$GREEN" "$RESET" "$(release_next_rc)"
    printf '  2) %sfinal%s   → v%s\n' "$GREEN" "$RESET" "$(release_final_version)"
    printf '  3) patch       → v%s\n' "$(release_next_patch)"
    printf '  4) minor       → v%s\n' "$(release_next_minor)"
    printf '  5) major       → v%s\n' "$(release_next_major)"
    printf '  6) custom\n'
else
    printf '  1) %spatch%s → v%s\n' "$GREEN" "$RESET" "$(release_next_patch)"
    printf '  2) minor    → v%s\n' "$(release_next_minor)"
    printf '  3) major    → v%s\n' "$(release_next_major)"
    printf '  4) custom\n'
fi
printf '%s Choice [1]: ' "${YELLOW}?${RESET}"
read -r CHOICE
CHOICE="${CHOICE:-1}"

if [ -n "$RELEASE_RC_NUMBER" ]; then
    case "$CHOICE" in
        1) NEW_VERSION="$(release_next_rc)" ;;
        2) NEW_VERSION="$(release_final_version)" ;;
        3) NEW_VERSION="$(release_next_patch)" ;;
        4) NEW_VERSION="$(release_next_minor)" ;;
        5) NEW_VERSION="$(release_next_major)" ;;
        6) printf "  Enter version (without v prefix): "; read -r NEW_VERSION ;;
        *) err "Invalid choice" ;;
    esac
else
    case "$CHOICE" in
        1) NEW_VERSION="$(release_next_patch)" ;;
        2) NEW_VERSION="$(release_next_minor)" ;;
        3) NEW_VERSION="$(release_next_major)" ;;
        4) printf "  Enter version (without v prefix): "; read -r NEW_VERSION ;;
        *) err "Invalid choice" ;;
    esac
fi

release_parse_version "$NEW_VERSION" || \
    err "Invalid version '${NEW_VERSION}' — expected MAJOR.MINOR.PATCH[-rcN]"

NEW_TAG="v${NEW_VERSION}"
info "Will release: ${NEW_TAG}"

if git rev-parse "$NEW_TAG" >/dev/null 2>&1; then
    err "Tag ${NEW_TAG} already exists"
fi

# ── Require a curated CHANGELOG entry ────────────────────────────

CHANGELOG_ENTRY="$(awk -v version="$NEW_VERSION" '
    index($0, "## [" version "]") == 1 { capture = 1 }
    capture && /^## \[/ && index($0, "## [" version "]") != 1 { exit }
    capture { print }
' CHANGELOG.md)"
if [ -z "$CHANGELOG_ENTRY" ]; then
    err "Prepare a curated CHANGELOG.md section for ${NEW_VERSION} before releasing"
fi

# ── Update version references (deterministic) ───────────────────

info "Updating version references..."

CURRENT_DATE="$(date '+%B %d, %Y')"
sedi "s/^\.Dd .*/\\.Dd ${CURRENT_DATE}/" hl.1
ok "hl.1 date → ${CURRENT_DATE}"

if [ -f site/install.sh ]; then
    sedi "s/^VERSION=\".*\"/VERSION=\"${NEW_VERSION}\"/" site/install.sh
    ok "site/install.sh VERSION → ${NEW_VERSION}"
fi

if [ -f site/index.html ]; then
    sedi "s/v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\(-rc[0-9][0-9]*\)\{0,1\}/v${NEW_VERSION}/g" site/index.html
    ok "site/index.html versions → v${NEW_VERSION}"
fi

if [ -f Formula/hl.rb ]; then
    sedi "s/^  version \".*\"/  version \"${NEW_VERSION}\"/" Formula/hl.rb
    ok "Formula/hl.rb version → ${NEW_VERSION}"
fi

if [ -f flake.nix ]; then
    # Only need to update the version attribute — buildPhase uses ${version}
    sedi "s/version = \"[^\"]*\"/version = \"${NEW_VERSION}\"/" flake.nix
    ok "flake.nix version → ${NEW_VERSION}"
fi

if [ -f README.md ]; then
    sedi "s/hl-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\(-rc[0-9][0-9]*\)\{0,1\}/hl-v${NEW_VERSION}/g" README.md
    ok "README.md versions → v${NEW_VERSION}"
fi

# ── Show diff and confirm ────────────────────────────────────────

printf "\n"
info "Changes to be committed:"
git diff --stat
printf "\n"
git diff --no-color
printf "\n"

if ! confirm "Commit the release metadata for ${NEW_TAG}?"; then
    warn "Aborted. Changes are unstaged — review and commit manually."
    exit 0
fi

# ── Commit release metadata ──────────────────────────────────────

# Required files — must be staged successfully
git add CHANGELOG.md hl.1
# Optional files — may not exist in all configurations
for f in site/install.sh site/index.html Formula/hl.rb README.md flake.nix; do
    [ -f "$f" ] && git add "$f"
done
git commit -m "release: ${NEW_TAG}

$(echo "$CHANGELOG_ENTRY" | head -20)"

ok "Created release metadata commit for ${NEW_TAG}"

# ── Build release artifacts ──────────────────────────────────────

info "Building release artifacts..."

DIST_OUT="${PROJECT}/dist/out"
DIST_ZIP="$(release_artifact_path "$DIST_OUT" "$NEW_TAG" zip)"
DIST_PKG="$(release_artifact_path "$DIST_OUT" "$NEW_TAG" pkg)"
SHA256SUMS="${DIST_OUT}/SHA256SUMS"
mkdir -p "$DIST_OUT"
rm -f "$DIST_ZIP" "$DIST_PKG" "$SHA256SUMS"

if [ -n "${SIGN_IDENTITY:-}" ] && [ -n "${INSTALLER_SIGN_IDENTITY:-}" ]; then
    sh dist/build-release.sh "$NEW_TAG"
else
    warn "SIGN_IDENTITY / INSTALLER_SIGN_IDENTITY not set — building unsigned"
    make clean
    make hl VERSION="$NEW_TAG" SIGN_IDENTITY=-
    make dist VERSION="$NEW_TAG"
    make pkg VERSION="$NEW_TAG"
fi

if [ ! -f "$DIST_ZIP" ]; then
    err "expected dist zip not found: ${DIST_ZIP}"
fi
if [ ! -f "$DIST_PKG" ]; then
    err "expected dist package not found: ${DIST_PKG}"
fi

ok "Artifacts built:"
ls -lh "$DIST_ZIP" "$DIST_PKG"

# ── Compute SHA256 for Homebrew formula ──────────────────────────

ZIP_SHA256="$(shasum -a 256 "$DIST_ZIP" | cut -d' ' -f1)"
ZIP_BASENAME="$(basename "$DIST_ZIP")"
info "ZIP SHA256: ${ZIP_SHA256}"

# ── Generate SHA256SUMS for release verification ─────────────────

# Run shasum from dist/out/ so output contains bare filenames (no directory prefix).
# This avoids the need for sed, matching the CI approach.
(cd "$DIST_OUT" && shasum -a 256 "$(basename "$DIST_ZIP")" > SHA256SUMS)
(cd "$DIST_OUT" && shasum -a 256 "$(basename "$DIST_PKG")" >> SHA256SUMS)
ok "SHA256SUMS generated"

# Update Formula with final URL and SHA
if [ -f Formula/hl.rb ]; then
    sedi "s|url \".*\"|url \"https://github.com/zw3rk/hyper-linux/releases/download/${NEW_TAG}/${ZIP_BASENAME}\"|" Formula/hl.rb
    sedi "s/sha256 \".*\"/sha256 \"${ZIP_SHA256}\"/" Formula/hl.rb
    ok "Formula/hl.rb URL + SHA256 updated"
    # Amend the release commit with the exact artifact URL and digest.
    git add Formula/hl.rb
    git commit --amend --no-edit
fi

# Tag only after the build and formula update succeed. Never move release tags.
git tag -a "$NEW_TAG" -m "Release ${NEW_TAG}"
ok "Created annotated tag ${NEW_TAG}"

# ── Optional push and DRAFT GitHub Release ───────────────────────

if ! confirm "Push ${NEW_TAG} and create a DRAFT GitHub Release?"; then
    info "Tag created locally. Artifacts in dist/out/. Push when ready:"
    printf '  git push origin master && git push origin %s\n' "$NEW_TAG"
    printf '  gh release create %s --draft ...\n' "$NEW_TAG"
    exit 0
fi

git push origin master
git push origin "$NEW_TAG"
ok "Pushed to origin"

info "Creating DRAFT GitHub Release..."
RELEASE_NOTES="${CHANGELOG_ENTRY}

---

**Install:**
\`\`\`sh
curl -fsSL https://hyper-linux.app/install.sh | sh
\`\`\`
or: \`brew install zw3rk/hyper-linux/hl\` · \`nix run github:zw3rk/hyper-linux\`"

set -- gh release create "$NEW_TAG" "$DIST_ZIP" "$DIST_PKG" "$SHA256SUMS" \
    --draft \
    --title "hyper-linux ${NEW_TAG}" \
    --notes "$RELEASE_NOTES"
if release_is_prerelease; then
    set -- "$@" --prerelease
fi
"$@"

ok "Draft created: https://github.com/zw3rk/hyper-linux/releases/tag/${NEW_TAG}"
warn "The release is NOT public. Do not publish until tag CI is green."
printf '\nNext:\n'
printf '  gh run list --workflow CI --branch %s\n' "$NEW_TAG"
printf '  sh dist/publish-release.sh %s\n' "$NEW_TAG"
