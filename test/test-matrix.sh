#!/usr/bin/env bash
# test-matrix.sh — Run test suites across hl, hl/rosetta, lima, lima/rosetta
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Usage: test/test-matrix.sh <mode>
#   mode: hl-aarch64 | hl-x64 | lima-aarch64 | lima-x64 | all

set -euo pipefail

MODE="${1:?Usage: $0 <hl-aarch64|hl-x64|lima-aarch64|lima-x64|all>}"

# ── Paths ─────────────────────────────────────────────────────────
HL=_build/hl
LIMACTL=/nix/store/75nyrllrdr96sbbfnwc3vxgmnbhhkar9-lima-2.0.3/bin/limactl
LIMA_VM=hl-rosetta

AARCH64_TEST_BIN=/nix/store/c58xq21arripb2raafjw8gr2yn3wv908-hl-test-binaries-aarch64-unknown-linux-musl-0.1.0/bin
AARCH64_COREUTILS=/nix/store/z2s6b6sghq3ax72mlwva4iabkjigd102-coreutils-aarch64-unknown-linux-musl-9.9/bin
AARCH64_BUSYBOX=/nix/store/pqz4ip0p7d911dqk8wsy90sbw63mcqzn-busybox-static-aarch64-unknown-linux-musl-1.37.0/bin/busybox
AARCH64_STATIC=/nix/store/r5mdx3mm79nql4wksg881f8ra7kxzghr-hl-static-bins/bin

X64_TEST_BIN=/nix/store/m1ncs5x28i0igcdzlkx7kr37rs1c05hw-hl-x64-test-binaries-x86_64-unknown-linux-musl-0.1.0/bin
X64_COREUTILS=/nix/store/a15g0vkrw5w7izkf5a37qrb6ymrdpkwy-coreutils-static-x86_64-unknown-linux-musl-9.9/bin
X64_BUSYBOX=/nix/store/aflblv48yhibafwyvh2qfl0r1xx41d6v-busybox-static-x86_64-unknown-linux-musl-1.37.0/bin/busybox

# ── Colors ────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
RESET='\033[0m'

# ── Globals ───────────────────────────────────────────────────────
pass=0
fail=0
skip=0
timeout_count=0

# Test fixture directory — must be accessible from the runner's context.
# For hl modes, a local macOS temp dir works fine.
# For lima modes, macOS $TMPDIR (/var/folders/...) is not mounted in the
# VM, so we create the fixtures inside the VM's /tmp instead.
TEST_TMPDIR=""
_lima_tmpdir=""

setup_fixtures() {
    local mode="$1"
    case "$mode" in
        lima-*)
            # Create temp dir inside the Lima VM and populate fixtures there.
            _lima_tmpdir=$("$LIMACTL" shell "$LIMA_VM" -- mktemp -d /tmp/test-matrix.XXXXXX 2>/dev/null)
            TEST_TMPDIR="$_lima_tmpdir"
            "$LIMACTL" shell "$LIMA_VM" -- sh -c "
                echo 'hello world' > '$TEST_TMPDIR/hello.txt'
                printf 'cherry\napple\nbanana\n' > '$TEST_TMPDIR/unsorted.txt'
                printf 'line1\nline2\nline3\nline4\nline5\n' > '$TEST_TMPDIR/lines.txt'
            " 2>/dev/null
            ;;
        *)
            # Local macOS temp dir — accessible by hl directly.
            TEST_TMPDIR=$(mktemp -d)
            echo "hello world" > "$TEST_TMPDIR/hello.txt"
            printf 'cherry\napple\nbanana\n' > "$TEST_TMPDIR/unsorted.txt"
            printf 'line1\nline2\nline3\nline4\nline5\n' > "$TEST_TMPDIR/lines.txt"
            ;;
    esac
}

