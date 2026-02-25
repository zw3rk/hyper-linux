/* test/hello-x86_64.S — Minimal x86_64-linux assembly hello world
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises Tier 1 syscalls: write(2) and exit(2).
 * Assemble and link as a static x86_64-linux ELF:
 *   x86_64-unknown-linux-musl-as -o hello-x86_64.o hello-x86_64.S
 *   x86_64-unknown-linux-musl-ld -T simple-x86_64.ld -o hello-x86_64 hello-x86_64.o
 */

.text
.global _start
_start:
    /* write(1, msg, 6) */
    mov $1, %rax          /* __NR_write = 1 */
    mov $1, %rdi          /* fd = stdout */
    lea msg(%rip), %rsi   /* buf = address of message */
    mov $6, %rdx          /* count = 6 bytes */
    syscall

    /* exit(0) */
    mov $60, %rax         /* __NR_exit = 60 */
    xor %rdi, %rdi        /* exit code = 0 */
    syscall

msg:
    .ascii "hello\n"
