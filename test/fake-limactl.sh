#!/usr/bin/env bash
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

: "${FAKE_LIMACTL_LOG:?set FAKE_LIMACTL_LOG}"
: "${FAKE_LIMA_VM:?set FAKE_LIMA_VM}"

printf '%q ' "$@" >>"$FAKE_LIMACTL_LOG"
printf '\n' >>"$FAKE_LIMACTL_LOG"

if [ "${FAKE_LIMACTL_HANG:-0}" = 1 ]; then
    sleep 10
fi

[ "${1:-}" = shell ] || exit 64
shift
[ "${1:-}" = --start ] || exit 65
shift
[ "${1:-}" = "$FAKE_LIMA_VM" ] || exit 66
shift
[ "${1:-}" = -- ] || exit 67
shift

if [ "${1:-}" = sh ] && [ "${2:-}" = -c ] &&
    [ -n "${FAKE_LIMACTL_REMOTE_SCRIPT_LOG:-}" ]; then
    printf '%s\n' "${3:-}" >>"$FAKE_LIMACTL_REMOTE_SCRIPT_LOG"
fi

if [ "${1:-}" = mktemp ]; then
    case "${FAKE_LIMACTL_MKTEMP:-normal}" in
        empty)
            exit 0
            ;;
        relative)
            printf '%s\n' test-matrix.relative
            exit 0
            ;;
    esac
fi

exec "$@"
