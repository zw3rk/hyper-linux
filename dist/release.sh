#!/bin/sh
# release.sh — Interactive release workflow for hyper-linux
#
# Uses `claude` CLI to generate CHANGELOG entries and validate manpage.
# Prompts for version bump type, creates git tag, updates website/formula,
# builds release artifacts, publishes GitHub Release, and pushes the
# updated Homebrew formula to the tap repo — all in one script.
#
# Usage: dist/release.sh
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$PROJECT"

TAP_REPO="zw3rk/homebrew-hyper-linux"

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
command -v claude >/dev/null 2>&1 || err "claude CLI not found (install: npm i -g @anthropic-ai/claude-code)"

# Allow claude -p to work when release.sh is invoked from within a
# Claude Code session. The CLAUDECODE env var blocks nested sessions,
# but -p (prompt mode) is safe and doesn't share runtime resources.
unset CLAUDECODE 2>/dev/null || true

# Must be on master with clean tree
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$BRANCH" != "master" ]; then
    err "Must be on master branch (currently on: $BRANCH)"
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    err "Working tree is dirty. Commit or stash changes first."
fi

# ── Determine current version ───────────────────────────────────

CURRENT_TAG="$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")"
CURRENT_VERSION="${CURRENT_TAG#v}"
COMMITS_SINCE="$(git rev-list "${CURRENT_TAG}..HEAD" --count 2>/dev/null || echo "0")"

info "Current version: ${CURRENT_TAG} (${COMMITS_SINCE} commits since)"

if [ "$COMMITS_SINCE" = "0" ]; then
    err "No new commits since ${CURRENT_TAG}. Nothing to release."
fi

# ── Parse current version components ─────────────────────────────

MAJOR="$(echo "$CURRENT_VERSION" | cut -d. -f1)"
MINOR="$(echo "$CURRENT_VERSION" | cut -d. -f2)"
PATCH="$(echo "$CURRENT_VERSION" | cut -d. -f3)"

# Validate components are numeric (catches pre-release suffixes like 1.0.0-rc1)
case "${MAJOR}${MINOR}${PATCH}" in
    *[!0-9]*) err "Cannot parse version '${CURRENT_TAG}' — expected vMAJOR.MINOR.PATCH (numeric, no pre-release suffix)" ;;
esac

NEXT_MAJOR="$((MAJOR + 1)).0.0"
NEXT_MINOR="${MAJOR}.$((MINOR + 1)).0"
NEXT_PATCH="${MAJOR}.${MINOR}.$((PATCH + 1))"

# ── Ask claude to suggest version bump type ──────────────────────

info "Analyzing commits to suggest version bump..."

COMMIT_LOG="$(git log "${CURRENT_TAG}..HEAD" --pretty=format:'%s' --no-merges)"

SUGGESTION="$(claude -p "You are a release manager. Given these commits since ${CURRENT_TAG}, suggest whether the next release should be a MAJOR, MINOR, or PATCH bump. Be brief (one word + one sentence reason).

Commits:
${COMMIT_LOG}

Rules:
- MAJOR: breaking API/CLI changes, removed features
- MINOR: new features, new syscalls, significant enhancements
- PATCH: bug fixes, documentation, minor improvements

Respond with exactly: BUMP_TYPE: reason" < /dev/null 2>/dev/null || echo "PATCH: could not analyze")"

printf "\n"
info "Commit summary (${COMMITS_SINCE} commits since ${CURRENT_TAG}):"
printf "%s\n" "$COMMIT_LOG" | head -20
printf "\n"
info "Claude suggests: ${SUGGESTION}"
printf "\n"

# ── Prompt for version ───────────────────────────────────────────

printf '%s\n' "${YELLOW}Select version bump:${RESET}"
printf '  1) %spatch%s  → v%s\n' "$GREEN" "$RESET" "$NEXT_PATCH"
printf '  2) %sminor%s  → v%s\n' "$GREEN" "$RESET" "$NEXT_MINOR"
printf '  3) %smajor%s  → v%s\n' "$GREEN" "$RESET" "$NEXT_MAJOR"
printf '  4) custom\n'
printf '%s Choice [1]: ' "${YELLOW}?${RESET}"
read -r CHOICE
CHOICE="${CHOICE:-1}"

