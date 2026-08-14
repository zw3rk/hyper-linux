#!/usr/bin/env bash
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TEST_TMP=$(mktemp -d)

# Globals supplied by test-matrix.sh; initialize them here so static analysis
# can also verify this standalone regression harness.
pass=0
fail=0
timeout_count=0
xfail=0
xpass=0
CURRENT_MODE=""
export CURRENT_MODE

cleanup() {
    cleanup_fixtures 2>/dev/null || true
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT

fail_test() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_eq() {
    local expected="$1"
    local actual="$2"
    local label="$3"
    [ "$actual" = "$expected" ] ||
        fail_test "$label: expected '$expected', got '$actual'"
}

export FAKE_LIMACTL_LOG="$TEST_TMP/limactl.log"
export FAKE_LIMACTL_REMOTE_SCRIPT_LOG="$TEST_TMP/remote-script.log"
export FAKE_LIMA_VM=test-matrix-policy
export LIMACTL="$ROOT/test/fake-limactl.sh"
export LIMA_VM="$FAKE_LIMA_VM"

# Both files are libraries when sourced. The matrix main must not run here.
# shellcheck source=matrix-xfails.sh
source "$ROOT/test/matrix-xfails.sh"
# shellcheck source=test-matrix.sh
source "$ROOT/test/test-matrix.sh"

expected_x64_names=$'test-signal\ntest-signal-thread'
expected_lima_x64_names="${expected_x64_names}"$'\ntest-clock-gettime-efault'
assert_eq '' "$(matrix_xfail_names hl-aarch64)" "hl-aarch64 XFAIL names"
assert_eq '' "$(matrix_xfail_names lima-aarch64)" "lima-aarch64 XFAIL names"
assert_eq "$expected_x64_names" "$(matrix_xfail_names hl-x64)" \
    "hl-x64 XFAIL names"
assert_eq "$expected_lima_x64_names" "$(matrix_xfail_names lima-x64)" \
    "lima-x64 XFAIL names"
make_names=$(awk '$1 == "run_policy_test" { print $2 }' "$ROOT/Makefile" |
    sed 's/;//')
assert_eq "$expected_x64_names" "$make_names" "Makefile hl-x64 XFAIL call sites"
assert_eq rc:1 "$(matrix_xfail_kind hl-x64 test-signal)" "test-signal kind"
assert_eq rc:1 "$(matrix_xfail_kind lima-x64 test-signal-thread)" \
    "test-signal-thread kind"
assert_eq rc:139 "$(matrix_xfail_kind lima-x64 test-clock-gettime-efault)" \
    "test-clock-gettime-efault kind"
for mode in hl-aarch64 hl-x64 lima-aarch64 lima-x64; do
    for test_name in test-thread test-stress test-pthread test-futex-pi; do
        if matrix_xfail_kind "$mode" "$test_name" >/dev/null 2>&1; then
            fail_test "$test_name must not be an XFAIL in $mode"
        fi
    done
done
assert_eq 4 "$(grep -c 'run_test .*test-.*-efault' "$ROOT/Makefile")" \
    "Makefile focused EFAULT unit lanes"
assert_eq 2 "$(grep -c 'test_check .*test-.*-efault' \
    "$ROOT/test/test-matrix.sh")" "matrix focused EFAULT unit lane"
assert_eq 120 "$(matrix_haskell_timeout hl-aarch64)" \
    "aarch64 Haskell timeout"
assert_eq 120 "$(matrix_haskell_timeout hl-x64)" "hl-x64 Haskell timeout"
assert_eq 120 "$(matrix_haskell_timeout lima-x64)" "lima-x64 Haskell timeout"

runner_ok() {
    printf '%s\n' "${1:-0 failed}"
}

runner_fail() {
    printf '%s\n' "${1:-FAIL}"
    return 1
}

runner_timeout() {
    return 124
}

runner_crash() {
    return 139
}

runner_guest_timeout() {
    return 125
}

runner_capture() {
    printf '%s\n' "$*" >"$TEST_TMP/runner-args"
    printf '%s\n' '0 failed'
}

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_fail test-signal "$SIGNAL_SUCCESS_PATTERN" ignored >"$TEST_TMP/result"
assert_eq 1 "$xfail" "known signal failure classification"
assert_eq 0 "$fail" "known signal failure count"

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_crash test-signal "$SIGNAL_SUCCESS_PATTERN" >"$TEST_TMP/result"
assert_eq 0 "$xfail" "unexpected signal crash XFAIL count"
assert_eq 1 "$fail" "unexpected signal crash failure count"

matrix_reset_counts
CURRENT_MODE=lima-x64
test_check runner_ok test-uname-efault '0 failed' '0 failed' \
    >"$TEST_TMP/result"
assert_eq 1 "$pass" "Lima uname EFAULT normal pass"
assert_eq 0 "$xfail" "Lima uname EFAULT XFAIL count"

matrix_reset_counts
CURRENT_MODE=lima-x64
test_check runner_crash test-clock-gettime-efault '0 failed' \
    >"$TEST_TMP/result"
assert_eq 1 "$xfail" "known Lima clock_gettime SIGSEGV classification"
assert_eq 0 "$fail" "known Lima clock_gettime SIGSEGV failure count"

matrix_reset_counts
CURRENT_MODE=lima-x64
test_check runner_crash test-uname-efault '0 failed' >"$TEST_TMP/result"
assert_eq 0 "$xfail" "Lima uname crash XFAIL count"
assert_eq 1 "$fail" "Lima uname crash failure count"

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_ok test-signal "$SIGNAL_SUCCESS_PATTERN" \
    'test-signal: all tests passed — PASS' >"$TEST_TMP/result"
assert_eq 1 "$xpass" "XPASS count"
assert_eq 1 "$fail" "XPASS failure count"
if matrix_result; then
    fail_test "XPASS must make the matrix result nonzero"
fi

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_ok test-thread '0 failed' '0 failed' >"$TEST_TMP/result"
test_check runner_ok test-stress '0 failed' '0 failed' >"$TEST_TMP/result"
assert_eq 2 "$pass" "fixed clone/stress normal pass count"
assert_eq 0 "$xfail" "fixed clone/stress XFAIL count"

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_ok test-pthread '0 failed' '0 failed' >"$TEST_TMP/result"
assert_eq 1 "$pass" "test-pthread normal pass"
assert_eq 0 "$xfail" "test-pthread XFAIL count"

matrix_reset_counts
CURRENT_MODE=hl-x64
test_check runner_ok test-futex-pi '0 failed' '0 failed' >"$TEST_TMP/result"
assert_eq 1 "$pass" "test-futex-pi normal pass"
assert_eq 0 "$xfail" "test-futex-pi XFAIL count"

matrix_reset_counts
CURRENT_MODE=lima-aarch64
run_futex_pi_test runner_capture /guest/bin >"$TEST_TMP/result"
assert_eq '/guest/bin/test-futex-pi --linux-reference' \
    "$(cat "$TEST_TMP/runner-args")" "Lima futex-pi arguments"
assert_eq 1 "$pass" "Lima futex-pi normal pass"

matrix_reset_counts
CURRENT_MODE=hl-aarch64
run_futex_pi_test runner_capture /guest/bin >"$TEST_TMP/result"
assert_eq '/guest/bin/test-futex-pi' "$(cat "$TEST_TMP/runner-args")" \
    "hl futex-pi arguments"
assert_eq 1 "$pass" "hl futex-pi normal pass"

matrix_reset_counts
CURRENT_MODE=hl-aarch64
test_check runner_ok test-signal "$SIGNAL_SUCCESS_PATTERN" \
    'test-signal: all tests passed — PASS' >"$TEST_TMP/result"
assert_eq 1 "$pass" "aarch64 executes known Rosetta XFAIL"
assert_eq 0 "$xfail" "aarch64 XFAIL count"

matrix_reset_counts
CURRENT_MODE=hl-aarch64
test_check runner_ok test-signal "$SIGNAL_SUCCESS_PATTERN" PASS >"$TEST_TMP/result"
assert_eq 1 "$fail" "partial signal output must fail"

matrix_reset_counts
CURRENT_MODE=hl-aarch64
test_check runner_fail ordinary-test expected expected >"$TEST_TMP/result"
assert_eq 1 "$fail" "matching output with nonzero rc must fail"

matrix_reset_counts
CURRENT_MODE=hl-aarch64
test_check runner_timeout ordinary-test expected >"$TEST_TMP/result"
assert_eq 1 "$timeout_count" "unexpected timeout count"
if matrix_result; then
    fail_test "unexpected timeout must make the matrix result nonzero"
fi

setup_fixtures lima-x64
case "$TEST_TMPDIR" in
    /tmp/test-matrix.*) ;;
    *) fail_test "Lima fixture path is not validated: $TEST_TMPDIR" ;;
