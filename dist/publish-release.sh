#!/bin/sh
# Publish a prepared draft only after its exact tag CI has succeeded.

# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
PROJECT=$(CDPATH='' cd -- "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=release-lib.sh
. "${SCRIPT_DIR}/release-lib.sh"

REPO=zw3rk/hyper-linux
TAP_REPO=zw3rk/homebrew-hyper-linux
TAG=${1-}

err() { printf 'error: %s\n' "$1" >&2; exit 1; }
info() { printf '==> %s\n' "$1"; }
warn() { printf 'warning: %s\n' "$1" >&2; }
confirm() {
    printf '? %s [y/N] ' "$1"
    read -r answer
    [ "$answer" = y ] || [ "$answer" = Y ]
}

[ -n "$TAG" ] || err "usage: $0 vMAJOR.MINOR.PATCH[-rcN]"
release_parse_version "$TAG" || err "invalid release tag: $TAG"
case $TAG in v*) ;; *) err "release tag must start with v" ;; esac

command -v git >/dev/null 2>&1 || err "git not found"
command -v gh >/dev/null 2>&1 || err "gh not found"
command -v cmp >/dev/null 2>&1 || err "cmp not found"

cd "$PROJECT"
LOCAL_COMMIT=$(git rev-parse "${TAG}^{commit}" 2>/dev/null) || \
    err "local tag not found: $TAG"
REMOTE_COMMIT=$(git ls-remote origin "refs/tags/${TAG}^{}" | awk 'NR == 1 { print $1 }')
if [ -z "$REMOTE_COMMIT" ]; then
    REMOTE_COMMIT=$(git ls-remote origin "refs/tags/${TAG}" | awk 'NR == 1 { print $1 }')
fi
[ "$LOCAL_COMMIT" = "$REMOTE_COMMIT" ] || \
    err "local tag commit ${LOCAL_COMMIT} does not match remote ${REMOTE_COMMIT:-missing}"

CI_ROW=$(gh run list --repo "$REPO" --workflow CI --branch "$TAG" --limit 1 \
    --json headSha,status,conclusion,url \
    --jq '.[0] | [.headSha, .status, .conclusion, .url] | @tsv')
[ -n "$CI_ROW" ] || err "no CI run found for $TAG"
tab=$(printf '\t')
old_ifs=$IFS
IFS=$tab
set -- $CI_ROW
IFS=$old_ifs
CI_SHA=${1-}
CI_STATUS=${2-}
CI_CONCLUSION=${3-}
CI_URL=${4-}
[ "$CI_SHA" = "$LOCAL_COMMIT" ] || \
    err "latest tag CI is for ${CI_SHA:-unknown}, expected $LOCAL_COMMIT"
[ "$CI_STATUS" = completed ] || err "tag CI is not complete: $CI_URL"
[ "$CI_CONCLUSION" = success ] || err "tag CI did not succeed: $CI_URL"
info "Tag CI succeeded: $CI_URL"

IS_DRAFT=$(gh release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft)
[ "$IS_DRAFT" = true ] || err "release $TAG is missing or is already public"

DIST_OUT="${PROJECT}/dist/out"
DIST_ZIP=$(release_artifact_path "$DIST_OUT" "$TAG" zip)
DIST_PKG=$(release_artifact_path "$DIST_OUT" "$TAG" pkg)
DIST_SUMS="${DIST_OUT}/SHA256SUMS"
[ -f "$DIST_ZIP" ] || err "missing local release artifact: $DIST_ZIP"
[ -f "$DIST_PKG" ] || err "missing local release artifact: $DIST_PKG"
[ -f "$DIST_SUMS" ] || err "missing local release artifact: $DIST_SUMS"
ZIP_BASENAME=$(basename "$DIST_ZIP")
PKG_BASENAME=$(basename "$DIST_PKG")
ZIP_SHA256=$(shasum -a 256 "$DIST_ZIP" | awk '{ print $1 }')
FORMULA_URL=$(sed -n 's/^  url "\([^"]*\)"/\1/p' Formula/hl.rb)
FORMULA_SHA256=$(sed -n 's/^  sha256 "\([^"]*\)"/\1/p' Formula/hl.rb)
EXPECTED_URL="https://github.com/${REPO}/releases/download/${TAG}/${ZIP_BASENAME}"
[ "$FORMULA_URL" = "$EXPECTED_URL" ] || err "Formula URL does not match $TAG"
[ "$FORMULA_SHA256" = "$ZIP_SHA256" ] || err "Formula digest does not match $DIST_ZIP"

