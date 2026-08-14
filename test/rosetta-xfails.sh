#!/bin/sh
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

# One source of truth for Rosetta tests whose current outcome is understood.
# Format: test name | expected outcome | reason. Expected outcomes are
# "failure" (bounded exit 1) and "timeout" (bounded runner timeout).
rosetta_xfail_entries() {
    printf '%s\n' \
        'test-signal|failure|rosetta: SA_RESETHAND not reset (also fails in Lima, 3/4 subtests pass)' \
        'test-thread|timeout|rosetta: raw clone(CLONE_THREAD) hangs (also hangs in Lima)' \
        'test-stress|timeout|rosetta: raw clone hangs (also hangs in Lima)' \
        'test-signal-thread|failure|rosetta: SA_RESETHAND not reset (also fails in Lima, 4/5 subtests pass)' \
        'test-futex-pi|timeout|rosetta: raw clone(CLONE_THREAD) in dead-owner test hangs'
}

rosetta_xfail_lookup() {
    rosetta_xfail_entries | awk -F '|' -v requested="$1" '
        $1 == requested {
            print $2 "|" $3
            found = 1
            exit
        }
        END { if (!found) exit 1 }
    '
}

rosetta_xfail_kind() {
    entry=$(rosetta_xfail_lookup "$1") || return 1
    printf '%s\n' "${entry%%|*}"
}

rosetta_xfail_reason() {
    entry=$(rosetta_xfail_lookup "$1") || return 1
    printf '%s\n' "${entry#*|}"
}
