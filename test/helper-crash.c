/* helper-crash.c — spin forever so hl's opt-in --timeout produces its crash
 * report. Used by test-diagnostics.sh to check the report redacts $HOME even
 * mid-token in the guest command line (V26).
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    volatile uint64_t x = 0;
    for (;;) x++;            /* no VM exit → --timeout fires the crash report */
    return 0;
}