cleanup_fixtures() {
    if [ -n "$_lima_tmpdir" ]; then
        "$LIMACTL" shell "$LIMA_VM" -- rm -rf "$_lima_tmpdir" 2>/dev/null || true
        _lima_tmpdir=""
    fi
    if [ -n "$TEST_TMPDIR" ] && [ -d "$TEST_TMPDIR" ]; then
        rm -rf "$TEST_TMPDIR"
    fi
    TEST_TMPDIR=""
}

trap 'cleanup_fixtures' EXIT

# ── Runners ───────────────────────────────────────────────────────

# Run binary via hl, with timeout (stderr suppressed to avoid hl debug noise)
run_hl() {
    timeout 30 "$HL" "$@" 2>/dev/null
}

# Run binary directly in Lima VM (stderr suppressed to avoid limactl warnings)
run_lima() {
    timeout 60 "$LIMACTL" shell "$LIMA_VM" -- "$@" 2>/dev/null
}

# Generic test: run binary, check output contains pattern
test_check() {
    local runner="$1"; shift
    local label="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-28s" "$label")

    local output rc
    if output=$($runner "$@"); then
        rc=0
    else
        rc=$?
    fi

    # timeout returns 124
    if [ "$rc" = "124" ]; then
        printf "${YELLOW}▸${RESET} %s ${CYAN}⏱ TIMEOUT${RESET}\n" "$name"
        timeout_count=$((timeout_count + 1))
        return
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (rc=%d)\n" "$name" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# Generic test: run binary, check exit code
test_rc() {
    local runner="$1"; shift
    local label="$1"; shift
    local expect_rc="$1"; shift
    local name
    name=$(printf "%-28s" "$label")

    local output rc
    if output=$($runner "$@"); then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = "124" ]; then
        printf "${YELLOW}▸${RESET} %s ${CYAN}⏱ TIMEOUT${RESET}\n" "$name"
        timeout_count=$((timeout_count + 1))
        return
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

