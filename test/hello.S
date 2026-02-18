/* test/hello.S — Minimal aarch64-linux assembly hello world
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises Tier 1 syscalls: write(2) and exit(2).
 * Assemble and link as a static aarch64-linux ELF:
 *   aarch64-unknown-linux-musl-as -o hello.o hello.S
 *   aarch64-unknown-linux-musl-ld -T simple.ld -o hello hello.o
 */

.text
.global _start
_start:
    /* write(1, msg, 6) */
    mov x0, #1          /* fd = stdout */
    adr x1, msg         /* buf = address of message */
    mov x2, #6          /* count = 6 bytes */
    mov x8, #64         /* __NR_write = 64 */
    svc #0

    /* exit(0) */
    mov x0, #0          /* exit code = 0 */
    mov x8, #93         /* __NR_exit = 93 */
    svc #0

msg:
    .ascii "hello\n"
