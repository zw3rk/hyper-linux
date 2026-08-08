#!/usr/bin/env bash
# test-diagnostics.sh — operator-facing output: redaction and SIGUSR1 stats
#
# These are host-side behaviours (what hl prints on stderr), so they cannot
# be asserted from inside the guest.
#
#  (-) HL_TRACE_REDACT was read only inside the HL_TRACE branch, so it was
#      unreachable when categories were enabled with --trace=... instead.
#  (-) The crash report — the one output meant to be pasted into a public
#      issue — ignored redaction entirely.
#  (-) A SIGUSR1 stats dump was serviced only from the syscall path, so a
#      guest that is parked and issuing no syscalls never produced one.
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
set -u

HL=${HL:-./_build/hl}
BUILD=$(dirname "$HL")
pass=0; fail=0
ok()   { printf '  %-52s OK\n'   "$1"; pass=$((pass+1)); }
bad()  { printf '  %-52s FAIL: %s\n' "$1" "$2"; fail=$((fail+1)); }

echo "test-diagnostics: redaction and SIGUSR1 stats"

# ---- 1. HL_TRACE_REDACT with --trace= (not HL_TRACE) ----
# Rooted mode binds $HOME at /home/user, so the mount trace carries the
# host home path — exactly what redaction is supposed to replace with "~".
run_trace() { "$HL" --fs-mode=rooted --audio-backend null --trace=fs \
                 "$BUILD/test-cat" /home/user/.hl-does-not-exist 2>&1 >/dev/null; }
out=$(HL_TRACE_REDACT=1 run_trace)
if [ -z "$out" ]; then
    bad "HL_TRACE_REDACT applies to --trace=" "no trace output at all"
elif printf '%s' "$out" | grep -qF "$HOME"; then
    bad "HL_TRACE_REDACT applies to --trace=" "raw \$HOME still in the trace"
else
    ok "HL_TRACE_REDACT applies to --trace="
fi

# (+) Without the variable, paths are NOT redacted — proves the check above
# is actually observing redaction rather than an absence of paths.
out=$(run_trace)
if [ -z "$out" ]; then
    bad "without it, paths are left alone" "no trace output at all"
elif printf '%s' "$out" | grep -qF "$HOME"; then
    ok "without it, paths are left alone"
else
    bad "without it, paths are left alone" "redacted without being asked"
fi

# ---- 2. SIGUSR1 dump while the guest makes no syscalls ----
tmp=$(mktemp)
HL_SYSCALL_STATS=1 "$HL" --fs-mode=legacy --audio-backend null \
    "$BUILD/helper-idle" >/dev/null 2>"$tmp" &
hlpid=$!
# Wait for the guest to reach its parked state.
for _ in $(seq 1 50); do
    grep -q "syscall_stats enabled" "$tmp" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5
kill -USR1 $hlpid 2>/dev/null
got=0
for _ in $(seq 1 30); do
    # Match the dump BANNER, not the word "SIGUSR1" — hl's own startup
    # line ("syscall_stats enabled (SIGUSR1 dumps; ...)") contains it and
    # made this check pass unconditionally.
    if grep -q "hl syscall_stats: SIGUSR1" "$tmp" 2>/dev/null; then
        got=1; break
    fi
    sleep 0.1
done
kill -KILL $hlpid 2>/dev/null; wait $hlpid 2>/dev/null
if [ "$got" = 1 ]; then
    ok "SIGUSR1 dumps stats while the guest is parked"
else
    bad "SIGUSR1 dumps stats while the guest is parked" \
        "no dump within 3s (request latched but never serviced)"
fi
rm -f "$tmp"

echo
echo "test-diagnostics: $pass passed, $fail failed — $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
[ $fail -eq 0 ]