esac
[ "$(cat "$TEST_TMPDIR/hello.txt")" = 'hello world' ] ||
    fail_test "Lima fixture population was not verified"
cleanup_fixtures
if grep -Ev '^shell --start test-matrix-policy -- ' "$FAKE_LIMACTL_LOG" |
    grep -q .; then
    fail_test "a Lima call omitted --start"
fi

export FAKE_LIMACTL_MKTEMP=empty
if setup_fixtures lima-x64 2>/dev/null; then
    fail_test "empty Lima mktemp output must fail"
fi
export FAKE_LIMACTL_MKTEMP=relative
if setup_fixtures lima-x64 2>/dev/null; then
    fail_test "relative Lima mktemp output must fail"
fi
unset FAKE_LIMACTL_MKTEMP

if MATRIX_CASE_TIMEOUT=1 LIMA_TRANSPORT_TIMEOUT=4 \
    run_lima sh -c 'exec sleep 5'; then
    fail_test "inner Lima timeout unexpectedly passed"
else
    rc=$?
fi
assert_eq 125 "$rc" "inner guest timeout status"
if ! grep -Fq "timeout \"\$guest_timeout\" \"\$@\"" \
    "$FAKE_LIMACTL_REMOTE_SCRIPT_LOG"; then
    fail_test "guest command must use plain timeout"
fi
if grep -Fq 'timeout --kill-after' "$FAKE_LIMACTL_REMOTE_SCRIPT_LOG"; then
    fail_test "guest command must not use timeout --kill-after"
fi

export FAKE_LIMACTL_HANG=1
if MATRIX_CASE_TIMEOUT=1 LIMA_TRANSPORT_TIMEOUT=2 run_lima true; then
    fail_test "outer Lima timeout unexpectedly passed"
else
    rc=$?
fi
unset FAKE_LIMACTL_HANG
assert_eq 124 "$rc" "outer transport timeout status"

printf 'ok: matrix policy and Lima runner contracts\n'