case "$CHOICE" in
    1) NEW_VERSION="$NEXT_PATCH" ;;
    2) NEW_VERSION="$NEXT_MINOR" ;;
    3) NEW_VERSION="$NEXT_MAJOR" ;;
    4) printf "  Enter version (without v prefix): "; read -r NEW_VERSION ;;
    *) err "Invalid choice" ;;
esac

NEW_TAG="v${NEW_VERSION}"
info "Will release: ${NEW_TAG}"

if git rev-parse "$NEW_TAG" >/dev/null 2>&1; then
    err "Tag ${NEW_TAG} already exists"
fi

# ── Generate CHANGELOG entry ─────────────────────────────────────

info "Generating CHANGELOG entry with claude..."

CHANGELOG_ENTRY="$(claude -p "Generate a CHANGELOG entry for hyper-linux version ${NEW_VERSION} (released today).

Previous version: ${CURRENT_TAG}
Commits since last release:
${COMMIT_LOG}

Format:
## [${NEW_VERSION}] - $(date +%Y-%m-%d)

Group changes under these headers (omit empty groups):
### Added
### Changed
### Fixed
### Documentation

Rules:
- One bullet per logical change (combine related commits)
- Start each bullet with a verb (Add, Fix, Change, Update, etc.)
- Be concise but informative
- Include the key technical detail that matters to users
- Do NOT include commit hashes" < /dev/null 2>/dev/null || echo "## [${NEW_VERSION}] - $(date +%Y-%m-%d)

(changelog generation failed — please edit manually)")"

# Write or prepend to CHANGELOG.md
if [ -f CHANGELOG.md ] && [ -s CHANGELOG.md ]; then
    # Prepend new entry after the first line (header)
    TMPFILE="$(mktemp)"
    head -1 CHANGELOG.md > "$TMPFILE"
    printf "\n%s\n" "$CHANGELOG_ENTRY" >> "$TMPFILE"
    tail -n +2 CHANGELOG.md >> "$TMPFILE"
    mv "$TMPFILE" CHANGELOG.md
else
    printf '%s\n\n%s\n' "# Changelog" "$CHANGELOG_ENTRY" > CHANGELOG.md
fi

ok "CHANGELOG.md updated"

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
    sedi "s/v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*/v${NEW_VERSION}/g" site/index.html
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
    sedi "s/hl-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*/hl-v${NEW_VERSION}/g" README.md
    ok "README.md versions → v${NEW_VERSION}"
fi

# ── Update docs to match source (claude, minimal diff) ──────────
#
# Feed claude the commit log plus each doc file. Ask for a unified
# diff that corrects only factual inaccuracies / missing features.
# The prompt is deliberately strict: no style rewrites, no
# re-ordering, no cosmetic changes.

info "Reviewing docs for factual accuracy (claude)..."

# Gather concise source-of-truth context: syscall list from source,
# CLI flags from hl.c, feature list from CLAUDE.md header.
SYSCALL_NAMES="$(grep -h 'case SYS_' src/syscall*.c src/fork_ipc.c src/syscall_fd.c \
    src/syscall_poll.c src/syscall_inotify.c 2>/dev/null \
    | sed 's/.*case SYS_//;s/:.*//' | sort -u | tr '\n' ', ')"
CLI_FLAGS="$(sed -n '/getopt\|struct option/,/};/p' src/hl.c 2>/dev/null)"

DOC_PROMPT="You are reviewing documentation for the hyper-linux (hl) project.
Version being released: ${NEW_VERSION}
Date: $(date +%Y-%m-%d)

Commits since last release (${CURRENT_TAG}):
${COMMIT_LOG}

Syscalls implemented in source (grep 'case SYS_'):
${SYSCALL_NAMES}

CLI option parsing (from src/hl.c):
${CLI_FLAGS}

Key capabilities (from CLAUDE.md / commit history):
- aarch64-linux native + x86_64-linux via Rosetta
- 172 syscalls, multi-threading (64 vCPUs), fork/execve
- COW fork, inotify via kqueue, /proc emulation
- Homebrew: brew install zw3rk/hyper-linux/hl
- curl installer: curl -fsSL https://hyper-linux.app/install.sh | sh
- Signal delivery with rt_sigframe, PI futex, signalfd

