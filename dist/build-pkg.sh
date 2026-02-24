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
BINARY="$2"
MANPAGE="$3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/out"
STAGING="${SCRIPT_DIR}/staging/pkg"
PKG_ID="com.zw3rk.hl"

# Verify inputs exist
for f in "$BINARY" "$MANPAGE"; do
    if [ ! -f "$f" ]; then
        echo "Error: $f not found" >&2
        exit 1
    fi
done

# Clean and create staging tree
rm -rf "$STAGING"
mkdir -p "${STAGING}/usr/local/bin"
mkdir -p "${STAGING}/usr/local/share/man/man1"
mkdir -p "$OUT_DIR"

# Install files into payload
cp "$BINARY" "${STAGING}/usr/local/bin/hl"
chmod 755 "${STAGING}/usr/local/bin/hl"
cp "$MANPAGE" "${STAGING}/usr/local/share/man/man1/hl.1"
chmod 644 "${STAGING}/usr/local/share/man/man1/hl.1"

# Build unsigned package
UNSIGNED="${SCRIPT_DIR}/staging/hl-${VERSION}-unsigned.pkg"
echo "Building unsigned .pkg ..."
pkgbuild \
    --root "$STAGING" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
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
rm -rf "$STAGING"