# Generic test: pipe input, check output pattern
test_pipe() {
    local runner="$1"; shift
    local label="$1"; shift
    local pattern="$1"; shift
    local input="$1"; shift
    local name
    name=$(printf "%-28s" "$label")

    local output rc
    if output=$(printf '%s' "$input" | $runner "$@"); then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = "124" ]; then
        printf "${YELLOW}▸${RESET} %s ${CYAN}⏱ TIMEOUT${RESET}\n" "$name"
        timeout_count=$((timeout_count + 1))
        return
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (rc=%d)\n" "$name" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# ── Test suite: unit tests ────────────────────────────────────────
run_unit_tests() {
    local runner="$1"
    local bindir="$2"

    printf "${BLUE}── Unit tests ──${RESET}\n"
    test_check "$runner" "hello-musl"        "Hello"           "$bindir/hello-musl"
    test_check "$runner" "hello-write"        "Hello"           "$bindir/hello-write"
    test_check "$runner" "echo-test"          "hello world"     "$bindir/echo-test" hello world
    test_check "$runner" "test-argc"          "argc.*3"         "$bindir/test-argc" arg1 arg2
    test_rc    "$runner" "test-complex"       42                "$bindir/test-complex"
    test_check "$runner" "test-fileio"        "lines"           "$bindir/test-fileio" CLAUDE.md
    test_check "$runner" "test-string"        "memcpy"          "$bindir/test-string"
    test_check "$runner" "test-malloc"        "OK"              "$bindir/test-malloc"
    test_check "$runner" "test-cat"           ""                "$bindir/test-cat" test/hello.S
    test_check "$runner" "test-ls"            "hello"           "$bindir/test-ls" test/
    test_check "$runner" "test-roundtrip"     "OK"              "$bindir/test-roundtrip"
    test_check "$runner" "test-comprehensive" "0 failures"      "$bindir/test-comprehensive"

    printf "\n${BLUE}── Process tests ──${RESET}\n"
    test_check "$runner" "test-fork"          "PASS"            "$bindir/test-fork"
    test_check "$runner" "test-exec"          "exec-works"      "$bindir/test-exec" "$bindir/echo-test" exec-works
    test_check "$runner" "test-fork-exec"     "PASS"            "$bindir/test-fork-exec" "$bindir/echo-test"
    test_check "$runner" "test-cloexec"       "PASS"            "$bindir/test-cloexec"

    printf "\n${BLUE}── Signal tests ──${RESET}\n"
    test_check "$runner" "test-signal"        "PASS|0 failed"   "$bindir/test-signal"
    test_check "$runner" "test-signal-thread" "PASS|0 failed"   "$bindir/test-signal-thread"

    printf "\n${BLUE}── Socket tests ──${RESET}\n"
    test_check "$runner" "test-socket"        "PASS|0 failed"   "$bindir/test-socket"

    printf "\n${BLUE}── Syscall coverage ──${RESET}\n"
    test_check "$runner" "test-file-ops"      "0 failed"        "$bindir/test-file-ops"
    test_check "$runner" "test-sysinfo"       "0 failed"        "$bindir/test-sysinfo"
    test_check "$runner" "test-io-opt"        "0 failed"        "$bindir/test-io-opt"
    test_check "$runner" "test-poll"          "0 failed"        "$bindir/test-poll"

    printf "\n${BLUE}── I/O subsystem ──${RESET}\n"
    test_check "$runner" "test-eventfd"       "0 failed"        "$bindir/test-eventfd"
    test_check "$runner" "test-signalfd"      "0 failed"        "$bindir/test-signalfd"
    test_check "$runner" "test-epoll"         "0 failed"        "$bindir/test-epoll"
    test_check "$runner" "test-timerfd"       "0 failed"        "$bindir/test-timerfd"

    printf "\n${BLUE}── /proc and /dev ──${RESET}\n"
    test_check "$runner" "test-proc"          "0 failed"        "$bindir/test-proc"

    printf "\n${BLUE}── Network ──${RESET}\n"
    test_check "$runner" "test-net"           "0 failed"        "$bindir/test-net"

    printf "\n${BLUE}── Threading ──${RESET}\n"
    test_check "$runner" "test-thread"        "0 failed"        "$bindir/test-thread"
    test_check "$runner" "test-pthread"       "0 failed"        "$bindir/test-pthread"
    test_check "$runner" "test-stress"        "0 failed"        "$bindir/test-stress"

    printf "\n${BLUE}── Negative tests ──${RESET}\n"
    test_check "$runner" "test-negative"      "0 failed"        "$bindir/test-negative"
}

