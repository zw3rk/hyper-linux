/* hello-dynamic.c — Dynamically-linked test program for hl --sysroot
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This program is compiled WITHOUT -static, producing a dynamically-linked
 * ELF that requires ld-musl-aarch64.so.1 and libc.so at runtime.  It
 * exercises printf (PLT call through libc.so) and argv processing.
 */

#include <stdio.h>

int main(int argc, char **argv) {
    printf("Hello from dynamic musl! argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
