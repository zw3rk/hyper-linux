#!/usr/bin/env bash
# test-haskell-bins.sh — Haskell binary integration tests for hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Exercises pandoc and shellcheck through hl.  These are large, real-world
# Haskell programs that stress the GHC runtime: threaded RTS, heavy
# allocation, Lua FFI (pandoc), regex engines (shellcheck).
#
# Usage: test/test-haskell-bins.sh <hl-binary> <haskell-bins-dir> [<sysroot>] [<guest-extra-args>]
#        where <haskell-bins-dir> contains: pandoc, shellcheck
#        <sysroot> is optional; when set, dynamically-linked binaries
#        (pandoc) are run with --sysroot <sysroot>.
#        <guest-extra-args> is optional; passed to every guest binary
#        (e.g. "+RTS -xr4G -RTS" to shrink GHC's VA reservation on M1).

set -euo pipefail

HL="${1:?Usage: $0 <hl-binary> <haskell-bins-dir> [<sysroot>] [<guest-extra-args>]}"
BINDIR="${2:?Usage: $0 <hl-binary> <haskell-bins-dir> [<sysroot>] [<guest-extra-args>]}"
SYSROOT="${3:-}"
GUEST_EXTRA="${4:-}"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RESET='\033[0m'

pass=0
fail=0
skip=0

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Resolve binary path: try $BINDIR/<name> then $BINDIR/bin/<name>.
# Uses -f (not -x) because aarch64-linux ELFs aren't executable on macOS.
find_bin() {
    local name="$1"
    if [ -f "$BINDIR/$name" ]; then
        echo "$BINDIR/$name"
    elif [ -f "$BINDIR/bin/$name" ]; then
        echo "$BINDIR/bin/$name"
    else
        echo ""
    fi
}