ASSETS=$(gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets[].name')
EXPECTED_ASSETS=$(printf '%s\n' "$ZIP_BASENAME" "$PKG_BASENAME" SHA256SUMS | \
    LC_ALL=C sort)
ACTUAL_ASSETS=$(printf '%s\n' "$ASSETS" | sed '/^$/d' | LC_ALL=C sort)
[ "$ACTUAL_ASSETS" = "$EXPECTED_ASSETS" ] || {
    printf 'error: draft asset set does not match\nexpected:\n%s\nactual:\n%s\n' \
        "$EXPECTED_ASSETS" "$ACTUAL_ASSETS" >&2
    exit 1
}

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hl-publish.XXXXXX")
ASSET_DIR="${WORK_DIR}/assets"
TAP_DIR="${WORK_DIR}/tap"
TAP_UPDATED=0
RELEASE_PUBLISHED=0
TAP_BRANCH=

rollback_tap() {
    cd "$TAP_DIR"
    git revert --no-edit HEAD && git push origin "$TAP_BRANCH"
}

cleanup() {
    cleanup_status=$?
    trap - 0 HUP INT TERM
    if [ "$TAP_UPDATED" = 1 ] && [ "$RELEASE_PUBLISHED" = 0 ]; then
        if rollback_tap; then
            warn "release was not published; reverted the Homebrew tap update"
        else
            warn "HOMEBREW TAP ROLLBACK FAILED; revert the last tap commit manually"
        fi
    fi
    cd "$PROJECT" || :
    rm -rf "$WORK_DIR"
    exit "$cleanup_status"
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$ASSET_DIR"
LOCAL_SUMS="${WORK_DIR}/local-SHA256SUMS"
(cd "$DIST_OUT" && shasum -a 256 "$ZIP_BASENAME" "$PKG_BASENAME") > \
    "$LOCAL_SUMS"
cmp -s "$LOCAL_SUMS" "$DIST_SUMS" || \
    err "local SHA256SUMS does not match the exact ZIP and PKG"

gh release download "$TAG" --repo "$REPO" --dir "$ASSET_DIR" \
    --pattern "$ZIP_BASENAME" --pattern "$PKG_BASENAME" \
    --pattern SHA256SUMS
for asset in "$ZIP_BASENAME" "$PKG_BASENAME" SHA256SUMS; do
    [ -f "$ASSET_DIR/$asset" ] || err "downloaded draft is missing $asset"
    cmp -s "$DIST_OUT/$asset" "$ASSET_DIR/$asset" || \
        err "draft asset $asset does not match local release artifact"
done
info "Draft assets match local ZIP, PKG and SHA256SUMS byte-for-byte"

printf '\nRelease: %s\nCommit:  %s\nCI:      %s\nArtifact: %s\n\n' \
    "$TAG" "$LOCAL_COMMIT" "$CI_URL" "$ZIP_BASENAME"
if ! confirm "Update the Homebrew tap and publish this release?"; then
    info "aborted; draft remains private"
    exit 0
fi

gh repo clone "$TAP_REPO" "$TAP_DIR" -- --depth=1
mkdir -p "$TAP_DIR/Formula"
cp Formula/hl.rb "$TAP_DIR/Formula/hl.rb"
cd "$TAP_DIR"
git add Formula/hl.rb
if ! git diff --cached --quiet; then
    git commit -m "hl ${TAG}"
    TAP_BRANCH=$(git rev-parse --abbrev-ref HEAD)
    git push origin "$TAP_BRANCH"
    TAP_UPDATED=1
else
    info "Homebrew tap already contains the exact formula"
fi

if release_is_prerelease; then
    set -- gh release edit "$TAG" --repo "$REPO" --draft=false --prerelease
else
    set -- gh release edit "$TAG" --repo "$REPO" --draft=false --prerelease=false
fi
if "$@"; then
    RELEASE_PUBLISHED=1
else
    err "failed to publish $TAG; Homebrew tap rollback will be attempted"
fi
info "published https://github.com/${REPO}/releases/tag/${TAG}"