# ── Test suite: coreutils ─────────────────────────────────────────
run_coreutils_tests() {
    local runner="$1"
    local bindir="$2"

    printf "${BLUE}── Coreutils text ──${RESET}\n"
    test_check "$runner" "echo"               "hello"           "$bindir/echo" hello
    test_check "$runner" "cat"                "hello world"     "$bindir/cat" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "head"               "line1"           "$bindir/head" "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "tail"               "line5"           "$bindir/tail" "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "wc"                 "5"               "$bindir/wc" -l "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "sort"               "apple"           "$bindir/sort" "$TEST_TMPDIR/unsorted.txt"
    test_pipe  "$runner" "tr"                 "HELLO"           "hello" "$bindir/tr" a-z A-Z
    test_check "$runner" "seq"                "5"               "$bindir/seq" 1 5
    test_check "$runner" "expr"               "3"               "$bindir/expr" 1 + 2
    test_check "$runner" "factor"             "2 2 3"           "$bindir/factor" 12
    test_check "$runner" "base64"             "aGVsbG8"         "$bindir/base64" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "md5sum"             "hello.txt"       "$bindir/md5sum" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "sha256sum"          "hello.txt"       "$bindir/sha256sum" "$TEST_TMPDIR/hello.txt"

    printf "\n${BLUE}── Coreutils file ops ──${RESET}\n"
    test_rc    "$runner" "cp"                 0                 "$bindir/cp" "$TEST_TMPDIR/hello.txt" "$TEST_TMPDIR/hello-cp-$$"
    test_rc    "$runner" "touch"              0                 "$bindir/touch" "$TEST_TMPDIR/touched-$$"
    test_check "$runner" "ls"                 "hello"           "$bindir/ls" "$TEST_TMPDIR"
    test_check "$runner" "stat"               "File:"           "$bindir/stat" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "basename"           "hello.txt"       "$bindir/basename" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "dirname"            "$TEST_TMPDIR"    "$bindir/dirname" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "realpath"           "hello.txt"       "$bindir/realpath" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "df"                 "Filesystem"      "$bindir/df" "$TEST_TMPDIR"
    test_check "$runner" "du"                 "[0-9]"           "$bindir/du" -s "$TEST_TMPDIR"

    printf "\n${BLUE}── Coreutils sysinfo ──${RESET}\n"
    test_check "$runner" "uname"              "Linux"           "$bindir/uname" -s
    test_check "$runner" "date"               "202"             "$bindir/date" "+%Y"
    test_check "$runner" "id"                 "uid="            "$bindir/id"
    test_check "$runner" "printenv"           "/"               "$bindir/printenv" PATH
    test_check "$runner" "nproc"              "[0-9]"           "$bindir/nproc"

    printf "\n${BLUE}── Coreutils process ──${RESET}\n"
    test_rc    "$runner" "true"               0                 "$bindir/true"
    test_rc    "$runner" "false"              1                 "$bindir/false"
    test_rc    "$runner" "sleep"              0                 "$bindir/sleep" 0
    test_rc    "$runner" "env"                0                 "$bindir/env" "$bindir/true"
    test_rc    "$runner" "nice"               0                 "$bindir/nice" "$bindir/true"
    test_rc    "$runner" "nohup"              0                 "$bindir/nohup" "$bindir/true"
    test_rc    "$runner" "timeout"            0                 "$bindir/timeout" 5 "$bindir/true"

    printf "\n${BLUE}── Coreutils encoding ──${RESET}\n"
    test_check "$runner" "base32"             "NBSWY"           "$bindir/base32" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "sha1sum"            "hello.txt"       "$bindir/sha1sum" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "sha512sum"          "hello.txt"       "$bindir/sha512sum" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "b2sum"              "hello.txt"       "$bindir/b2sum" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "cksum"              "hello.txt"       "$bindir/cksum" "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "numfmt"             "1\\.0[kK]"       "$bindir/numfmt" --to=si 1000
}

