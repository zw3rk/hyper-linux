#!/usr/bin/env bash
# test-glibc-coreutils.sh — glibc dynamically-linked GNU coreutils test suite for hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Mirrors test-dynamic-coreutils.sh but invokes every tool through hl --sysroot,
# exercising the dynamic linker (ld-linux-aarch64.so.1) and glibc shared libc.so.6.
#
# Usage: test/test-glibc-coreutils.sh <hl-binary> <sysroot-dir> <coreutils-bin-dir>
# Example: test/test-glibc-coreutils.sh _build/hl $GUEST_SYSROOT $GUEST_DYNAMIC_COREUTILS/bin

set -euo pipefail

HL="${1:?Usage: $0 <hl-binary> <sysroot-dir> <coreutils-bin-dir>}"
SYSROOT="${2:?Usage: $0 <hl-binary> <sysroot-dir> <coreutils-bin-dir>}"
BIN="${3:?Usage: $0 <hl-binary> <sysroot-dir> <coreutils-bin-dir>}"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RESET='\033[0m'

pass=0
fail=0
skip=0
expected_fail=0

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Run a coreutils tool through hl --sysroot. Args:
#   $1 = tool name
#   $2 = expected exit code (default 0)
#   $3... = arguments to the tool
run() {
    local tool="$1"; shift
    local expect_rc="${1:-0}"; shift || true
    local name
    name=$(printf "%-14s" "$tool")

    if output=$("$HL" --sysroot "$SYSROOT" "$BIN/$tool" "$@" 2>&1); then
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

# Run and check output contains expected string
run_check() {
    local tool="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-14s" "$tool")

    if output=$("$HL" --sysroot "$SYSROOT" "$BIN/$tool" "$@" 2>&1); then
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

# Mark a tool as expected failure with reason
run_xfail() {
    local tool="$1"; shift
    local reason="$1"; shift
    local name
    name=$(printf "%-14s" "$tool")
    printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ XFAIL${RESET} (%s)\n" "$name" "$reason"
    expected_fail=$((expected_fail + 1))
}

# Mark a tool as skipped
run_skip() {
    local tool="$1"; shift
    local reason="$1"; shift
    local name
    name=$(printf "%-14s" "$tool")
    printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (%s)\n" "$name" "$reason"
    skip=$((skip + 1))
}

# Run a tool with piped stdin, check output for pattern
run_pipe() {
    local tool="$1"; shift
    local pattern="$1"; shift
    local input="${1:-}"; shift || true
    local name
    name=$(printf "%-14s" "$tool")

    if output=$(printf '%s' "$input" | timeout 10 "$HL" --sysroot "$SYSROOT" "$BIN/$tool" "$@" 2>&1); then
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

# Run a tool with a timeout, check exit code
run_timeout() {
    local secs="$1"; shift
    local tool="$1"; shift
    local expect_rc="${1:-0}"; shift || true
    local name
    name=$(printf "%-14s" "$tool")

    if output=$(timeout "$secs" "$HL" --sysroot "$SYSROOT" "$BIN/$tool" "$@" 2>&1); then
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

# Create test fixtures
echo "hello world" > "$TMPDIR/hello.txt"
echo -e "cherry\napple\nbanana" > "$TMPDIR/unsorted.txt"
echo -e "aaa\nbbb\naaa\nccc\nbbb" > "$TMPDIR/dups.txt"
echo -e "one\ttwo\tthree" > "$TMPDIR/tabs.txt"
echo -e "line1\nline2\nline3\nline4\nline5" > "$TMPDIR/lines.txt"
echo -e "a:b:c\nd:e:f" > "$TMPDIR/delim.txt"
mkdir -p "$TMPDIR/testdir/sub"
echo "file1" > "$TMPDIR/testdir/file1.txt"
echo "file2" > "$TMPDIR/testdir/sub/file2.txt"
ln -s "$TMPDIR/hello.txt" "$TMPDIR/symlink.txt"

printf "\n${BLUE}━━━ glibc dynamic GNU coreutils test suite (--sysroot) ━━━${RESET}\n\n"

# ── Output / text utilities ──────────────────────────────────────
printf "${BLUE}── Output / text utilities ──${RESET}\n"
run_check  cat       "hello world"          "$TMPDIR/hello.txt"
run_check  echo      "hello"                "hello"
run_check  printf    "42"                   "%d" 42
# yes writes infinitely — use timeout to limit; rc=124 (timeout) is expected
run_timeout 2  yes    124
run_check  head      "line1"               "$TMPDIR/lines.txt"
run_check  tail      "line5"               "$TMPDIR/lines.txt"
run_check  wc        "5"                   "-l" "$TMPDIR/lines.txt"
run_check  sort      "^apple"              "$TMPDIR/unsorted.txt"  # verify apple is first (sorted order)
run_check  uniq      "aaa"                 "$TMPDIR/dups.txt"
run_check  cut       "b"                   "-d:" "-f2" "$TMPDIR/delim.txt"
run_pipe   tr        "HELLO"               "hello" "a-z" "A-Z"
run_check  paste     "one"                 "$TMPDIR/tabs.txt"
run_check  expand    "one"                 "$TMPDIR/tabs.txt"
run_check  unexpand  "one"                 "$TMPDIR/tabs.txt"
run_check  fmt       "hello world"         "$TMPDIR/hello.txt"
run_check  fold      "hello"               "-w5" "$TMPDIR/hello.txt"
run_check  nl        "hello"               "$TMPDIR/hello.txt"
run_check  od        "0000000"             "-c" "$TMPDIR/hello.txt"
run_check  pr        "hello"               "-l20" "$TMPDIR/hello.txt"
run_check  tac       "line5"               "$TMPDIR/lines.txt"
run_check  comm      "apple"               "$TMPDIR/unsorted.txt" "$TMPDIR/unsorted.txt"
run_check  join      "a:b:c"              "$TMPDIR/delim.txt" "$TMPDIR/delim.txt"
run_check  ptx       "hello"              "$TMPDIR/hello.txt"
run_pipe   tsort     "a"                  "a b\nb c\n"  # topological sort
run_check  shuf      "line"               "-n1" "$TMPDIR/lines.txt"
run        split     0                   "-l2" "$TMPDIR/lines.txt" "$TMPDIR/split-"
run        csplit    0                   "$TMPDIR/lines.txt" 3

# ── Encoding / hashing ──────────────────────────────────────────
printf "\n${BLUE}── Encoding / hashing ──${RESET}\n"
run_check  base32    "NBSWY"              "$TMPDIR/hello.txt"
run_check  base64    "aGVsbG8gd29ybGQ"    "$TMPDIR/hello.txt"
run_check  basenc    "aGVsbG8"            "--base64" "$TMPDIR/hello.txt"
run_check  md5sum    "6f5902ac237024bdd0c176cb93063dc4"          "$TMPDIR/hello.txt"
run_check  sha1sum   "22596363b3de40b06f981fb85d82312e8c0ed511"  "$TMPDIR/hello.txt"
run_check  sha224sum "95041dd60ab08c0bf5636d50be85fe9790300f39eb84602858a9b430"  "$TMPDIR/hello.txt"
run_check  sha256sum "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447"  "$TMPDIR/hello.txt"
run_check  sha384sum "6b3b69ff0a404f28d75e98a066d3fc64fffd9940870cc68bece28545b9a75086"  "$TMPDIR/hello.txt"
run_check  sha512sum "db3974a97f2407b7cae1ae637c0030687a11913274d578492558e39c16c017de"  "$TMPDIR/hello.txt"
run_check  b2sum     "hello.txt"          "$TMPDIR/hello.txt"
run_check  cksum     "hello.txt"          "$TMPDIR/hello.txt"
run_check  sum       "[0-9]"              "$TMPDIR/hello.txt"

# ── File operations ──────────────────────────────────────────────
printf "\n${BLUE}── File operations ──${RESET}\n"
run        cp        0    "$TMPDIR/hello.txt" "$TMPDIR/hello-copy.txt"
run        mv        0    "$TMPDIR/hello-copy.txt" "$TMPDIR/hello-moved.txt"
run        rm        0    "$TMPDIR/hello-moved.txt"
run        ln        0    "-s" "$TMPDIR/hello.txt" "$TMPDIR/newlink.txt"
run        link      0    "$TMPDIR/hello.txt" "$TMPDIR/hardlink.txt"
run        unlink    0    "$TMPDIR/hardlink.txt"
run        mkdir     0    "$TMPDIR/newdir"
run        rmdir     0    "$TMPDIR/newdir"
run        mkfifo    0    "$TMPDIR/testfifo"
run        touch     0    "$TMPDIR/touched.txt"
run        truncate  0    "-s0" "$TMPDIR/touched.txt"
run        shred     0    "-u" "$TMPDIR/touched.txt"
run        install   0    "-m" "644" "$TMPDIR/hello.txt" "$TMPDIR/installed.txt"
run        dd        0    "if=$TMPDIR/hello.txt" "of=$TMPDIR/dd-out.txt" "bs=12" "count=1"
run        sync      0
run        mktemp    0    "-p" "$TMPDIR"

# ── File info ────────────────────────────────────────────────────
printf "\n${BLUE}── File info ──${RESET}\n"
run_check  ls        "hello.txt"           "$TMPDIR"
run_check  dir       "hello.txt"           "$TMPDIR"
run_check  vdir      "hello.txt"           "$TMPDIR"
run_check  stat      "File:"               "$TMPDIR/hello.txt"
run_check  du        "[0-9]"               "-s" "$TMPDIR"
run_check  df        "Filesystem"          "$TMPDIR"
run_check  dircolors "COLOR"               "-b"
run_check  readlink  "$TMPDIR/hello.txt"   "$TMPDIR/symlink.txt"
run_check  realpath  "hello.txt"           "$TMPDIR/hello.txt"

# ── Path utilities ───────────────────────────────────────────────
printf "\n${BLUE}── Path utilities ──${RESET}\n"
run_check  basename  "hello.txt"           "$TMPDIR/hello.txt"
run_check  dirname   "$TMPDIR"             "$TMPDIR/hello.txt"
run_check  pathchk   ""                    "$TMPDIR/hello.txt"
run_check  pwd       "/"                   # some path

# ── Math / sequence ──────────────────────────────────────────────
printf "\n${BLUE}── Math / sequence ──${RESET}\n"
run_check  seq       "5"                   "1" "5"
run_check  expr      "3"                   "1" "+" "2"
run_check  factor    "2 2 3"               "12"
run_check  numfmt    "1.0k"                "--to=si" "1000"

# ── System info ──────────────────────────────────────────────────
printf "\n${BLUE}── System info ──${RESET}\n"
run_check  uname     "Linux"               "-s"
run_check  date      "202"                 "+%Y"
run_check  nproc     "[0-9]"               # prints CPU count
run_check  uptime    "load average"        # reads /proc/uptime + /proc/loadavg
run_check  hostid    "[0-9a-f]"            # prints hex host ID
run_check  printenv  "/"                   "PATH"
run_check  id        "uid="                # prints uid/gid info

# ── Process utilities ────────────────────────────────────────────
printf "\n${BLUE}── Process utilities ──${RESET}\n"
run        true      0
run        false     1
run        sleep     0    "0"
run        env       0    "$BIN/true"
run        nice      0    "$BIN/true"
run        nohup     0    "$BIN/true"
run_check  kill      "TERM"                "-l"

# ── Permissions / ownership ──────────────────────────────────────
printf "\n${BLUE}── Permissions / ownership ──${RESET}\n"
run        chmod     0    "644" "$TMPDIR/hello.txt"
run        chown     1    "root:root" "$TMPDIR/hello.txt"  # expected to fail (not root)
run        chgrp     0    "root" "$TMPDIR/hello.txt"       # succeeds (fchown stub + /etc/group)
run        mknod     1    "$TMPDIR/testnode" "c" "1" "1"   # expected to fail (not root)

# ── User info (limited without /etc/passwd) ──────────────────────
printf "\n${BLUE}── User info ──${RESET}\n"
run_check  whoami    "user"                 # reads /etc/passwd (synthetic)
run        logname   1                                   # exit 1 = "no login name" (no tty)
run_check  groups    "user"                # reads /etc/group (synthetic)
run_check  pinky     "Login"               "-l" "user"  # reads /etc/passwd (synthetic)
run        who       0                                   # musl getutxent() is a stub; just verify exit 0
run        users     0                                   # musl getutxent() is a stub; just verify exit 0

# ── Terminal ─────────────────────────────────────────────────────
printf "\n${BLUE}── Terminal ──${RESET}\n"
run        tty       1                                   # exit 1 = "not a tty" (correct)
run        stty      1                                   # exit 1 = "not a tty" (correct)

# ── I/O utilities ────────────────────────────────────────────────
printf "\n${BLUE}── I/O utilities ──${RESET}\n"
run_pipe   tee       "hello world"        "hello world\n" "$TMPDIR/tee-out.txt"

# ── Special / test ───────────────────────────────────────────────
printf "\n${BLUE}── Special ──${RESET}\n"
run        test      0    "-f" "$TMPDIR/hello.txt"
run        "["       0    "-f" "$TMPDIR/hello.txt" "]"

# ── Expected failures / skips ────────────────────────────────────
printf "\n${BLUE}── Expected failures / skips ──${RESET}\n"
run_timeout 10 timeout 0 "5" "$BIN/true"
run_skip   stdbuf    "requires LD_PRELOAD (N/A for hl)"
run        chroot    0                     "/" "$BIN/true"

# ── Summary ──────────────────────────────────────────────────────
total=$((pass + fail + expected_fail + skip))
printf "\n${BLUE}━━━ glibc results: %d passed, %d failed, %d xfail, %d skipped (of %d) ━━━${RESET}\n" \
    "$pass" "$fail" "$expected_fail" "$skip" "$total"

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
