#!/usr/bin/env bash
# test-busybox.sh — Busybox 1.37.0 applet smoke tests for hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Tests a selection of busybox applets through hl. Busybox includes ~300+
# applets covering shell, networking stubs, editors, and more.
#
# Usage: test/test-busybox.sh <hl-binary> <busybox-binary>

set -euo pipefail

HL="${1:?Usage: $0 <hl-binary> <busybox-binary>}"
BB="${2:?Usage: $0 <hl-binary> <busybox-binary>}"

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

run_check() {
    local applet="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-16s" "$applet")

    if output=$("$HL" "$BB" "$applet" "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -q "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

run() {
    local applet="$1"; shift
    local expect_rc="${1:-0}"; shift || true
    local name
    name=$(printf "%-16s" "$applet")

    if output=$("$HL" "$BB" "$applet" "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = "$expect_rc" ]; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (got %d, expected %d)\n" "$name" "$rc" "$expect_rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

run_skip() {
    local applet="$1"; shift
    local reason="$1"
    local name
    name=$(printf "%-16s" "$applet")
    printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (%s)\n" "$name" "$reason"
    skip=$((skip + 1))
}

# Run an applet with piped stdin, check output for pattern
run_pipe() {
    local applet="$1"; shift
    local pattern="$1"; shift
    local input="${1:-}"; shift || true
    local name
    name=$(printf "%-16s" "$applet")

    if output=$(printf '%s' "$input" | timeout 10 "$HL" "$BB" "$applet" "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -q "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# Test fixtures
echo "hello world" > "$TMPDIR/hello.txt"
echo -e "cherry\napple\nbanana" > "$TMPDIR/unsorted.txt"
echo -e "line1\nline2\nline3\nline4\nline5" > "$TMPDIR/lines.txt"
mkdir -p "$TMPDIR/testdir"
echo "content" > "$TMPDIR/testdir/file.txt"

printf "\n${BLUE}━━━ Busybox 1.37.0 applet smoke tests ━━━${RESET}\n\n"

# ── Core utilities ───────────────────────────────────────────────
printf "${BLUE}── Core utilities ──${RESET}\n"
run_check  echo      "hello"               "hello"
run_check  printf    "42"                   "%d" 42
run_check  cat       "hello world"          "$TMPDIR/hello.txt"
run_check  head      "line1"               "-n1" "$TMPDIR/lines.txt"
run_check  tail      "line5"               "-n1" "$TMPDIR/lines.txt"
run_check  wc        "5"                   "-l" "$TMPDIR/lines.txt"
run_check  sort      "apple"               "$TMPDIR/unsorted.txt"
run_check  uniq      "hello"               "$TMPDIR/hello.txt"
run_check  cut       "hello"               "-d " "-f1" "$TMPDIR/hello.txt"
run_pipe   tr        "HELLO"               "hello" "a-z" "A-Z"
run_pipe   sed       "HELLO"               "hello" "s/hello/HELLO/"
run_pipe   awk       "b"                   "a b" "{print \$2}"
run_pipe   grep      "hello"               "hello" "hello"
run_check  tee       ""                    "$TMPDIR/tee-out.txt" < /dev/null || true
run        true      0
run        false     1
run        sleep     0    "0"

# ── File operations ──────────────────────────────────────────────
printf "\n${BLUE}── File operations ──${RESET}\n"
run        cp        0    "$TMPDIR/hello.txt" "$TMPDIR/hello-cp.txt"
run        mv        0    "$TMPDIR/hello-cp.txt" "$TMPDIR/hello-mv.txt"
run        rm        0    "$TMPDIR/hello-mv.txt"
run        ln        0    "-s" "$TMPDIR/hello.txt" "$TMPDIR/bb-link.txt"
run        mkdir     0    "$TMPDIR/bb-newdir"
run        rmdir     0    "$TMPDIR/bb-newdir"
run        touch     0    "$TMPDIR/bb-touched.txt"
run        chmod     0    "644" "$TMPDIR/bb-touched.txt"
run_check  ls        "hello.txt"           "$TMPDIR"
run_check  stat      "File:"               "$TMPDIR/hello.txt"
run_check  du        "[0-9]"               "-s" "$TMPDIR"
run_check  df        "Filesystem"          "$TMPDIR"
run_check  readlink  "$TMPDIR/hello.txt"   "$TMPDIR/bb-link.txt"
run_check  realpath  ""                    "$TMPDIR/hello.txt"
run_check  basename  "hello.txt"           "$TMPDIR/hello.txt"
run_check  dirname   "$TMPDIR"             "$TMPDIR/hello.txt"
run_check  pwd       "/"
run        dd        0    "if=$TMPDIR/hello.txt" "of=$TMPDIR/bb-dd.txt" "bs=12" "count=1"
run        sync      0

# ── Text processing ──────────────────────────────────────────────
printf "\n${BLUE}── Text processing ──${RESET}\n"
run_check  md5sum    "hello.txt"           "$TMPDIR/hello.txt"
run_check  sha1sum   "hello.txt"           "$TMPDIR/hello.txt"
run_check  sha256sum "hello.txt"           "$TMPDIR/hello.txt"
run_check  sha512sum "hello.txt"           "$TMPDIR/hello.txt"
run_check  od        "0000000"             "-c" "$TMPDIR/hello.txt"
run_check  hexdump   "0000000"             "-C" "$TMPDIR/hello.txt"
run_check  xxd       "0000000"             "$TMPDIR/hello.txt"
run_check  base64    "aGVsbG8gd29ybGQ"    "$TMPDIR/hello.txt"
run_check  fold      "hello"              "-w5" "$TMPDIR/hello.txt"
run_check  nl        "hello"              "$TMPDIR/hello.txt"
run_check  expand    "hello"              "$TMPDIR/hello.txt"
run_check  unexpand  "hello"              "$TMPDIR/hello.txt"
run_check  paste     "hello"              "$TMPDIR/hello.txt"
run_check  tac       "line5"              "$TMPDIR/lines.txt"
run_check  rev       "dlrow olleh"        "$TMPDIR/hello.txt"
run_check  comm      ""                   "$TMPDIR/hello.txt" "$TMPDIR/hello.txt"

# ── Math / misc ──────────────────────────────────────────────────
printf "\n${BLUE}── Math / misc ──${RESET}\n"
run_check  seq       "5"                  "1" "5"
run_check  expr      "3"                  "1" "+" "2"
run_check  factor    "2 2 3"              "12"
run_check  date      ""                   "+%Y"
run_check  uname     "Linux"              "-s"
run_check  id        "uid="
run_check  whoami    "user"               # reads /etc/passwd (synthetic)
run_check  hostname  "hl"                # returns synthetic hostname
run_check  env       "PATH"             # prints environment variables
run_check  test      ""                   "-f" "$TMPDIR/hello.txt"

# ── Archive / compression ────────────────────────────────────────
printf "\n${BLUE}── Archive / compression ──${RESET}\n"
run        gzip      0    "-k" "$TMPDIR/hello.txt"
run_check  zcat      "hello world"        "$TMPDIR/hello.txt.gz"
rm -f "$TMPDIR/hello.txt"   # Remove original so gunzip can decompress
run        gunzip    0    "$TMPDIR/hello.txt.gz"
echo "test data" > "$TMPDIR/tar-file.txt"
run        tar       0    "cf" "$TMPDIR/test.tar" "-C" "$TMPDIR" "tar-file.txt"
run_check  tar       "tar-file.txt"       "tf" "$TMPDIR/test.tar"
echo "bzip test data" > "$TMPDIR/bz-file.txt"
run        bzip2     0    "-k" "$TMPDIR/bz-file.txt"
run_check  bzcat     "bzip test data"     "$TMPDIR/bz-file.txt.bz2"
rm -f "$TMPDIR/bz-file.txt"
run        bunzip2   0    "$TMPDIR/bz-file.txt.bz2"

# ── Additional utilities ────────────────────────────────────────
printf "\n${BLUE}── Additional utilities ──${RESET}\n"
run_pipe   bc        "6"                  $'2*3\n'
run        cmp       0    "$TMPDIR/hello.txt" "$TMPDIR/hello.txt"
echo "different" > "$TMPDIR/diff-other.txt"
run        diff      1    "$TMPDIR/hello.txt" "$TMPDIR/diff-other.txt"
run_check  strings   "hello"              "$TMPDIR/hello.txt"
run_check  find      "hello.txt"          "$TMPDIR" "-name" "hello.txt"

# ── Networking ───────────────────────────────────────────────────
printf "\n${BLUE}── Networking ──${RESET}\n"
run_check  nslookup  "Address"              "example.com"
run_check  wget      "Example"              "-q" "-O" "-" "http://example.com/"
run_skip   ping      "needs raw socket / setuid"
# nc: use subshell+sleep to delay stdin EOF — prevents premature half-close
applet="nc"
name=$(printf "%-16s" "$applet")
if output=$( (printf 'HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n'; sleep 2) \
             | timeout 10 "$HL" "$BB" nc -w 3 example.com 80 2>&1 ); then
    rc=0
else
    rc=$?
fi
if echo "$output" | grep -q "HTTP"; then
    printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
    pass=$((pass + 1))
else
    printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern 'HTTP' not found, rc=%d)\n" "$name" "$rc"
    printf "  %.120s\n" "$output" | head -3
    fail=$((fail + 1))
fi
run_skip   telnet    "needs interactive terminal"

# ── Shell ────────────────────────────────────────────────────────
printf "\n${BLUE}── Shell ──${RESET}\n"
run_pipe   ash       "hello"              "" "-c" "echo hello"
run_pipe   sh        "hello"              "" "-c" "echo hello"

# ── Summary ──────────────────────────────────────────────────────
total=$((pass + fail + skip))
printf "\n${BLUE}━━━ Results: %d passed, %d failed, %d skipped (of %d) ━━━${RESET}\n" \
    "$pass" "$fail" "$skip" "$total"

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
