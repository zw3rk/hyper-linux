#!/bin/sh
# hyper-linux installer — https://hyper-linux.app
#
# Usage: curl -fsSL https://hyper-linux.app/install.sh | sh
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -e

VERSION="0.1.1"
REPO="zw3rk/hyper-linux"
INSTALL_DIR="/usr/local/bin"
MAN_DIR="/usr/local/share/man/man1"

# ── Helpers ──────────────────────────────────────────────────────

info()  { printf '\033[0;34m==>\033[0m %s\n' "$1"; }
ok()    { printf '\033[0;32m==>\033[0m %s\n' "$1"; }
err()   { printf '\033[0;31merror:\033[0m %s\n' "$1" >&2; exit 1; }

# ── Pre-flight checks ───────────────────────────────────────────

# Apple Silicon only
arch=$(uname -m)
if [ "$arch" != "arm64" ]; then
    err "hyper-linux requires Apple Silicon (arm64). Detected: $arch"
fi

# macOS only
os=$(uname -s)
if [ "$os" != "Darwin" ]; then
    err "hyper-linux requires macOS. Detected: $os"
fi

info "Installing hyper-linux v${VERSION}..."

# ── Download ─────────────────────────────────────────────────────

ASSET_NAME="hl-unknown.zip"
URL="https://github.com/${REPO}/releases/download/v${VERSION}/${ASSET_NAME}"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

info "Downloading from ${URL}..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "${tmpdir}/${ASSET_NAME}" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${tmpdir}/${ASSET_NAME}" "$URL"
else
    err "Neither curl nor wget found. Install one and retry."
fi

# ── Extract ──────────────────────────────────────────────────────

info "Extracting..."
# The zip contains a directory (hl-unknown/) with hl, hl.1, entitlements.plist, etc.
unzip -qo "${tmpdir}/${ASSET_NAME}" -d "${tmpdir}"

# Find the extracted directory
extracted=$(find "$tmpdir" -maxdepth 1 -type d -name 'hl-*' | head -1)
if [ -z "$extracted" ]; then
    err "Unexpected archive layout — no hl-* directory found."
fi

# ── Codesign ─────────────────────────────────────────────────────

info "Codesigning with Hypervisor.framework entitlement..."
codesign --entitlements "${extracted}/entitlements.plist" \
         -f -s - "${extracted}/hl"

# ── Install ──────────────────────────────────────────────────────

info "Installing to ${INSTALL_DIR}/hl ..."
if [ -w "$INSTALL_DIR" ]; then
    cp "${extracted}/hl" "${INSTALL_DIR}/hl"
    chmod 755 "${INSTALL_DIR}/hl"
else
    sudo cp "${extracted}/hl" "${INSTALL_DIR}/hl"
    sudo chmod 755 "${INSTALL_DIR}/hl"
fi

# Man page (best effort — don't fail if directory doesn't exist)
if [ -d "$MAN_DIR" ] || mkdir -p "$MAN_DIR" 2>/dev/null; then
    if [ -w "$MAN_DIR" ]; then
        cp "${extracted}/hl.1" "${MAN_DIR}/hl.1"
    else
        sudo mkdir -p "$MAN_DIR" 2>/dev/null || true
        sudo cp "${extracted}/hl.1" "${MAN_DIR}/hl.1" 2>/dev/null || true
    fi
fi

# ── Verify ───────────────────────────────────────────────────────

if command -v hl >/dev/null 2>&1; then
    ok "hyper-linux v${VERSION} installed successfully!"
    printf '\n  Run:  hl ./your-linux-binary\n'
    printf '  Help: hl --help\n'
    printf '  Docs: https://hyper-linux.app\n\n'
else
    ok "Installed to ${INSTALL_DIR}/hl"
    printf '\n  Make sure %s is in your PATH.\n\n' "$INSTALL_DIR"
fi