# ── Test suite: busybox ───────────────────────────────────────────
run_busybox_tests() {
    local runner="$1"
    local bb="$2"

    printf "${BLUE}── Busybox core ──${RESET}\n"
    test_check "$runner" "bb echo"            "hello"           "$bb" echo hello
    test_check "$runner" "bb cat"             "hello world"     "$bb" cat "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb head"            "line1"           "$bb" head -n1 "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "bb tail"            "line5"           "$bb" tail -n1 "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "bb wc"              "5"               "$bb" wc -l "$TEST_TMPDIR/lines.txt"
    test_check "$runner" "bb sort"            "apple"           "$bb" sort "$TEST_TMPDIR/unsorted.txt"
    test_check "$runner" "bb seq"             "5"               "$bb" seq 1 5
    test_check "$runner" "bb expr"            "3"               "$bb" expr 1 + 2
    test_check "$runner" "bb factor"          "2 2 3"           "$bb" factor 12
    test_check "$runner" "bb base64"          "aGVsbG8"         "$bb" base64 "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb md5sum"          "hello.txt"       "$bb" md5sum "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb sha256sum"       "hello.txt"       "$bb" sha256sum "$TEST_TMPDIR/hello.txt"
    test_pipe  "$runner" "bb tr"              "HELLO"           "hello" "$bb" tr a-z A-Z
    test_pipe  "$runner" "bb sed"             "HELLO"           "hello" "$bb" sed 's/hello/HELLO/'
    test_pipe  "$runner" "bb awk"             "b"               "a b"  "$bb" awk '{print $2}'
    test_pipe  "$runner" "bb grep"            "hello"           "hello" "$bb" grep hello

    printf "\n${BLUE}── Busybox file ops ──${RESET}\n"
    test_rc    "$runner" "bb cp"              0                 "$bb" cp "$TEST_TMPDIR/hello.txt" "$TEST_TMPDIR/bb-cp-$$"
    test_rc    "$runner" "bb touch"           0                 "$bb" touch "$TEST_TMPDIR/bb-touch-$$"
    test_check "$runner" "bb ls"              "hello"           "$bb" ls "$TEST_TMPDIR"
    test_check "$runner" "bb stat"            "File:"           "$bb" stat "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb basename"        "hello.txt"       "$bb" basename "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb dirname"         "$TEST_TMPDIR"    "$bb" dirname "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb uname"           "Linux"           "$bb" uname -s
    test_check "$runner" "bb date"            "202"             "$bb" date "+%Y"
    test_check "$runner" "bb id"              "uid="            "$bb" id

    printf "\n${BLUE}── Busybox archive ──${RESET}\n"
    test_rc    "$runner" "bb gzip"            0                 "$bb" gzip -kf "$TEST_TMPDIR/hello.txt"
    test_check "$runner" "bb zcat"            "hello world"     "$bb" zcat "$TEST_TMPDIR/hello.txt.gz"

    printf "\n${BLUE}── Busybox shell ──${RESET}\n"
    test_pipe  "$runner" "bb ash"             "hello"           "" "$bb" ash -c "echo hello"
    test_pipe  "$runner" "bb sh"              "hello"           "" "$bb" sh -c "echo hello"
}

# ── Test suite: static bins ───────────────────────────────────────
run_static_tests() {
    local runner="$1"
    local bindir="$2"

    printf "${BLUE}── Static bins ──${RESET}\n"

    if [ -f "$bindir/dash" ]; then
        test_check "$runner" "dash echo"         "hello"           "$bindir/dash" -c "echo hello"
        test_check "$runner" "dash arithmetic"   "2\\+3=5"         "$bindir/dash" -c 'echo "2+3=$((2+3))"'
    fi

    if [ -f "$bindir/bash" ]; then
        test_check "$runner" "bash echo"         "hello"           "$bindir/bash" -c "echo hello"
        test_pipe  "$runner" "bash subshell"     "sub=25"          ""  "$bindir/bash" -c 'echo "sub=$(echo $((5*5)))"'
    fi

    if [ -f "$bindir/lua" ]; then
        test_check "$runner" "lua hello"         "Hello"           "$bindir/lua" -e 'print("Hello from " .. _VERSION)'
        test_check "$runner" "lua fib(30)"       "832040"          "$bindir/lua" -e 'local function f(n) if n<2 then return n end; return f(n-1)+f(n-2) end; print(f(30))'
    fi

    if [ -f "$bindir/gawk" ]; then
        test_pipe  "$runner" "gawk field"        "world"           "hello world" "$bindir/gawk" '{print $2}'
    fi

    if [ -f "$bindir/grep" ]; then
        test_pipe  "$runner" "grep basic"        "hello"           "hello world" "$bindir/grep" hello
    fi

    if [ -f "$bindir/sed" ]; then
        test_pipe  "$runner" "sed subst"         "HELLO"           "hello" "$bindir/sed" 's/hello/HELLO/'
    fi

    if [ -f "$bindir/jq" ]; then
        test_pipe  "$runner" "jq simple"         "^1$"             '{"a":1}' "$bindir/jq" '.a'
        test_pipe  "$runner" "jq filter"         "Alice"           '{"users":[{"name":"Alice","age":30},{"name":"Bob","age":25}]}' "$bindir/jq" '.users[] | select(.age > 28) | .name'
    fi

    if [ -f "$bindir/sqlite3" ]; then
        test_check "$runner" "sqlite version"    "^3\\."           "$bindir/sqlite3" ":memory:" "SELECT sqlite_version();"
        test_check "$runner" "sqlite arith"      "^42$"            "$bindir/sqlite3" ":memory:" "SELECT 6 * 7;"
    fi

    if [ -f "$bindir/tree" ]; then
        test_check "$runner" "tree"              "director"        "$bindir/tree" "$TEST_TMPDIR"
    fi

    if [ -f "$bindir/find" ]; then
        test_check "$runner" "find"              "hello.txt"       "$bindir/find" "$TEST_TMPDIR" -name "hello.txt"
    fi

    if [ -f "$bindir/diff" ]; then
        test_rc    "$runner" "diff identical"    0                 "$bindir/diff" "$TEST_TMPDIR/hello.txt" "$TEST_TMPDIR/hello.txt"
    fi
}

