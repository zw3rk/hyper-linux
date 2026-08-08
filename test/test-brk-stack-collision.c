/* test-brk-stack-collision.c — brk must not overlap the guest stack
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define TEST_STACK_SIZE (8UL * 1024 * 1024)
#define TEST_STACK_ALIGNMENT (2UL * 1024 * 1024)
#define TEST_PAGE_SIZE 4096UL
/* Verified high-water mark while XMMS opened GTK's file chooser. */
#define XMMS_GTK_REQUIRED_BREAK 0x0780D000UL

static int check_layout(void) {
    volatile uint8_t stack_marker = 0;
    uintptr_t stack_address = (uintptr_t)&stack_marker;
    uintptr_t stack_top = (stack_address + TEST_STACK_ALIGNMENT - 1) & ~(TEST_STACK_ALIGNMENT - 1);
    uintptr_t stack_base = stack_top - TEST_STACK_SIZE;
    uintptr_t overlapping_break = stack_base + TEST_PAGE_SIZE;
    uintptr_t initial_break = (uintptr_t)syscall(SYS_brk, 0);
    uintptr_t result;

    if (stack_base <= XMMS_GTK_REQUIRED_BREAK) {
        fprintf(stderr,
                "insufficient heap headroom: stack_base=%p "
                "required_break=%p\n",
                (void *)stack_base, (void *)XMMS_GTK_REQUIRED_BREAK);
        return 1;
    }

    result = (uintptr_t)syscall(SYS_brk, (void *)XMMS_GTK_REQUIRED_BREAK);
    if (result != XMMS_GTK_REQUIRED_BREAK) {
        fprintf(stderr,
                "brk did not reach required high-water mark: requested=%p "
                "result=%p stack_base=%p\n",
                (void *)XMMS_GTK_REQUIRED_BREAK, (void *)result, (void *)stack_base);
        return 1;
    }

    result = (uintptr_t)syscall(SYS_brk, (void *)overlapping_break);

    if (result != XMMS_GTK_REQUIRED_BREAK) {
        fprintf(stderr,
                "brk crossed stack boundary: requested=%p result=%p "
                "expected=%p stack_base=%p\n",
                (void *)overlapping_break, (void *)result, (void *)XMMS_GTK_REQUIRED_BREAK,
                (void *)stack_base);
        return 1;
    }

    (void)syscall(SYS_brk, (void *)initial_break);
    printf("brk rejected stack overlap at %p\n", (void *)stack_base);
    return 0;
}

int main(int argc, char **argv) {
    if (check_layout() != 0)
        return 1;

    if (argc == 1) {
        execl(argv[0], argv[0], "after-exec", NULL);
        perror("execl");
        return 1;
    }

    printf("brk/stack separation survived execve\n");
    return 0;
}