# Run a test, checking that output contains a pattern.
# First 3 args: label, hl-args (array as string), bin, pattern, [extra args...]
# The HL_ARGS variable can be set per-section (e.g. "--sysroot /path") and
# is expanded into the hl command line before the binary path.
run_check() {
    local label="$1"; shift
    local bin="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-30s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    # shellcheck disable=SC2086
    if output=$("$HL" $HL_ARGS "$bin" $GUEST_EXTRA "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.200s\n" "$output" | head -5
        fail=$((fail + 1))
    fi
}

# Run a test with piped stdin.
run_pipe() {
    local label="$1"; shift
    local bin="$1"; shift
    local pattern="$1"; shift
    local input="$1"; shift
    local name
    name=$(printf "%-30s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    # shellcheck disable=SC2086
    if output=$(printf '%s' "$input" | "$HL" $HL_ARGS "$bin" $GUEST_EXTRA "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.200s\n" "$output" | head -5
        fail=$((fail + 1))
    fi
}

# Run a test expecting a specific nonzero exit code.
run_expect_fail() {
    local label="$1"; shift
    local bin="$1"; shift
    local expected_rc="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-30s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    # shellcheck disable=SC2086
    if output=$("$HL" $HL_ARGS "$bin" $GUEST_EXTRA "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = "$expected_rc" ] && echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET} (exit %d)\n" "$name" "$rc"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (expected rc=%d pattern '%s', got rc=%d)\n" \
            "$name" "$expected_rc" "$pattern" "$rc"
        printf "  %.200s\n" "$output" | head -5
        fail=$((fail + 1))
    fi
}

# Run a test expecting a specific nonzero exit code with piped stdin.
run_pipe_expect_fail() {
    local label="$1"; shift
    local bin="$1"; shift
    local expected_rc="$1"; shift
    local pattern="$1"; shift
    local input="$1"; shift
    local name
    name=$(printf "%-30s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    # shellcheck disable=SC2086
    if output=$(printf '%s' "$input" | "$HL" $HL_ARGS "$bin" $GUEST_EXTRA "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = "$expected_rc" ] && echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET} (exit %d)\n" "$name" "$rc"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (expected rc=%d pattern '%s', got rc=%d)\n" \
            "$name" "$expected_rc" "$pattern" "$rc"
        printf "  %.200s\n" "$output" | head -5
        fail=$((fail + 1))
    fi
}

# Locate binaries
PANDOC_BIN=$(find_bin pandoc)
SHELLCHECK_BIN=$(find_bin shellcheck)

# Both pandoc and shellcheck are dynamically linked with glibc
# (justStaticExecutables strips Haskell libs, not system libs).
# Use --sysroot when a sysroot is provided.
if [ -n "$SYSROOT" ]; then
    HL_ARGS="--sysroot $SYSROOT"
else
    HL_ARGS=""
fi

# pandoc data directory: nix compiles a nix store path into the binary
# for Cabal data-files lookup, which doesn't exist on non-NixOS hosts
# (e.g., the CI runner). Pass --data-dir to override when available.
PANDOC_DATA_DIR=""
for candidate in \
    "$BINDIR/../share/pandoc-data" \
    "$BINDIR/../../share/pandoc-data" \
    "$(dirname "$BINDIR")/share/pandoc-data"; do
    if [ -d "$candidate" ]; then
        PANDOC_DATA_DIR="$(cd "$candidate" && pwd)"
        break
    fi
done

printf "\n${BLUE}━━━ Haskell binary integration tests ━━━${RESET}\n\n"

# ── pandoc ──────────────────────────────────────────────────────────
printf "${BLUE}── pandoc ──${RESET}\n"

# Build pandoc extra args: --data-dir when bundled data is available.
# The nix-built pandoc binary has a hardcoded nix store path for Cabal
# data-files — this doesn't exist on non-NixOS hosts (CI runners).
PANDOC_EXTRA=""
if [ -n "$PANDOC_DATA_DIR" ]; then
    PANDOC_EXTRA="--data-dir $PANDOC_DATA_DIR"
fi

# Version check — confirms binary loads, GHC RTS initialises
run_check  "pandoc: version"          "$PANDOC_BIN" "pandoc"          --version

# Markdown → HTML conversion
# shellcheck disable=SC2086
run_pipe   "pandoc: md→html"          "$PANDOC_BIN" "<strong>bold</strong>" \
           "**bold** text" $PANDOC_EXTRA -f markdown -t html

# HTML → Markdown conversion (reverse)
# shellcheck disable=SC2086
run_pipe   "pandoc: html→md"          "$PANDOC_BIN" "\\*\\*hello\\*\\*" \
           "<p><strong>hello</strong> world</p>" $PANDOC_EXTRA -f html -t markdown

# Markdown → JSON AST output (exercises aeson/JSON serialisation)
# shellcheck disable=SC2086
run_pipe   "pandoc: md→json"          "$PANDOC_BIN" '"Str"' \
           "hello world" $PANDOC_EXTRA -f markdown -t json

# List all output formats (exercises Lua initialisation)
run_check  "pandoc: list formats"     "$PANDOC_BIN" "html"            --list-output-formats

# Markdown → plain text (strip formatting)
# shellcheck disable=SC2086
run_pipe   "pandoc: md→plain"         "$PANDOC_BIN" "heading" \
           "# heading" $PANDOC_EXTRA -f markdown -t plain

# ── shellcheck ──────────────────────────────────────────────────────
printf "\n${BLUE}── shellcheck ──${RESET}\n"

# Version check
run_check  "shellcheck: version"      "$SHELLCHECK_BIN" "[Ss]hell[Cc]heck" --version

# Good script — should pass (exit 0)
cat > "$TMPDIR/good.sh" << 'GOOD'
#!/bin/bash
name="world"
echo "Hello, ${name}!"
GOOD
run_check  "shellcheck: good script"  "$SHELLCHECK_BIN" "." \
           -s bash "$TMPDIR/good.sh"

# Bad script — should detect issues (exit 1)
# SC2086: Double quote to prevent globbing and word splitting
cat > "$TMPDIR/bad.sh" << 'BAD'
#!/bin/bash
name="hello world"
echo $name
BAD
run_expect_fail "shellcheck: bad script"  "$SHELLCHECK_BIN" 1 "SC2086" \
                -s bash "$TMPDIR/bad.sh"

# JSON output format
run_expect_fail "shellcheck: json output" "$SHELLCHECK_BIN" 1 '"code"' \
                -f json -s bash "$TMPDIR/bad.sh"

# GCC-style output format
run_expect_fail "shellcheck: gcc output"  "$SHELLCHECK_BIN" 1 "SC2086" \
                -f gcc -s bash "$TMPDIR/bad.sh"

# ── Summary ─────────────────────────────────────────────────────────
total=$((pass + fail + skip))
printf "\n${BLUE}━━━ Results: %d passed, %d failed, %d skipped (of %d) ━━━${RESET}\n" \
    "$pass" "$fail" "$skip" "$total"

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
