#!/bin/sh
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

# One source of truth for mode-specific tests whose current outcome is
# understood. Format: mode | test name | expected outcome | reason. Expected
# outcomes are "rc:N" (an exact exit status) and "timeout" (the mode's bounded
# runner timeout).
matrix_xfail_entries() {
    printf '%s\n' \
        'hl-x64|test-signal|rc:1|rosetta: SA_RESETHAND is not reset (3/4 subtests pass)' \
        'hl-x64|test-signal-thread|rc:1|rosetta: SA_RESETHAND is not reset (4/5 subtests pass)' \
        'lima-x64|test-signal|rc:1|rosetta: SA_RESETHAND is not reset (3/4 subtests pass)' \
        'lima-x64|test-signal-thread|rc:1|rosetta: SA_RESETHAND is not reset (4/5 subtests pass)' \
        'lima-x64|test-clock-gettime-efault|rc:139|rosetta: clock_gettime with an invalid pointer raises SIGSEGV'
}

matrix_xfail_lookup() {
    matrix_xfail_entries | awk -F '|' -v mode="$1" -v requested="$2" '
        $1 == mode && $2 == requested {
            print $3 "|" $4
            found = 1
            exit
        }
        END { if (!found) exit 1 }
    '
}

matrix_xfail_kind() {
    matrix_entry=$(matrix_xfail_lookup "$1" "$2") || return 1
    printf '%s\n' "${matrix_entry%%|*}"
}

matrix_xfail_reason() {
    matrix_entry=$(matrix_xfail_lookup "$1" "$2") || return 1
    printf '%s\n' "${matrix_entry#*|}"
}

matrix_xfail_names() {
    matrix_xfail_entries | awk -F '|' -v mode="$1" '$1 == mode { print $2 }'
}