RULES — read these carefully:
1. Output ONLY unified diff hunks (--- a/file, +++ b/file, @@ ... @@).
2. Fix ONLY factual errors, missing features, or outdated information.
3. Do NOT rewrite prose style, reorder sections, or add commentary.
4. Do NOT touch formatting, whitespace, or punctuation unless wrong.
5. Keep the diff as small as possible. Fewer changed lines = better.
6. If a file needs no changes, omit it entirely.
7. Output nothing besides the diff. No explanation, no preamble.
"

# Process each doc file individually to keep prompts focused.
for DOC_FILE in hl.1 README.md; do
    [ -f "$DOC_FILE" ] || continue

    info "  checking ${DOC_FILE}..."
    DOC_DIFF="$(claude -p "${DOC_PROMPT}

Here is the current ${DOC_FILE}:

$(cat "$DOC_FILE")

Output a unified diff (--- a/${DOC_FILE} / +++ b/${DOC_FILE}) with
minimal corrections, or output nothing if the file is already accurate." < /dev/null 2>/dev/null || true)"

    # Skip empty or non-diff output
    if [ -z "$DOC_DIFF" ] || ! printf '%s' "$DOC_DIFF" | grep -q '^@@'; then
        ok "  ${DOC_FILE}: no changes needed"
        continue
    fi

    # Apply the diff, allow partial failures
    if printf '%s\n' "$DOC_DIFF" | patch -p1 --no-backup-if-mismatch 2>/dev/null; then
        ok "  ${DOC_FILE}: updated"
    else
        warn "  ${DOC_FILE}: patch partially applied — review manually"
    fi
done

# ── Show diff and confirm ────────────────────────────────────────

printf "\n"
info "Changes to be committed:"
git diff --stat
printf "\n"
git diff --no-color
printf "\n"

if ! confirm "Commit these changes and tag as ${NEW_TAG}?"; then
    warn "Aborted. Changes are unstaged — review and commit manually."
    exit 0
fi

# ── Commit and tag ───────────────────────────────────────────────

# Required files — must be staged successfully
git add CHANGELOG.md hl.1
# Optional files — may not exist in all configurations
for f in site/install.sh site/index.html Formula/hl.rb README.md flake.nix; do
    [ -f "$f" ] && git add "$f"
done
git commit -m "release: ${NEW_TAG}

$(echo "$CHANGELOG_ENTRY" | head -20)"

git tag -a "$NEW_TAG" -m "Release ${NEW_TAG}"

ok "Created commit and tag ${NEW_TAG}"

# ── Build release artifacts ──────────────────────────────────────

info "Building release artifacts..."

if [ -n "${SIGN_IDENTITY:-}" ] && [ -n "${INSTALLER_SIGN_IDENTITY:-}" ]; then
    sh dist/build-release.sh "$NEW_TAG"
else
    warn "SIGN_IDENTITY / INSTALLER_SIGN_IDENTITY not set — building unsigned"
    make clean
    make hl VERSION="$NEW_TAG" SIGN_IDENTITY=-
    make dist VERSION="$NEW_TAG"
    make pkg VERSION="$NEW_TAG"
fi

# Locate the artifacts
DIST_ZIP="$(find dist/out -maxdepth 1 -name 'hl-*.zip' -print | head -1)"
DIST_PKG="$(find dist/out -maxdepth 1 -name 'hl-*.pkg' -print | head -1)"

if [ -z "$DIST_ZIP" ]; then
    err "dist zip not found in dist/out/"
fi

ok "Artifacts built:"
if [ -n "$DIST_PKG" ] && [ -f "$DIST_PKG" ]; then
    ls -lh "$DIST_ZIP" "$DIST_PKG"
else
    ls -lh "$DIST_ZIP"
fi

# ── Compute SHA256 for Homebrew formula ──────────────────────────

ZIP_SHA256="$(shasum -a 256 "$DIST_ZIP" | cut -d' ' -f1)"
ZIP_BASENAME="$(basename "$DIST_ZIP")"
info "ZIP SHA256: ${ZIP_SHA256}"

# ── Generate SHA256SUMS for release verification ─────────────────

SHA256SUMS="${PROJECT}/dist/out/SHA256SUMS"
# Run shasum from dist/out/ so output contains bare filenames (no directory prefix).
# This avoids the need for sed, matching the CI approach.
(cd "${PROJECT}/dist/out" && shasum -a 256 "$(basename "$DIST_ZIP")" > SHA256SUMS)
if [ -n "$DIST_PKG" ] && [ -f "$DIST_PKG" ]; then
    (cd "${PROJECT}/dist/out" && shasum -a 256 "$(basename "$DIST_PKG")" >> SHA256SUMS)
