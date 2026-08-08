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

# ---- 3. SHM detach reclaims page-table pages (H2) ----
# 300 attach/partial-mprotect/detach cycles exceed the 240-page pool. With
# the L3 reclaim, hl never exhausts it; without, it logs "pool exhausted".
churn_err=$("$HL" --fs-mode=legacy --audio-backend null "$BUILD/helper-shm-churn" 2>&1 >/dev/null)
if printf '%s' "$churn_err" | grep -q "page table pool exhausted"; then
    bad "SHM detach reclaims page-table pages" "pool exhausted — L3 not reclaimed"
else
    ok "SHM detach reclaims page-table pages"
fi

# ---- 4. Watchdog is OFF by default; opt-in --timeout kills a hang (H7) ----
# A guest that loops on vDSO gettimeofday makes no VM exits — the exact
# pattern the old 10s default killed. By default it must run to completion.
wd_out=$(timeout 30 "$HL" --fs-mode=legacy --audio-backend null \
             "$BUILD/helper-watchdog" 12 2>&1)
if printf '%s' "$wd_out" | grep -q "wd-done"; then
    ok "watchdog off by default: a 12s no-exit guest completes"
else
    bad "watchdog off by default: a 12s no-exit guest completes" \
        "guest was killed (watchdog armed by default?)"
fi
# Opt-in must still catch a hang: --timeout 2 kills the same 12s loop.
wd_out=$(timeout 30 "$HL" --timeout 2 --fs-mode=legacy --audio-backend null \
             "$BUILD/helper-watchdog" 12 2>&1)
if printf '%s' "$wd_out" | grep -q "wd-done"; then
    bad "--timeout arms the opt-in watchdog" "12s loop was NOT killed by --timeout 2"
elif printf '%s' "$wd_out" | grep -q "timed out after 2s"; then
    ok "--timeout arms the opt-in watchdog"
else
    bad "--timeout arms the opt-in watchdog" "unexpected: $wd_out"
fi

# ---- 5. Abstract socket names are not stealable across processes (H6) ----
absname="hl-h6-$$-$RANDOM"
# Holder binds the name and keeps it for ~5s.
"$HL" --fs-mode=legacy --audio-backend null \
    "$BUILD/helper-abstract-hold" hold "$absname" >/tmp/h6_hold.$$ 2>&1 &
hpid=$!
for _ in $(seq 1 50); do grep -q BOUND /tmp/h6_hold.$$ 2>/dev/null && break; sleep 0.1; done
if ! grep -q BOUND /tmp/h6_hold.$$ 2>/dev/null; then
    bad "a live abstract name cannot be stolen" "holder never bound"
else
    # Second process must be refused while the holder is alive.
    got=$("$HL" --fs-mode=legacy --audio-backend null \
              "$BUILD/helper-abstract-hold" try "$absname" 2>&1)
    if printf '%s' "$got" | grep -q EADDRINUSE; then
        ok "a live abstract name cannot be stolen"
    else
        bad "a live abstract name cannot be stolen" "second bind: $got (want EADDRINUSE)"
    fi
fi
wait $hpid 2>/dev/null
# (+) Once the holder is gone, the name is reclaimable.
got=$("$HL" --fs-mode=legacy --audio-backend null \
          "$BUILD/helper-abstract-hold" try "$absname" 2>&1)
if printf '%s' "$got" | grep -q BOUND; then
    ok "a freed abstract name is reclaimable"
else
    bad "a freed abstract name is reclaimable" "bind after holder exit: $got"
fi
rm -f /tmp/h6_hold.$$

# ---- 6. Rooted mutating ops cannot escape the bind (H5) ----
mdir=$(mktemp -d); mkdir -p "$mdir/bound"; echo SECRET > "$mdir/secret.txt"
"$HL" --fs-mode=rooted --bind "$mdir/bound:/home/user" --guest-cwd /home/user \
    --audio-backend null "$BUILD/helper-vfs-mutate" >/dev/null 2>&1
if [ -f "$mdir/secret.txt" ] && [ "$(cat "$mdir/secret.txt")" = SECRET ]; then
    ok "rooted mutating ops cannot escape the bind"
else
    bad "rooted mutating ops cannot escape the bind" \
        "the outside secret was modified or deleted"
fi
rm -rf "$mdir"

# ---- 7. Crash report redacts $HOME even mid-token (V26) ----
# A guest arg carrying the host home path mid-token must appear redacted in
# the crash report (the output meant for public issues), not raw.
crash_out=$(HL_TRACE_REDACT=1 timeout 20 "$HL" --timeout 2 --fs-mode=legacy \
    --audio-backend null "$BUILD/helper-crash" "--data-dir=$HOME/pandoc" 2>&1)
if printf '%s' "$crash_out" | grep -qF "$HOME/pandoc"; then
    bad "crash report redacts \$HOME mid-token" "raw host path in the report"
elif printf '%s' "$crash_out" | grep -q "cmdline"; then
    ok "crash report redacts \$HOME mid-token"
else
    bad "crash report redacts \$HOME mid-token" "no crash report produced"
fi

echo
echo "test-diagnostics: $pass passed, $fail failed — $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
[ $fail -eq 0 ]
