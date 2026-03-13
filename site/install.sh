#!/bin/sh
# hyper-linux installer — https://hyper-linux.app
#
# Usage: curl -fsSL https://hyper-linux.app/install.sh | sh
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -e

VERSION="0.2.1"
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

# ── Detect alternative package managers ─────────────────────────

HAS_BREW=0; HAS_NIX=0
command -v brew >/dev/null 2>&1 && HAS_BREW=1
command -v nix  >/dev/null 2>&1 && HAS_NIX=1

if [ -t 0 ] && { [ "$HAS_BREW" = 1 ] || [ "$HAS_NIX" = 1 ]; }; then
    # Interactive terminal with alternative installers available
    printf '\n\033[1;33mAlternative installers detected:\033[0m\n'
    N=1
    BREW_N=0; NIX_N=0
    if [ "$HAS_BREW" = 1 ]; then
        BREW_N=$N
        printf '  %d) brew install zw3rk/hyper-linux/hl\n' "$N"
        N=$((N + 1))
    fi
    if [ "$HAS_NIX" = 1 ]; then
        NIX_N=$N
        printf '  %d) nix profile install github:zw3rk/hyper-linux\n' "$N"
        N=$((N + 1))
    fi
    CURL_N=$N
    printf '  %d) Continue with this installer\n' "$N"
    printf '\033[1;33m?\033[0m Choice [%d]: ' "$CURL_N"
    read -r CHOICE
    CHOICE="${CHOICE:-$CURL_N}"
    if [ "$HAS_BREW" = 1 ] && [ "$CHOICE" = "$BREW_N" ]; then
        exec brew install zw3rk/hyper-linux/hl
    fi
    if [ "$HAS_NIX" = 1 ] && [ "$CHOICE" = "$NIX_N" ]; then
        exec nix profile install "github:zw3rk/hyper-linux"
    fi
    printf '\n'
elif [ "$HAS_BREW" = 1 ] || [ "$HAS_NIX" = 1 ]; then
    # Non-interactive (piped): just print a tip.
    # Build the "via X" or "via X and Y" list explicitly to get correct grammar.
    _tip_via=""
    [ "$HAS_BREW" = 1 ] && _tip_via="brew"
    if [ "$HAS_NIX" = 1 ]; then
        [ -n "$_tip_via" ] && _tip_via="$_tip_via and nix" || _tip_via="nix"
    fi
    info "Tip: hl is also available via $_tip_via"
fi

info "Installing hyper-linux v${VERSION}..."

# ── Download ─────────────────────────────────────────────────────

ASSET_NAME="hl-v${VERSION}.zip"
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

# ── SHA256 verification ──────────────────────────────────────────

SHA256_URL="https://github.com/${REPO}/releases/download/v${VERSION}/SHA256SUMS"
sha256_ok=0
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "${tmpdir}/SHA256SUMS" "$SHA256_URL" 2>/dev/null && sha256_ok=1
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${tmpdir}/SHA256SUMS" "$SHA256_URL" 2>/dev/null && sha256_ok=1
fi
if [ "$sha256_ok" = 1 ]; then
    EXPECTED=$(grep -F "${ASSET_NAME}" "${tmpdir}/SHA256SUMS" | cut -d' ' -f1)
    if [ -n "$EXPECTED" ]; then
        ACTUAL=$(shasum -a 256 "${tmpdir}/${ASSET_NAME}" | cut -d' ' -f1)
        if [ "$ACTUAL" != "$EXPECTED" ]; then
            err "SHA256 mismatch! Expected: ${EXPECTED}, Got: ${ACTUAL}"
        fi
        ok "SHA256 verified"
    else
        info "SHA256SUMS found but no entry for ${ASSET_NAME} — skipping verification"
    fi
else
    info "SHA256SUMS not available for this release — skipping verification"
fi

# ── Extract ──────────────────────────────────────────────────────

info "Extracting..."
# The zip contains a directory (hl-v<version>/) with hl, hl.1, entitlements.plist, etc.
unzip -qo "${tmpdir}/${ASSET_NAME}" -d "${tmpdir}"

# Find the extracted directory (try versioned name first, fall back to any hl-*)
extracted="${tmpdir}/hl-v${VERSION}"
if [ ! -d "$extracted" ]; then
    extracted=$(find "$tmpdir" -maxdepth 1 -type d -name 'hl-*' | head -1)
fi
if [ -z "$extracted" ] || [ ! -d "$extracted" ]; then
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