fi
ok "SHA256SUMS generated"

# Update Formula with final URL and SHA
if [ -f Formula/hl.rb ]; then
    sedi "s|url \".*\"|url \"https://github.com/zw3rk/hyper-linux/releases/download/${NEW_TAG}/${ZIP_BASENAME}\"|" Formula/hl.rb
    sedi "s/sha256 \".*\"/sha256 \"${ZIP_SHA256}\"/" Formula/hl.rb
    ok "Formula/hl.rb URL + SHA256 updated"
    # Amend the release commit with the final formula
    git add Formula/hl.rb
    git commit --amend --no-edit
    # Re-tag (move the tag to the amended commit)
    git tag -f -a "$NEW_TAG" -m "Release ${NEW_TAG}"
fi

# ── Push and create GitHub Release ───────────────────────────────

if ! confirm "Push ${NEW_TAG} and create GitHub Release?"; then
    info "Tag created locally. Artifacts in dist/out/. Push when ready:"
    printf '  git push origin master && git push origin %s\n' "$NEW_TAG"
    exit 0
fi

git push origin master
git push origin "$NEW_TAG"
ok "Pushed to origin"

info "Creating GitHub Release..."
RELEASE_NOTES="${CHANGELOG_ENTRY}

---

**Install:**
\`\`\`sh
curl -fsSL https://hyper-linux.app/install.sh | sh
\`\`\`
or: \`brew install zw3rk/hyper-linux/hl\` · \`nix run github:zw3rk/hyper-linux\`"

if [ -n "$DIST_PKG" ] && [ -f "$DIST_PKG" ]; then
    gh release create "$NEW_TAG" "$DIST_ZIP" "$DIST_PKG" "$SHA256SUMS" \
        --title "hyper-linux ${NEW_TAG}" \
        --notes "$RELEASE_NOTES"
else
    gh release create "$NEW_TAG" "$DIST_ZIP" "$SHA256SUMS" \
        --title "hyper-linux ${NEW_TAG}" \
        --notes "$RELEASE_NOTES"
fi

ok "GitHub Release created: https://github.com/zw3rk/hyper-linux/releases/tag/${NEW_TAG}"

# ── Update Homebrew tap ──────────────────────────────────────────

info "Updating Homebrew tap (${TAP_REPO})..."

TAP_DIR="$(mktemp -d)"
trap 'rm -rf "$TAP_DIR"' EXIT

if gh repo clone "$TAP_REPO" "$TAP_DIR/tap" -- --depth=1 2>/dev/null; then
    mkdir -p "$TAP_DIR/tap/Formula"
    cp Formula/hl.rb "$TAP_DIR/tap/Formula/hl.rb"
    cd "$TAP_DIR/tap"
    git add Formula/hl.rb
    if ! git diff --cached --quiet; then
        git commit -m "hl ${NEW_TAG}

url: https://github.com/zw3rk/hyper-linux/releases/download/${NEW_TAG}/${ZIP_BASENAME}
sha256: ${ZIP_SHA256}"
        TAP_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
        if [ "$TAP_BRANCH" != "main" ] && [ "$TAP_BRANCH" != "master" ]; then
            warn "Tap repo default branch is '${TAP_BRANCH}' — expected main or master"
        fi
        git push origin "$TAP_BRANCH"
        ok "Homebrew tap updated"
    else
        ok "Homebrew tap already up-to-date"
    fi
    cd "$PROJECT"
else
    warn "Could not clone ${TAP_REPO} — skipping tap update"
    warn "Create it first: sh dist/homebrew-tap/setup.sh"
    warn "Then manually copy Formula/hl.rb into the tap repo"
fi

# ── Verify install methods ───────────────────────────────────────

printf "\n"
ok "Release ${NEW_TAG} complete!"
printf "\n"
info "Verify installation methods:"
printf "  curl -fsSL https://hyper-linux.app/install.sh | sh\n"
printf "  brew install zw3rk/hyper-linux/hl\n"
printf "  nix run github:zw3rk/hyper-linux\n"
printf "\n"
