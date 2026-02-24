#!/bin/sh
# build-release.sh — Build a signed, notarized release of hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Usage: dist/build-release.sh [<version>]
#
# Required environment:
#   SIGN_IDENTITY               Developer ID Application identity (binary)
#   INSTALLER_SIGN_IDENTITY     Developer ID Installer identity (.pkg)
#
# Optional environment (for notarization):
#   NOTARY_APPLE_ID             Apple ID email
#   NOTARY_PASSWORD             App-specific password or keychain profile
#   NOTARY_TEAM_ID              Developer team ID

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${SCRIPT_DIR}/out"
STAGING="${SCRIPT_DIR}/staging"

# ── Version ──────────────────────────────────────────────────────

VERSION="${1:-$(cd "$PROJECT" && git describe --tags --always --dirty 2>/dev/null || echo "unknown")}"
echo "=== Building hl ${VERSION} ==="

# ── Validate environment ─────────────────────────────────────────

if [ -z "${SIGN_IDENTITY:-}" ]; then
    echo "Error: SIGN_IDENTITY not set" >&2
    echo "  export SIGN_IDENTITY='Developer ID Application: ...'" >&2
    exit 1
fi

if [ -z "${INSTALLER_SIGN_IDENTITY:-}" ]; then
    echo "Error: INSTALLER_SIGN_IDENTITY not set" >&2
    echo "  export INSTALLER_SIGN_IDENTITY='Developer ID Installer: ...'" >&2
    exit 1
fi

# ── Step 1: Build ─────────────────────────────────────────────────

echo "--- Building hl ---"
make -C "$PROJECT" clean
make -C "$PROJECT" hl VERSION="$VERSION" SIGN_IDENTITY=-

# ── Step 2: Sign binary ──────────────────────────────────────────

BINARY="${PROJECT}/_build/hl"
echo "--- Signing binary with: ${SIGN_IDENTITY} ---"
codesign --force \
    --sign "$SIGN_IDENTITY" \
    --timestamp \
    --options runtime \
    --entitlements "${PROJECT}/entitlements.plist" \
    "$BINARY"

echo "--- Verifying signature ---"
codesign --verify --verbose=2 "$BINARY"

# ── Step 3: Assemble zip staging directory ────────────────────────

DIST_NAME="hl-${VERSION}"
ZIP_STAGING="${STAGING}/${DIST_NAME}"
rm -rf "$ZIP_STAGING"
mkdir -p "$ZIP_STAGING"
mkdir -p "$OUT_DIR"

cp "$BINARY"                              "${ZIP_STAGING}/hl"
cp "${PROJECT}/hl.1"                      "${ZIP_STAGING}/hl.1"
cp "${SCRIPT_DIR}/configure"              "${ZIP_STAGING}/configure"
cp "${SCRIPT_DIR}/Makefile.install"       "${ZIP_STAGING}/Makefile"
cp "${SCRIPT_DIR}/README"                 "${ZIP_STAGING}/README"
cp "${PROJECT}/LICENSE"                   "${ZIP_STAGING}/LICENSE"
cp "${PROJECT}/entitlements.plist"        "${ZIP_STAGING}/entitlements.plist"

chmod 755 "${ZIP_STAGING}/hl"
chmod 755 "${ZIP_STAGING}/configure"

# ── Step 4: Create zip (ditto preserves code signatures) ──────────

ZIP_PATH="${OUT_DIR}/${DIST_NAME}.zip"
echo "--- Creating ${ZIP_PATH} ---"
cd "$STAGING"
ditto -c -k --keepParent "$DIST_NAME" "$ZIP_PATH"
cd "$PROJECT"

# ── Step 5: Build .pkg ───────────────────────────────────────────

echo "--- Building .pkg ---"
sh "${SCRIPT_DIR}/build-pkg.sh" "$VERSION" "$BINARY" "${PROJECT}/hl.1"

# ── Step 6: Notarize (if credentials available) ──────────────────

PKG_PATH="${OUT_DIR}/${DIST_NAME}.pkg"

if [ -n "${NOTARY_APPLE_ID:-}" ] && \
   [ -n "${NOTARY_PASSWORD:-}" ] && \
   [ -n "${NOTARY_TEAM_ID:-}" ]; then

    echo "--- Notarizing zip ---"
    xcrun notarytool submit "$ZIP_PATH" \
        --apple-id "$NOTARY_APPLE_ID" \
        --password "$NOTARY_PASSWORD" \
        --team-id "$NOTARY_TEAM_ID" \
        --wait

    echo "--- Notarizing .pkg ---"
    xcrun notarytool submit "$PKG_PATH" \
        --apple-id "$NOTARY_APPLE_ID" \
        --password "$NOTARY_PASSWORD" \
        --team-id "$NOTARY_TEAM_ID" \
        --wait

    # Staple the .pkg (standalone Mach-O cannot be stapled)
    echo "--- Stapling .pkg ---"
    xcrun stapler staple "$PKG_PATH"
else
    echo "--- Skipping notarization (NOTARY_* vars not set) ---"
fi

# ── Step 7: Clean up staging ─────────────────────────────────────

rm -rf "$STAGING"

# ── Done ──────────────────────────────────────────────────────────

echo ""
echo "=== Release artifacts ==="
echo "  ${ZIP_PATH}"
echo "  ${PKG_PATH}"
ls -lh "$ZIP_PATH" "$PKG_PATH"
