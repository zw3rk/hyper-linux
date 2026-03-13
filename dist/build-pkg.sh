#!/bin/sh
# build-pkg.sh — Build a macOS .pkg installer for hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Usage: dist/build-pkg.sh <version> <binary-path> <man-page-path>
#
# Environment:
#   INSTALLER_SIGN_IDENTITY  Developer ID Installer identity (optional)
#
# Output: dist/out/hl-<version>.pkg

set -eu

if [ $# -lt 3 ]; then
    echo "Usage: $0 <version> <binary-path> <man-page-path>" >&2
    exit 1
fi

VERSION="$1"
# Strip leading 'v' from version for pkgbuild (expects bare semver)
PKG_VERSION="${VERSION#v}"
BINARY="$2"
MANPAGE="$3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${SCRIPT_DIR}/out"
STAGING="${SCRIPT_DIR}/staging/pkg"
SCRIPTS="${SCRIPT_DIR}/staging/scripts"
PKG_ID="com.zw3rk.hl"

# Verify inputs exist
for f in "$BINARY" "$MANPAGE"; do
    if [ ! -f "$f" ]; then
        echo "Error: $f not found" >&2
        exit 1
    fi
done

# Clean and create staging tree
rm -rf "$STAGING" "$SCRIPTS"
mkdir -p "${STAGING}/usr/local/bin"
mkdir -p "${STAGING}/usr/local/share/man/man1"
mkdir -p "${STAGING}/usr/local/share/hl"
mkdir -p "$SCRIPTS"
mkdir -p "$OUT_DIR"

# Install files into payload
cp "$BINARY" "${STAGING}/usr/local/bin/hl"
chmod 755 "${STAGING}/usr/local/bin/hl"
cp "$MANPAGE" "${STAGING}/usr/local/share/man/man1/hl.1"
chmod 644 "${STAGING}/usr/local/share/man/man1/hl.1"
# Include entitlements.plist for manual re-signing
cp "${PROJECT_DIR}/entitlements.plist" "${STAGING}/usr/local/share/hl/entitlements.plist"
chmod 644 "${STAGING}/usr/local/share/hl/entitlements.plist"

# Post-install script: codesign binary with Hypervisor.framework entitlement
cat > "${SCRIPTS}/postinstall" << 'POSTINSTALL'
#!/bin/sh
# Ad-hoc codesign with Hypervisor.framework entitlement.
# Must run on the end-user machine (ad-hoc signatures are local).
codesign --entitlements /usr/local/share/hl/entitlements.plist \
         -f -s - /usr/local/bin/hl 2>/dev/null || true
POSTINSTALL
chmod 755 "${SCRIPTS}/postinstall"

# Build unsigned package
UNSIGNED="${SCRIPT_DIR}/staging/hl-${VERSION}-unsigned.pkg"
echo "Building unsigned .pkg ..."
pkgbuild \
    --root "$STAGING" \
    --scripts "$SCRIPTS" \
    --identifier "$PKG_ID" \
    --version "$PKG_VERSION" \
    --install-location / \
    "$UNSIGNED"

# Sign if identity is available
OUTPUT="${OUT_DIR}/hl-${VERSION}.pkg"
if [ -n "${INSTALLER_SIGN_IDENTITY:-}" ]; then
    echo "Signing .pkg with: ${INSTALLER_SIGN_IDENTITY}"
    productsign \
        --sign "$INSTALLER_SIGN_IDENTITY" \
        --timestamp \
        "$UNSIGNED" "$OUTPUT"
    rm -f "$UNSIGNED"
else
    echo "No INSTALLER_SIGN_IDENTITY set — .pkg is unsigned"
    mv "$UNSIGNED" "$OUTPUT"
fi

echo "Package: ${OUTPUT}"

# Clean up staging
rm -rf "$STAGING" "$SCRIPTS"