# ── Run the selected mode ─────────────────────────────────────────
run_suite() {
    local mode="$1"
    local runner test_bin coreutils_bin busybox_bin static_bin

    # Create test fixtures in a location accessible to the runner.
    cleanup_fixtures
    setup_fixtures "$mode"

    case "$mode" in
        hl-aarch64)
            runner="run_hl"
            test_bin="$AARCH64_TEST_BIN"
            coreutils_bin="$AARCH64_COREUTILS"
            busybox_bin="$AARCH64_BUSYBOX"
            static_bin="$AARCH64_STATIC"
            ;;
        hl-x64)
            runner="run_hl"
            test_bin="$X64_TEST_BIN"
            coreutils_bin="$X64_COREUTILS"
            busybox_bin="$X64_BUSYBOX"
            static_bin=""  # no x64 static bins
            ;;
        lima-aarch64)
            runner="run_lima"
            test_bin="$AARCH64_TEST_BIN"
            coreutils_bin="$AARCH64_COREUTILS"
            busybox_bin="$AARCH64_BUSYBOX"
            static_bin="$AARCH64_STATIC"
            ;;
        lima-x64)
            runner="run_lima"
            test_bin="$X64_TEST_BIN"
            coreutils_bin="$X64_COREUTILS"
            busybox_bin="$X64_BUSYBOX"
            static_bin=""  # no x64 static bins
            ;;
        *)
            echo "Unknown mode: $mode"
            exit 1
            ;;
    esac

    printf "\n${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}\n"
    printf "${CYAN}  Testing: %s${RESET}\n" "$mode"
    printf "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}\n\n"

    pass=0; fail=0; skip=0; timeout_count=0

    printf "${BLUE}═══ Unit tests ═══${RESET}\n"
    run_unit_tests "$runner" "$test_bin"

    printf "\n${BLUE}═══ Coreutils ═══${RESET}\n"
    run_coreutils_tests "$runner" "$coreutils_bin"

    printf "\n${BLUE}═══ Busybox ═══${RESET}\n"
    run_busybox_tests "$runner" "$busybox_bin"

    if [ -n "$static_bin" ]; then
        printf "\n${BLUE}═══ Static bins ═══${RESET}\n"
        run_static_tests "$runner" "$static_bin"
    fi

    printf "\n${CYAN}━━━ %s Results: %d passed, %d failed, %d timeout, %d skipped ━━━${RESET}\n\n" \
        "$mode" "$pass" "$fail" "$timeout_count" "$skip"

    return "$fail"
}

# ── Main ──────────────────────────────────────────────────────────
total_fail=0

if [ "$MODE" = "all" ]; then
    for m in hl-aarch64 hl-x64 lima-aarch64 lima-x64; do
        run_suite "$m" || total_fail=$((total_fail + $?))
    done
    exit "$total_fail"
else
    run_suite "$MODE"
fi
